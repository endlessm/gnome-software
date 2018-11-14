/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2016-2018 Endless Mobile, Inc
 *
 * Authors:
 *   Joaquim Rocha <jrocha@endlessm.com>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <config.h>

#include <ostree.h>
#include <gnome-software.h>
#include <glib/gi18n.h>
#include <gs-plugin.h>
#include <gs-utils.h>
#include <libsoup/soup.h>

#define ENDLESS_ID_PREFIX "com.endlessm."
#define METADATA_SYS_DESKTOP_FILE "EndlessOS::system-desktop-file"
#define METADATA_REPLACED_BY_DESKTOP_FILE "EndlessOS::replaced-by-desktop-file"
#define EOS_PROXY_APP_PREFIX ENDLESS_ID_PREFIX "proxy"

/*
 * SECTION:
 * Plugin to improve GNOME Software integration in the EOS desktop.
 */

static gboolean app_is_flatpak (GsApp *app);

struct GsPluginData
{
	GDBusConnection *session_bus;
	GHashTable *desktop_apps;

	/* This hash table is for "replacement apps" for placeholders
	 * on the desktop. We are shipping systems with icons like
	 * "Get VLC" or "Get Spotify", which, when launched, open
	 * the app center. In any case where the user could install
	 * those apps, we want to ensure that we replace the icon
	 * on the desktop with the application's icon, in the same
	 * place. */
	GHashTable *replacement_app_lookup;
	int applications_changed_id;
	SoupSession *soup_session;
};

static gboolean
get_applications_with_shortcuts (GsPlugin	*plugin,
				 GHashTable    **apps_table,
				 GCancellable	*cancellable,
				 GError		**error) {
	g_autoptr (GVariantIter) iter = NULL;
	g_autoptr (GVariant) apps = NULL;
	gchar *application;
	GsPluginData *priv = gs_plugin_get_data (plugin);

	*apps_table = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
					     NULL);

	apps = g_dbus_connection_call_sync (priv->session_bus,
					    "org.gnome.Shell",
					    "/org/gnome/Shell",
					    "org.gnome.Shell.AppStore",
					    "ListApplications",
					    NULL, NULL,
					    G_DBUS_CALL_FLAGS_NONE,
					    -1,
					    cancellable,
					    error);
	if (apps == NULL)
		return FALSE;

	g_variant_get (apps, "(as)", &iter);
	while (g_variant_iter_loop (iter, "s", &application))
		g_hash_table_add (*apps_table, g_strdup (application));

	return TRUE;
}

static void
on_desktop_apps_changed (GDBusConnection *connection,
			 const gchar	 *sender_name,
			 const gchar	 *object_path,
			 const gchar	 *interface_name,
			 const gchar	 *signal_name,
			 GVariant	 *parameters,
			 GsPlugin	 *plugin)
{
	g_autoptr(GHashTable) apps = NULL;
	GHashTableIter iter;
	gpointer key, value;
	GsPluginData *priv = gs_plugin_get_data (plugin);
	g_autoptr(GError) error = NULL;

	if (!get_applications_with_shortcuts (plugin, &apps, NULL, &error)) {
		g_warning ("Error getting apps with shortcuts: %s",
			   error->message);
		return;
	}

	/* remove any apps that no longer have shortcuts */
	g_hash_table_iter_init (&iter, priv->desktop_apps);
	while (g_hash_table_iter_next (&iter, &key, &value)) {
		GsApp *app = NULL;

		/* remove the key (if it exists) so we don't have to deal with
		 * it again in the next loop */
		if (g_hash_table_remove (apps, key))
			continue;

		app = gs_plugin_cache_lookup (plugin, key);
		if (app)
			gs_app_remove_quirk (app, GS_APP_QUIRK_HAS_SHORTCUT);

		g_hash_table_iter_remove (&iter);
	}

	/* add any apps that have shortcuts now */
	g_hash_table_iter_init (&iter, apps);
	while (g_hash_table_iter_next (&iter, &key, &value)) {
		GsApp *app = gs_plugin_cache_lookup (plugin, key);

		if (app)
			gs_app_add_quirk (app, GS_APP_QUIRK_HAS_SHORTCUT);

		g_hash_table_add (priv->desktop_apps, g_strdup (key));
	}
}

