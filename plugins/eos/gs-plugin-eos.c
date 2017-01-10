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

#include <gnome-software.h>
#include <glib/gi18n.h>
#include <gs-plugin.h>
#include <gs-utils.h>
#include <libsoup/soup.h>

#define METADATA_SYS_DESKTOP_FILE "EndlessOS::system-desktop-file"

/*
 * SECTION:
 * Plugin to improve GNOME Software integration in the EOS desktop.
 */

struct GsPluginData
{
	GDBusConnection *session_bus;
	GHashTable *desktop_apps;
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

	return TRUE;
}

void
gs_plugin_initialize (GsPlugin *plugin)
{
	gs_plugin_alloc_data (plugin, sizeof(GsPluginData));

	/* let the flatpak plugin run first so we deal with the apps
	 * in a more complete/refined state */
	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_AFTER, "flatpak");
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
}

static const char*
get_desktop_file_id (GsApp *app)
{
	const char *desktop_file_id =
		gs_app_get_metadata_item (app, METADATA_SYS_DESKTOP_FILE);

	if (!desktop_file_id)
		desktop_file_id = gs_app_get_id (app);

	g_assert (desktop_file_id != NULL);
	return desktop_file_id;
}

static void
gs_plugin_eos_update_app_shortcuts_info (GsPlugin *plugin,
					 GsApp *app)
{
	GsPluginData *priv = NULL;
	const char *desktop_file_id = NULL;
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
	const char *desktop_file_id = get_desktop_file_id (app);
	g_autoptr(GDesktopAppInfo) app_info =
		gs_utils_get_desktop_app_info (desktop_file_id);
	const char *shortcut_id = g_app_info_get_id (G_APP_INFO (app_info));

	g_dbus_connection_call_sync (priv->session_bus,
				     "org.gnome.Shell",
				     "/org/gnome/Shell",
				     "org.gnome.Shell.AppStore",
				     "RemoveApplication",
				     g_variant_new ("(s)", shortcut_id),
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

static gboolean
add_app_to_shell (GsPlugin	*plugin,
		  GsApp		*app,
		  GCancellable	*cancellable,
		  GError	**error_out)
{
	GError *error = NULL;
	GsPluginData *priv = gs_plugin_get_data (plugin);
	const char *desktop_file_id = get_desktop_file_id (app);
	g_autoptr(GDesktopAppInfo) app_info =
		gs_utils_get_desktop_app_info (desktop_file_id);
	const char *shortcut_id = g_app_info_get_id (G_APP_INFO (app_info));

	g_dbus_connection_call_sync (priv->session_bus,
				     "org.gnome.Shell",
				     "/org/gnome/Shell",
				     "org.gnome.Shell.AppStore",
				     "AddAppIfNotVisible",
				     g_variant_new ("(s)", shortcut_id),
				     NULL,
				     G_DBUS_CALL_FLAGS_NONE,
				     -1,
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
	const char *desktop_file_id = get_desktop_file_id (app);
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