static void
read_icon_replacement_overrides (GHashTable *replacement_app_lookup)
{
	const gchar * const *datadirs = g_get_system_data_dirs ();
	g_autoptr(GError) error = NULL;

	for (; *datadirs; ++datadirs) {
		g_autofree gchar *candidate_path = g_build_filename (*datadirs,
                                                                     "eos-application-tools",
                                                                     "icon-overrides",
                                                                     "eos-icon-overrides.ini",
                                                                     NULL);
		g_autoptr(GKeyFile) config = g_key_file_new ();
		g_auto(GStrv) keys = NULL;
		gsize n_keys = 0;
		gsize key_iterator = 0;

		if (!g_key_file_load_from_file (config, candidate_path, G_KEY_FILE_NONE, &error)) {
			if (!g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
				g_warning ("Could not load icon overrides file %s: %s", candidate_path, error->message);
			g_clear_error (&error);
			continue;
		}

		if (!(keys = g_key_file_get_keys (config, "Overrides", &n_keys, &error))) {
			g_warning ("Could not read keys from icon overrides file %s: %s", candidate_path, error->message);
			g_clear_error (&error);
			continue;
		}

		/* Now add all the key-value pairs to the replacement app lookup table */
		for (; key_iterator != n_keys; ++key_iterator) {
			g_hash_table_replace (replacement_app_lookup,
					      g_strdup (keys[key_iterator]),
					      g_key_file_get_string (config,
								     "Overrides",
								     keys[key_iterator],
								     NULL));
		}

		/* First one takes priority, ignore the others */
		break;
	}
}

gboolean
gs_plugin_setup (GsPlugin *plugin,
		 GCancellable *cancellable,
		 GError **error)
{
	g_autoptr(GError) local_error = NULL;
	GsPluginData *priv = gs_plugin_get_data (plugin);

	priv->session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, cancellable, error);
	if (priv->session_bus == NULL)
		return FALSE;

	if (!get_applications_with_shortcuts (plugin, &priv->desktop_apps,
					      cancellable, &local_error))
		g_warning ("Couldn't get the apps with shortcuts: %s",
			   local_error->message);

	priv->applications_changed_id =
		g_dbus_connection_signal_subscribe (priv->session_bus,
						    "org.gnome.Shell",
						    "org.gnome.Shell.AppStore",
						    "ApplicationsChanged",
						    "/org/gnome/Shell",
						    NULL,
						    G_DBUS_SIGNAL_FLAGS_NONE,
						    (GDBusSignalCallback) on_desktop_apps_changed,
						    plugin, NULL);
	priv->soup_session = gs_plugin_get_soup_session (plugin);

	priv->replacement_app_lookup = g_hash_table_new_full (g_str_hash, g_str_equal,
							      g_free, g_free);

	/* Synchronous, but this guarantees that the lookup table will be
	 * there when we call ReplaceApplication later on */
	read_icon_replacement_overrides (priv->replacement_app_lookup);

	return TRUE;
}

void
gs_plugin_initialize (GsPlugin *plugin)
{
	gs_plugin_alloc_data (plugin, sizeof(GsPluginData));

	/* let the flatpak plugin run first so we deal with the apps
	 * in a more complete/refined state */
	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_AFTER, "flatpak");

	/* we already deal with apps that need to be proxied, so let's impede
	 * the other plugin from running */
	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_CONFLICTS, "generic-updates");
}

void
gs_plugin_destroy (GsPlugin *plugin)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);

	if (priv->applications_changed_id != 0) {
		g_dbus_connection_signal_unsubscribe (priv->session_bus,
						      priv->applications_changed_id);
		priv->applications_changed_id = 0;
	}

	g_hash_table_destroy (priv->desktop_apps);
	g_hash_table_destroy (priv->replacement_app_lookup);
}

/* Copy of the implementation of gs_flatpak_app_get_ref_name(). */
static const gchar *
app_get_flatpak_ref_name (GsApp *app)
{
	return gs_app_get_metadata_item (app, "flatpak::RefName");
}

static char *
get_desktop_file_id (GsApp *app)
{
	const char *desktop_file_id =
		gs_app_get_metadata_item (app, METADATA_SYS_DESKTOP_FILE);

	if (!desktop_file_id) {
		if (app_is_flatpak (app)) {
			/* ensure we add the .desktop suffix to the app's ref name
			 * since in Flatpak the app ID can have that suffix already
			 * or not, depending on how the appdata has been generated */
			const char *ref_name = app_get_flatpak_ref_name (app);
			return g_strconcat (ref_name, ".desktop", NULL);
		}

		/* just use the app ID if this is not a Flatpak app */
		desktop_file_id = gs_app_get_id (app);
	}

	g_assert (desktop_file_id != NULL);
	return g_strdup (desktop_file_id);
}

static void
gs_plugin_eos_update_app_shortcuts_info (GsPlugin *plugin,
					 GsApp *app)
{
	GsPluginData *priv = NULL;
	g_autofree char *desktop_file_id = NULL;
	g_autofree char *kde_desktop_file_id = NULL;

	if (!gs_app_is_installed (app)) {
		gs_app_remove_quirk (app, GS_APP_QUIRK_HAS_SHORTCUT);
		return;
	}

	priv = gs_plugin_get_data (plugin);
	desktop_file_id = get_desktop_file_id (app);
	kde_desktop_file_id =
		g_strdup_printf ("%s-%s", "kde4", desktop_file_id);

	/* Cache both keys, since we may see either variant in the desktop
	 * grid; see on_desktop_apps_changed().
	 */
	gs_plugin_cache_add (plugin, desktop_file_id, app);
	gs_plugin_cache_add (plugin, kde_desktop_file_id, app);

	if (g_hash_table_lookup (priv->desktop_apps, desktop_file_id) ||
	    g_hash_table_lookup (priv->desktop_apps, kde_desktop_file_id))
		gs_app_add_quirk (app, GS_APP_QUIRK_HAS_SHORTCUT);
	else
		gs_app_remove_quirk (app, GS_APP_QUIRK_HAS_SHORTCUT);
}

static gboolean
app_is_flatpak (GsApp *app)
{
	return gs_app_get_bundle_kind (app) == AS_BUNDLE_KIND_FLATPAK;
}

void
gs_plugin_adopt_app (GsPlugin *plugin, GsApp *app)
{
	if (app_is_flatpak (app))
		return;

	gs_app_set_management_plugin (app, gs_plugin_get_name (plugin));
}

static void
gs_plugin_eos_refine_core_app (GsApp *app)
{
	if (app_is_flatpak (app) ||
	    (gs_app_get_scope (app) == AS_APP_SCOPE_UNKNOWN))
		return;

	if (gs_app_get_kind (app) == AS_APP_KIND_OS_UPGRADE)
		return;

	/* we only allow to remove flatpak apps */
	gs_app_add_quirk (app, GS_APP_QUIRK_COMPULSORY);

	if (!gs_app_is_installed (app)) {
		/* forcibly set the installed state */
		gs_app_set_state (app, AS_APP_STATE_UNKNOWN);
		gs_app_set_state (app, AS_APP_STATE_INSTALLED);
	}
}

typedef struct
{
	GsApp *app;
	GsPlugin *plugin;
	char *cache_filename;
} PopularBackgroundRequestData;

static void
popular_background_image_tile_request_data_destroy (PopularBackgroundRequestData *data)
{
	g_clear_object (&data->app);
	g_free (data->cache_filename);
	g_free (data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (PopularBackgroundRequestData,
                               popular_background_image_tile_request_data_destroy)

static void
gs_plugin_eos_update_tile_image_from_filename (GsApp      *app,
                                               const char *filename)
{
	g_autofree char *css = g_strdup_printf ("background-image: url('%s')",
	                                       filename);
	gs_app_set_metadata (app, "GnomeSoftware::BackgroundTile-css", css);
}

static void
gs_plugin_eos_tile_image_downloaded_cb (SoupSession *session,
                                        SoupMessage *msg,
                                        gpointer user_data)
{
	g_autoptr(PopularBackgroundRequestData) data = user_data;
	g_autoptr(GError) error = NULL;

	if (msg->status_code == SOUP_STATUS_CANCELLED)
		return;

	if (msg->status_code != SOUP_STATUS_OK) {
		g_debug ("Failed to download tile image corresponding to cache entry %s: %s",
		         data->cache_filename,
		         msg->reason_phrase);
		return;
	}

	/* Write out the cache image to disk */
	if (!g_file_set_contents (data->cache_filename,
	                          msg->response_body->data,
	                          msg->response_body->length,
	                          &error)) {
		g_debug ("Failed to write cache image %s, %s",
		         data->cache_filename,
		         error->message);
		return;
	}

	gs_plugin_eos_update_tile_image_from_filename (data->app, data->cache_filename);
}

static void
gs_plugin_eos_refine_popular_app (GsPlugin *plugin,
				  GsApp *app)
{
	const char *popular_bg = NULL;
	g_autofree char *tile_cache_hash = NULL;
	g_autofree char *cache_filename = NULL;
	g_autofree char *writable_cache_filename = NULL;
	g_autofree char *url_basename = NULL;
	g_autofree char *cache_identifier = NULL;
	GsPluginData *priv = gs_plugin_get_data (plugin);
	PopularBackgroundRequestData *request_data = NULL;
	g_autoptr(SoupURI) soup_uri = NULL;
	g_autoptr(SoupMessage) message = NULL;

	if (gs_app_get_metadata_item (app, "GnomeSoftware::BackgroundTile-css"))
		return;

	popular_bg =
	   gs_app_get_metadata_item (app, "GnomeSoftware::popular-background");

	if (!popular_bg)
		return;

	url_basename = g_path_get_basename (popular_bg);

	/* First take a hash of this URL and see if it is in our cache */
	tile_cache_hash = g_compute_checksum_for_string (G_CHECKSUM_SHA256,
	                                                 popular_bg,
	                                                 -1);
	cache_identifier = g_strdup_printf ("%s-%s", tile_cache_hash, url_basename);
	cache_filename = gs_utils_get_cache_filename ("eos-popular-app-thumbnails",
	                                              cache_identifier,
	                                              GS_UTILS_CACHE_FLAG_NONE,
	                                              NULL);

	/* Check to see if the file exists in the cache at the time we called this
	 * function. If it does, then change the css so that the tile loads. Otherwise,
	 * we'll need to asynchronously fetch the image from the server and write it
	 * to the cache */
	if (g_file_test (cache_filename, G_FILE_TEST_EXISTS)) {
		g_debug ("Hit cache for thumbnail %s: %s", popular_bg, cache_filename);
		gs_plugin_eos_update_tile_image_from_filename (app, cache_filename);
		return;
	}

	writable_cache_filename = gs_utils_get_cache_filename ("eos-popular-app-thumbnails",
	                                                       cache_identifier,
	                                                       GS_UTILS_CACHE_FLAG_WRITEABLE,
	                                                       NULL);

	soup_uri = soup_uri_new (popular_bg);
	g_debug ("Downloading thumbnail %s to %s", popular_bg, writable_cache_filename);
	if (!soup_uri || !SOUP_URI_VALID_FOR_HTTP (soup_uri)) {
		g_debug ("Couldn't download %s, URL is not valid", popular_bg);
		return;
	}

	/* XXX: Note that we might have multiple downloads in progress here. We
	 * don't make any attempt to keep track of this. */
	message = soup_message_new_from_uri (SOUP_METHOD_GET, soup_uri);
	if (!message) {
		g_debug ("Couldn't download %s, network not available", popular_bg);
		return;
	}

	request_data = g_new0 (PopularBackgroundRequestData, 1);
	request_data->app = g_object_ref (app);
	request_data->plugin = plugin;
	request_data->cache_filename = g_steal_pointer (&writable_cache_filename);

	soup_session_queue_message (priv->soup_session,
	                            g_steal_pointer (&message),
	                            gs_plugin_eos_tile_image_downloaded_cb,
	                            request_data);
}

gboolean
gs_plugin_refine (GsPlugin		*plugin,
		  GsAppList		*list,
		  GsPluginRefineFlags	flags,
		  GCancellable		*cancellable,
		  GError		**error)
{
	for (guint i = 0; i < gs_app_list_length (list); ++i) {
		GsApp *app = gs_app_list_index (list, i);

		gs_plugin_eos_refine_core_app (app);

		if (gs_app_get_kind (app) != AS_APP_KIND_DESKTOP)
			continue;

		gs_plugin_eos_update_app_shortcuts_info (plugin, app);

		gs_plugin_eos_refine_popular_app (plugin, app);
	}

	return TRUE;
}

static gboolean
remove_app_from_shell (GsPlugin		*plugin,
		       GsApp		*app,
		       GCancellable	*cancellable,
		       GError		**error_out)
{
	GError *error = NULL;
	GsPluginData *priv = gs_plugin_get_data (plugin);
	g_autofree char *desktop_file_id = get_desktop_file_id (app);

	g_dbus_connection_call_sync (priv->session_bus,
				     "org.gnome.Shell",
				     "/org/gnome/Shell",
				     "org.gnome.Shell.AppStore",
				     "RemoveApplication",
				     g_variant_new ("(s)", desktop_file_id),
				     NULL,
				     G_DBUS_CALL_FLAGS_NONE,
				     -1,
				     cancellable,
				     &error);

	if (error != NULL) {
		g_debug ("Error removing app from shell: %s", error->message);
		g_propagate_error (error_out, error);
		return FALSE;
	}

	return TRUE;
}

static GVariant *
shell_add_app_if_not_visible (GDBusConnection *session_bus,
			      const gchar *shortcut_id,
			      GCancellable *cancellable,
			      GError **error)
{
	return g_dbus_connection_call_sync (session_bus,
					    "org.gnome.Shell",
					    "/org/gnome/Shell",
					    "org.gnome.Shell.AppStore",
					    "AddAppIfNotVisible",
					    g_variant_new ("(s)", shortcut_id),
					    NULL,
					    G_DBUS_CALL_FLAGS_NONE,
					    -1,
					    cancellable,
					    error);
}

static GVariant *
shell_replace_app (GDBusConnection *session_bus,
		   const char *original_shortcut_id,
		   const char *replacement_shortcut_id,
		   GCancellable *cancellable,
		   GError **error)
{
	return g_dbus_connection_call_sync (session_bus,
					    "org.gnome.Shell",
					    "/org/gnome/Shell",
					    "org.gnome.Shell.AppStore",
					    "ReplaceApplication",
					    g_variant_new ("(ss)",
					                   original_shortcut_id,
					                   replacement_shortcut_id),
					    NULL,
					    G_DBUS_CALL_FLAGS_NONE,
					    -1,
					    cancellable,
					    error);
}

static gboolean
add_app_to_shell (GsPlugin	*plugin,
		  GsApp		*app,
		  GCancellable	*cancellable,
		  GError	**error_out)
{
	GError *error = NULL;
	GsPluginData *priv = gs_plugin_get_data (plugin);
	g_autofree char *desktop_file_id = get_desktop_file_id (app);

	/* Look up the app in our replacement list to see if we
	 * can replace an existing shortcut, and if so, do that
	 * instead */
	const char *shortcut_id_to_replace = g_hash_table_lookup (priv->replacement_app_lookup,
								  desktop_file_id);


	if (shortcut_id_to_replace)
		shell_replace_app (priv->session_bus,
				   shortcut_id_to_replace,
				   desktop_file_id,
				   cancellable,
				   &error);
	else
		shell_add_app_if_not_visible (priv->session_bus,
					      desktop_file_id,
					      cancellable,
					      &error);

	if (error != NULL) {
		g_debug ("Error adding app to shell: %s", error->message);
		g_propagate_error (error_out, error);
		return FALSE;
	}

	return TRUE;
}

gboolean
gs_plugin_add_shortcut (GsPlugin	*plugin,
			GsApp		*app,
			GCancellable	*cancellable,
			GError		**error)
{
	gs_app_add_quirk (app, GS_APP_QUIRK_HAS_SHORTCUT);
	return add_app_to_shell (plugin, app, cancellable, error);
}

gboolean
gs_plugin_remove_shortcut (GsPlugin	*plugin,
			   GsApp	*app,
			   GCancellable	*cancellable,
			   GError	**error)
{
	gs_app_remove_quirk (app, GS_APP_QUIRK_HAS_SHORTCUT);
	return remove_app_from_shell (plugin, app, cancellable, error);
}

gboolean
gs_plugin_app_install (GsPlugin *plugin,
		       GsApp *app,
		       GCancellable *cancellable,
		       GError **error)
{
	g_autoptr(GError) local_error = NULL;
	if (!app_is_flatpak (app))
		return TRUE;

	/* We're only interested in already installed flatpak apps so we can
	 * add them to the desktop */
	if (gs_app_get_state (app) != AS_APP_STATE_INSTALLED)
		return TRUE;

	if (!add_app_to_shell (plugin, app, cancellable, &local_error)) {
		g_warning ("Failed to add shortcut: %s",
			   local_error->message);
	}
	return TRUE;
}

static gboolean
launch_with_sys_desktop_file (GsApp *app,
                              GError **error)
{
	GdkDisplay *display;
	g_autoptr(GAppLaunchContext) context = NULL;
	g_autofree char *desktop_file_id = get_desktop_file_id (app);
	g_autoptr(GDesktopAppInfo) app_info =
		gs_utils_get_desktop_app_info (desktop_file_id);
	g_autoptr(GError) local_error = NULL;
	gboolean ret;

	display = gdk_display_get_default ();
	context = G_APP_LAUNCH_CONTEXT (gdk_display_get_app_launch_context (display));
	ret = g_app_info_launch (G_APP_INFO (app_info), NULL, context, &local_error);

	if (!ret) {
		g_warning ("Could not launch %s: %s", gs_app_get_unique_id (app),
			   local_error->message);
		g_set_error (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_FAILED,
			     _("Could not launch this application."));
	}

	return ret;
}

gboolean
gs_plugin_app_remove (GsPlugin *plugin,
		      GsApp *app,
		      GCancellable *cancellable,
		      GError **error)
{
	g_autoptr(GError) local_error = NULL;
	if (!app_is_flatpak (app))
		return TRUE;

	/* We're only interested in apps that have been successfully uninstalled */
	if (gs_app_is_installed (app))
		return TRUE;

	if (!remove_app_from_shell (plugin, app, cancellable, &local_error)) {
		g_warning ("Failed to remove shortcut: %s",
			   local_error->message);
	}
	return TRUE;
}

gboolean
gs_plugin_launch (GsPlugin *plugin,
		  GsApp *app,
		  GCancellable *cancellable,
		  GError **error)
{
	/* if the app is one of the system ones, we simply launch it through the
	 * plugin's app launcher */
	if (gs_app_has_quirk (app, GS_APP_QUIRK_COMPULSORY) &&
	    !app_is_flatpak (app))
		return gs_plugin_app_launch (plugin, app, error);

	/* for apps that have a special desktop file (e.g. Google Chrome) */
	if (gs_app_get_metadata_item (app, METADATA_SYS_DESKTOP_FILE))
		return launch_with_sys_desktop_file (app, error);

	return TRUE;
}

static GsApp *
gs_plugin_eos_create_proxy_app (GsPlugin *plugin,
				const char *id,
				const char *name,
				const char *summary)
{
	GsApp *proxy = gs_app_new (id);
	g_autoptr(AsIcon) icon;

	gs_app_set_scope (proxy, AS_APP_SCOPE_SYSTEM);
	gs_app_set_kind (proxy, AS_APP_KIND_RUNTIME);
	gs_app_set_name (proxy, GS_APP_QUALITY_NORMAL, name);
	gs_app_set_summary (proxy, GS_APP_QUALITY_NORMAL, summary);
	gs_app_set_state (proxy, AS_APP_STATE_UPDATABLE_LIVE);
	gs_app_add_quirk (proxy, GS_APP_QUIRK_IS_PROXY);
	gs_app_set_management_plugin (proxy, gs_plugin_get_name (plugin));

	icon = as_icon_new ();
	as_icon_set_kind (icon, AS_ICON_KIND_STOCK);
	as_icon_set_name (icon, "system-run-symbolic");
	gs_app_add_icon (proxy, icon);

	return proxy;
}

static void
process_proxy_updates (GsPlugin *plugin,
		       GsAppList *list,
		       GsApp *proxy_app,
		       const char **proxied_apps)
{
	g_autoptr(GSList) proxied_updates = NULL;

	for (guint i = 0; i < gs_app_list_length (list); ++i) {
		GsApp *app = gs_app_list_index (list, i);
		const char *id = gs_app_get_id (app);

		if (!g_strv_contains (proxied_apps, id) ||
		    gs_app_get_scope (proxy_app) != gs_app_get_scope (app))
			continue;

		proxied_updates = g_slist_prepend (proxied_updates, app);
	}

	if (!proxied_updates)
		return;

	for (GSList *iter = proxied_updates; iter; iter = g_slist_next (iter)) {
		GsApp *app = GS_APP (iter->data);
		gs_app_add_related (proxy_app, app);
		/* remove proxied apps from updates list since they will be
		 * updated from the proxy app */
		gs_app_list_remove (list, app);
	}
	gs_app_list_add (list, proxy_app);
}

static gboolean
add_updates (GsPlugin *plugin,
	     GsAppList *list,
	     GCancellable *cancellable,
	     GError **error)
{
	g_autoptr(GsApp) framework_proxy_app =
		gs_plugin_eos_create_proxy_app (plugin,
						EOS_PROXY_APP_PREFIX ".EOSUpdatesProxy",
						/* TRANSLATORS: this is the name of the Endless Platform app */
						_("Endless Platform"),
						/* TRANSLATORS: this is the summary of the Endless Platform app */
						_("Framework for applications"));
	const char *framework_proxied_apps[] = {"com.endlessm.Platform",
						"com.endlessm.apps.Platform",
						"com.endlessm.CompanionAppService.desktop",
						"com.endlessm.EknServicesMultiplexer.desktop",
						"com.endlessm.quote_of_the_day.en.desktop",
						"com.endlessm.word_of_the_day.en.desktop",
						NULL};
	process_proxy_updates (plugin, list,
			       framework_proxy_app,
			       framework_proxied_apps);

	return TRUE;
}

gboolean
gs_plugin_add_updates (GsPlugin *plugin,
		       GsAppList *list,
		       GCancellable *cancellable,
		       GError **error)
{
	return add_updates (plugin, list, cancellable, error);
}

static char *
get_os_collection_id (void)
{
	OstreeDeployment *booted_deployment;
	GKeyFile *origin;
	g_autofree char *refspec = NULL;
	g_autofree char *remote = NULL;
	g_autofree char *collection_id = NULL;
	g_autoptr(OstreeRepo) repo = NULL;
	g_autoptr(OstreeSysroot) sysroot = NULL;
	g_autoptr(GError) error = NULL;

	sysroot = ostree_sysroot_new_default ();
	if (!ostree_sysroot_load (sysroot, NULL, &error))
		goto err_out;

	booted_deployment = ostree_sysroot_get_booted_deployment (sysroot);
	if (booted_deployment == NULL)
		return NULL;

	origin = ostree_deployment_get_origin (booted_deployment);
	if (origin == NULL)
		return NULL;

	refspec = g_key_file_get_string (origin, "origin", "refspec", &error);
	if (refspec == NULL)
		goto err_out;

	ostree_parse_refspec (refspec, &remote, NULL, &error);
	if (remote == NULL)
		goto err_out;

	repo = ostree_repo_new_default ();
	if (!ostree_repo_open (repo, NULL, &error))
		goto err_out;

	if (!ostree_repo_get_remote_option (repo, remote, "collection-id", NULL, &collection_id, &error))
		goto err_out;

	return g_steal_pointer (&collection_id);

err_out:
	if (error != NULL)
		g_debug ("failed to get OSTree collection ID: %s", error->message);
	return NULL;
}

gboolean
gs_plugin_os_get_copyable (GsPlugin *plugin,
			   GFile *copy_dest,
			   gboolean *copyable,
			   GCancellable *cancellable,
			   GError **error)
{
	g_autofree char *collection_id = get_os_collection_id ();

	*copyable = (collection_id != NULL);

	return TRUE;
}

typedef struct {
	GCancellable *cancellable;
	gulong cancelled_id;
	gboolean finished;
	GError *error;
} OsCopyProcessHelper;

static void
os_copy_process_helper_free (OsCopyProcessHelper *helper)
{
	g_clear_object (&helper->cancellable);
	g_clear_error (&helper->error);
	g_free (helper);
}

static OsCopyProcessHelper *
os_copy_process_helper_new (GCancellable *cancellable, gulong cancelled_id)
{
	OsCopyProcessHelper *helper = g_new0 (OsCopyProcessHelper, 1);
	helper->cancellable = g_object_ref (cancellable);
	helper->cancelled_id = cancelled_id;
	helper->finished = FALSE;
	helper->error = NULL;
	return helper;
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(OsCopyProcessHelper, os_copy_process_helper_free)

static void
os_copy_process_watch_cb (GPid pid, gint status, gpointer user_data)
{
	OsCopyProcessHelper *helper = user_data;
	g_autoptr(GError) error = NULL;

	if (!g_cancellable_is_cancelled (helper->cancellable) && status != 0)
		g_set_error (&helper->error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "failed to copy OS to removable media: command "
			     "failed with status %d", status);

	g_cancellable_disconnect (helper->cancellable, helper->cancelled_id);
	g_spawn_close_pid (pid);

	/* once the copy terminates (successfully or not), set plugin status to
	 * update UI accordingly */

	helper->finished = TRUE;
}

static void
os_copy_cancelled_cb (GCancellable *cancellable, gpointer user_data)
{
	GPid pid = GPOINTER_TO_INT (user_data);

	/* terminate the process which is copying the OS */
	kill (pid, SIGTERM);
}

gboolean
gs_plugin_os_copy (GsPlugin *plugin,
		   GFile *copy_dest,
		   GCancellable *cancellable,
		   GError **error)
{
	/* this is used in an async function but we block here until that
	 * returns so we won't auto-free while other threads depend on this */
	g_autoptr (OsCopyProcessHelper) helper = NULL;
	gboolean spawn_retval;
	const gchar *argv[] = {"/usr/bin/pkexec",
			       "/usr/bin/eos-updater-prepare-volume",
			       g_file_peek_path (copy_dest),
			       NULL};
	GPid child_pid;
	gulong cancelled_id;

	g_debug ("Copying OS to: %s", g_file_peek_path (copy_dest));

	spawn_retval = g_spawn_async (".",
				      (gchar **) argv,
				      NULL,
				      G_SPAWN_DO_NOT_REAP_CHILD,
				      NULL,
				      NULL,
				      &child_pid,
				      error);

	if (spawn_retval) {
		cancelled_id = g_cancellable_connect (cancellable,
						      G_CALLBACK (os_copy_cancelled_cb),
						      GINT_TO_POINTER (child_pid),
						      NULL);

		helper = os_copy_process_helper_new (cancellable,
								cancelled_id);
		g_child_watch_add (child_pid, os_copy_process_watch_cb, helper);
	} else
		return FALSE;

	/* Iterate the main loop until either the copy process completes or the
	 * user cancels the copy. Without this, it is impossible to cancel the
	 * copy because we reach the end of this function, its parent GTask
	 * returns and we disconnect the handler that would kill the copy
	 * process. */
	while (!helper->finished)
		g_main_context_iteration (NULL, FALSE);

	if (helper->error) {
		g_propagate_error (error, g_steal_pointer (&helper->error));
		return FALSE;
	}

	return TRUE;
}
