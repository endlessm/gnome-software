/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2016 Endless Mobile, Inc
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

#include <gio/gdesktopappinfo.h>
#include <libsoup/soup.h>
#include <gnome-software.h>
#include <glib/gi18n.h>
#include <gs-common.h>
#include <gs-plugin.h>
#include <gs-utils.h>

#include "gs-flatpak.h"

#define FLATPAK_EXTRA_CONF_REMOTE_GROUP_PREFIX "remote:"
#define FLATPAK_EXTRA_CONF_REMOTE_DEFAULT_BRANCH "default-branch"
#define ENDLESS_ID_PREFIX "com.endlessm."

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
	GHashTable *default_branches;
};

static GHashTable *
get_applications_with_shortcuts (GsPlugin	*self,
				 GCancellable	*cancellable,
				 GError		**error);

static void
on_desktop_apps_changed (GDBusConnection *connection,
			 const gchar	 *sender_name,
			 const gchar	 *object_path,
			 const gchar	 *interface_name,
			 const gchar	 *signal_name,
			 GVariant	 *parameters,
			 GsPlugin	 *plugin)
{
	GHashTable *apps;
	GHashTableIter iter;
	gpointer key, value;
	GsPluginData *priv = gs_plugin_get_data (plugin);
	apps = get_applications_with_shortcuts (plugin, NULL, NULL);

	g_hash_table_iter_init (&iter, priv->desktop_apps);
	while (g_hash_table_iter_next (&iter, &key, &value)) {
		GsApp *app = gs_plugin_cache_lookup (plugin, key);

		if (!g_hash_table_lookup (apps, key)) {
			if (app)
				gs_app_remove_quirk (app,
						     AS_APP_QUIRK_HAS_SHORTCUT);

			g_hash_table_remove (priv->desktop_apps, key);
		} else {
			if (app)
				gs_app_add_quirk (app,
						  AS_APP_QUIRK_HAS_SHORTCUT);

			g_hash_table_add (priv->desktop_apps,
					  g_strdup (key));
		}
	}

	if (apps)
		g_hash_table_destroy (apps);
}

static GKeyFile *
load_branches_config_file (const char *path)
{
	g_autoptr(GKeyFile) config_file = g_key_file_new ();
	g_autoptr(GError) error = NULL;

	g_debug ("Reloading default branches from '%s'...", path);

	if (!g_key_file_load_from_file (config_file, path,
					G_KEY_FILE_NONE, &error)) {
		g_debug ("Error loading Flatpak extra config file '%s': %s",
			 path, error->message);
		return NULL;
	}

	return g_steal_pointer (&config_file);
}

static void
reload_default_branches (GsPlugin *plugin)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	g_autoptr(GKeyFile) config_file = g_key_file_new ();
	g_auto(GStrv) groups = NULL;
	g_autofree char *extra_conf_file = NULL;
	int idx;
	const guint group_prefix_len =
		strlen (FLATPAK_EXTRA_CONF_REMOTE_GROUP_PREFIX);;

	g_hash_table_remove_all (priv->default_branches);

	extra_conf_file = g_build_filename (SYSCONFDIR, "gnome-software",
					    "flatpak-extra.conf", NULL);
	config_file = load_branches_config_file (extra_conf_file);

	if (!config_file) {
		/* Try loading the file from our pkgdatadir instead */
		g_free (extra_conf_file);
		extra_conf_file = g_build_filename (GS_DATA, "flatpak-extra.conf", NULL);

		config_file = load_branches_config_file (extra_conf_file);
	}

	if (!config_file)
		return;

	groups = g_key_file_get_groups (config_file, NULL);
	for (idx = 0; groups[idx] != NULL; idx++) {
		const char *group = groups[idx];
		char *remote = NULL;
		char *default_branch = NULL;

		if (!g_str_has_prefix (group,
				       FLATPAK_EXTRA_CONF_REMOTE_GROUP_PREFIX))
			continue;

		default_branch = g_key_file_get_string (config_file,
							group,
							FLATPAK_EXTRA_CONF_REMOTE_DEFAULT_BRANCH,
							NULL);

		if (!default_branch)
			continue;

		remote = g_strdup (group + group_prefix_len);
		g_hash_table_insert (priv->default_branches, remote,
				     default_branch);
		g_debug ("Found default branch '%s' for remote '%s'",
			 default_branch, remote);
	}

	if (g_hash_table_size (priv->default_branches) == 0)
		g_debug ("No default branches configured!");
}

/**
 * gs_plugin_initialize:
 */
void
gs_plugin_initialize (GsPlugin *plugin)
{
	GsPluginData *priv = gs_plugin_alloc_data (plugin,
						   sizeof(GsPluginData));

	/* let the flatpak plugins run first so we deal with the apps
	 * in a more complete/refined state */
	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_AFTER, "flatpak-system");
	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_AFTER, "flatpak-user");

	priv->session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
	priv->desktop_apps = g_hash_table_new_full (g_str_hash, g_str_equal,
						    g_free, NULL);
	priv->default_branches = g_hash_table_new_full (g_str_hash, g_str_equal,
							g_free, g_free);
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
	priv->soup_session = soup_session_new_with_options (SOUP_SESSION_USER_AGENT,
	                                                    gs_user_agent (),
	                                                    NULL);
}

/**
 * gs_plugin_destroy:
 */
void
gs_plugin_destroy (GsPlugin *plugin)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);

	if (priv->applications_changed_id != 0) {
		g_dbus_connection_signal_unsubscribe (priv->session_bus,
						      priv->applications_changed_id);
		priv->applications_changed_id = 0;
	}

	g_clear_object (&priv->session_bus);
	g_clear_object (&priv->soup_session);
	g_hash_table_destroy (priv->desktop_apps);
	g_hash_table_destroy (priv->default_branches);
}

static GHashTable *
get_applications_with_shortcuts (GsPlugin	*plugin,
				 GCancellable	*cancellable,
				 GError		**error_out) {
	g_autoptr (GVariantIter) iter = NULL;
	g_autoptr (GVariant) apps = NULL;
	GError *error = NULL;
	gchar *application;
	GHashTable *apps_table;
	GsPluginData *priv = gs_plugin_get_data (plugin);

	apps_table = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
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
					    &error);
	if (error != NULL) {
		g_critical ("Unable to list available applications: %s",
			    error->message);
		g_propagate_error (error_out, error);
		return NULL;
	}

	g_variant_get (apps, "(as)", &iter);

	while (g_variant_iter_loop (iter, "s", &application))
		g_hash_table_add (apps_table, g_strdup (application));

	return apps_table;
}

static gboolean
app_is_renamed (GsApp *app)
{
	/* Apps renamed by eos-desktop get the desktop attribute of
	 * X-Endless-CreatedBy assigned to the desktop's name */
	return g_strcmp0 (gs_app_get_metadata_item (app, "X-Endless-CreatedBy"),
			  "eos-desktop") == 0;
}

static gboolean
gs_plugin_locale_is_compatible (GsPlugin *plugin,
				const char *locale)
{
	g_auto(GStrv) locale_variants;
	const char *plugin_locale = gs_plugin_get_locale (plugin);
	int idx;

	locale_variants = g_get_locale_variants (plugin_locale);
	for (idx = 0; locale_variants[idx] != NULL; idx++) {
		if (g_strcmp0 (locale_variants[idx], locale) == 0)
			return TRUE;
	}

	return FALSE;
}

static char *
get_app_locale_cache_key (const char *app_name)
{
	guint name_length = strlen (app_name);
	char *suffix = NULL;
	/* locales can be as long as 5 chars (e.g. pt_PT) so  */
	const guint locale_max_length = 5;
	char *locale_cache_name;

	if (name_length <= locale_max_length)
		return NULL;

	locale_cache_name = g_strdup_printf ("locale:%s", app_name);
	/* include the 'locale:' prefix */
	name_length += 7;

	/* get the suffix after the last '.' so we can get
	 * e.g. com.endlessm.FooBar.pt or com.endlessm.FooBar.pt_BR */
	suffix = g_strrstr (locale_cache_name + name_length - locale_max_length,
			    ".");

	if (suffix) {
		/* get the language part of the eventual locale suffix
		 * e.g. pt_BR -> pt */
		char *locale_split = g_strrstr (suffix + 1, "_");

		if (locale_split)
			*locale_split = '\0';
	}

	return locale_cache_name;
}

static gboolean
gs_plugin_app_is_locale_best_match (GsPlugin *plugin,
				    GsApp *app)
{
	return g_str_has_suffix (gs_app_get_flatpak_name (app),
				 gs_plugin_get_locale (plugin));
}

static void
gs_plugin_update_locale_cache_app (GsPlugin *plugin,
				   const char *locale_cache_key,
				   GsApp *app)
{
	GsApp *cached_app = gs_plugin_cache_lookup (plugin, locale_cache_key);

	/* avoid blacklisting the same app that's already cached */
	if (cached_app == app)
		return;

	if (cached_app && !gs_app_is_installed (cached_app)) {
		g_debug ("Blacklisting '%s': using '%s' due to its locale",
			 gs_app_get_unique_id (cached_app),
			 gs_app_get_unique_id (app));
		gs_app_add_category (cached_app, "Blacklisted");
	}

	gs_plugin_cache_add (plugin, locale_cache_key, app);
}

static gboolean
gs_plugin_eos_blacklist_kapp_if_needed (GsPlugin *plugin, GsApp *app)
{
	guint endless_prefix_len = strlen (ENDLESS_ID_PREFIX);
	g_autofree char *locale_cache_key = NULL;
	g_auto(GStrv) tokens = NULL;
	const char *last_token = NULL;
	guint num_tokens = 0;
	/* getting the app name, besides skipping the '.desktop' part of the id
	 * also makes sure we're dealing with a Flatpak app */
	const char *app_name = gs_app_get_flatpak_name (app);
	GsApp *cached_app = NULL;

	if (!app_name || !g_str_has_prefix (app_name, ENDLESS_ID_PREFIX))
		return FALSE;

	tokens = g_strsplit (app_name + endless_prefix_len, ".", -1);
	num_tokens = g_strv_length (tokens);

	/* we need at least 2 tokens: app-name & locale */
	if (num_tokens < 2)
		return FALSE;

	/* last token may be the locale */
	last_token = tokens[num_tokens - 1];

	if (!gs_plugin_locale_is_compatible (plugin, last_token)) {
		if (!gs_app_is_installed (app)) {
			g_debug ("Blacklisting '%s': incompatible with the "
				 "current locale", gs_app_get_unique_id (app));
			gs_app_add_category (app, "Blacklisted");
		}

		return TRUE;
	}

	locale_cache_key = get_app_locale_cache_key (app_name);
	cached_app = gs_plugin_cache_lookup (plugin, locale_cache_key);

	if (cached_app == app)
		return FALSE;

	/* skip if the cached app is already our best */
	if (cached_app &&
	    gs_plugin_app_is_locale_best_match (plugin, cached_app)) {
		if (!gs_app_is_installed (app)) {
			g_debug ("Blacklisting '%s': cached app '%s' is best "
				 "match", gs_app_get_unique_id (app),
				 gs_app_get_unique_id (cached_app));
			gs_app_add_category (app, "Blacklisted");
		}

		return TRUE;
	}

	gs_plugin_update_locale_cache_app (plugin, locale_cache_key, app);
	return FALSE;
}

static gboolean
gs_plugin_eos_blacklist_if_needed (GsApp *app)
{
	gboolean blacklist_app = FALSE;
	const char *id = gs_app_get_id (app);

	blacklist_app = gs_app_get_kind (app) != AS_APP_KIND_DESKTOP &&
			gs_app_has_quirk (app, AS_APP_QUIRK_COMPULSORY);

	if (!blacklist_app) {
		if (g_str_has_prefix (id, "eos-link-")) {
			blacklist_app = TRUE;
		} else if (gs_app_has_quirk (app, AS_APP_QUIRK_COMPULSORY) &&
			   g_strcmp0 (id, "org.gnome.Software.desktop") == 0) {
			blacklist_app = TRUE;
		} else if (app_is_renamed (app)) {
			blacklist_app = TRUE;
		}
	}

	if (blacklist_app)
		gs_app_add_category (app, "Blacklisted");

	return blacklist_app;
}

static void
gs_plugin_eos_update_app_shortcuts_info (GsPlugin *plugin,
					 GsApp *app,
					 GHashTable *apps_with_shortcuts)
{
	GsPluginData *priv = NULL;
	const char *app_id = NULL;
	g_autoptr (GDesktopAppInfo) app_info = NULL;

	if (gs_app_get_state (app) != AS_APP_STATE_INSTALLED &&
	    gs_app_get_state (app) != AS_APP_STATE_UPDATABLE) {
		gs_app_remove_quirk (app, AS_APP_QUIRK_HAS_SHORTCUT);
		return;
	}

	priv = gs_plugin_get_data (plugin);
	app_id = gs_app_get_id (app);
	app_info = gs_utils_get_desktop_app_info (app_id);
	if (!app_info)
		return;

	app_id = g_app_info_get_id (G_APP_INFO (app_info));

	gs_plugin_cache_add (plugin, app_id, app);
	if (g_hash_table_lookup (apps_with_shortcuts, app_id)) {
		g_hash_table_add (priv->desktop_apps, g_strdup (app_id));
		gs_app_add_quirk (app, AS_APP_QUIRK_HAS_SHORTCUT);
	} else {
		g_hash_table_remove (priv->desktop_apps, app_id);
		gs_app_remove_quirk (app, AS_APP_QUIRK_HAS_SHORTCUT);
	}
}

static void
gs_plugin_eos_refine_core_app (GsApp *app)
{
	/* we only allow to remove flatpak apps */
	if (!gs_app_is_flatpak (app)) {
		gs_app_add_quirk (app, AS_APP_QUIRK_COMPULSORY);
	}
}

typedef struct _PopularBackgroundImageTileRequestData
{
	GsApp *app;
	GsPlugin *plugin;
	char *cache_filename;
} PopularBackgroundImageTileRequestData;

static void
popular_background_image_tile_request_data_destroy (PopularBackgroundImageTileRequestData *data)
{
	g_free (data->cache_filename);
	g_free (data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(PopularBackgroundImageTileRequestData,
                              popular_background_image_tile_request_data_destroy);

static void
gs_plugin_eos_update_tile_image_from_filename (GsApp      *app,
                                               const char *filename)
{
	g_autofree char *css = g_strdup_printf ("background-image: url('%s')",
	                                       filename);
	gs_app_set_metadata (app, "GnomeSoftware::ImageTile-css", css);
}

static void
gs_plugin_eos_tile_image_downloaded_cb (SoupSession *session,
                                        SoupMessage *msg,
                                        gpointer user_data)
{
	g_autoptr(PopularBackgroundImageTileRequestData) data = user_data;
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
	PopularBackgroundImageTileRequestData *request_data = NULL;
	g_autoptr(SoupURI) soup_uri = NULL;
	g_autoptr(SoupMessage) message = NULL;

	popular_bg =
	   gs_app_get_metadata_item (app, "GnomeSoftware::popular-background");

	if (!popular_bg ||
	    gs_app_get_metadata_item (app, "GnomeSoftware::ImageTile-css"))
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

	request_data = g_new0 (PopularBackgroundImageTileRequestData, 1);
	request_data->app = app;
	request_data->plugin = plugin;
	request_data->cache_filename = g_steal_pointer (&writable_cache_filename);

	soup_session_queue_message (priv->soup_session,
	                            g_steal_pointer (&message),
	                            gs_plugin_eos_tile_image_downloaded_cb,
	                            request_data);
}

void
gs_plugin_adopt_app (GsPlugin *plugin, GsApp *app)
{
	if (gs_app_is_flatpak (app))
		return;

	gs_app_set_management_plugin (app, gs_plugin_get_name (plugin));
}

static gboolean
gs_plugin_eos_blacklist_by_branch_if_needed (GsPlugin *plugin, GsApp *app)
{
	const char *default_branch;
	const char *branch = NULL;
	const char *origin = gs_app_get_origin (app);
	GsPluginData *priv = NULL;

	if (!gs_app_is_flatpak (app))
		return FALSE;

	priv = gs_plugin_get_data (plugin);
	default_branch = g_hash_table_lookup (priv->default_branches, origin);

	/* if we do not have a configured default branch for this repo then
	 * do nothing */
	if (!default_branch)
		return FALSE;

	/* if an app has no branch set, maybe it will be set later so we let
	 * it pass */
	branch = gs_app_get_flatpak_branch (app);
	if (!branch)
		return FALSE;

	/* do not show an app if it doesn't belong to the default branch that
	 * is configured for its remote */
	if (g_strcmp0 (branch, default_branch) != 0) {
		gs_app_add_category (app, "Blacklisted");
		return TRUE;
	}

	return FALSE;
}

/**
 * gs_plugin_refine:
 */
gboolean
gs_plugin_refine (GsPlugin		*plugin,
		  GsAppList		*list,
		  GsPluginRefineFlags	flags,
		  GCancellable		*cancellable,
		  GError		**error)
{
	guint i;
	GHashTable *apps;
	GsPluginData *priv = gs_plugin_get_data (plugin);

	g_hash_table_remove_all (priv->desktop_apps);
	apps = get_applications_with_shortcuts (plugin, cancellable, NULL);

	for (i = 0; i < gs_app_list_length (list); ++i) {
		GsApp *app = gs_app_list_index (list, i);

		gs_plugin_eos_refine_core_app (app);

		if (gs_plugin_eos_blacklist_if_needed (app))
			continue;

		if (gs_app_get_kind (app) != AS_APP_KIND_DESKTOP)
			continue;

		if (gs_plugin_eos_blacklist_by_branch_if_needed (plugin, app))
			continue;

		if (gs_plugin_eos_blacklist_kapp_if_needed (plugin, app))
			continue;

		gs_plugin_eos_refine_popular_app (plugin, app);

		gs_plugin_eos_update_app_shortcuts_info (plugin, app, apps);
	}

	if (apps)
		g_hash_table_destroy (apps);

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
	const char *id = gs_app_get_id (app);
	g_autoptr (GDesktopAppInfo) app_info =
		gs_utils_get_desktop_app_info (id);
	const char *app_id = g_app_info_get_id (G_APP_INFO (app_info));

	g_dbus_connection_call_sync (priv->session_bus,
				     "org.gnome.Shell",
				     "/org/gnome/Shell",
				     "org.gnome.Shell.AppStore",
				     "RemoveApplication",
				     g_variant_new ("(s)", app_id),
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
	const char *id = gs_app_get_id (app);
	g_autoptr (GDesktopAppInfo) app_info =
		gs_utils_get_desktop_app_info (id);
	const char *app_id = g_app_info_get_id (G_APP_INFO (app_info));

	g_dbus_connection_call_sync (priv->session_bus,
				     "org.gnome.Shell",
				     "/org/gnome/Shell",
				     "org.gnome.Shell.AppStore",
				     "AddApplication",
				     g_variant_new ("(s)", app_id),
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

/**
 * gs_plugin_add_shortcut:
 */
gboolean
gs_plugin_add_shortcut (GsPlugin	*plugin,
			GsApp		*app,
			GCancellable	*cancellable,
			GError		**error)
{
	gs_app_add_quirk (app, AS_APP_QUIRK_HAS_SHORTCUT);
	return add_app_to_shell (plugin, app, cancellable, error);
}

/**
 * gs_plugin_remove_shortcut:
 */
gboolean
gs_plugin_remove_shortcut (GsPlugin	*plugin,
			   GsApp	*app,
			   GCancellable	*cancellable,
			   GError	**error)
{
	gs_app_remove_quirk (app, AS_APP_QUIRK_HAS_SHORTCUT);
	return remove_app_from_shell (plugin, app, cancellable, error);
}

gboolean
gs_plugin_app_install (GsPlugin *plugin,
		       GsApp *app,
		       GCancellable *cancellable,
		       GError **error)
{
	if (!gs_app_is_flatpak (app))
		return TRUE;

	/* We're only interested in already installed flatpak apps so we can
	 * add them to the desktop */
	if (gs_app_get_state (app) != AS_APP_STATE_INSTALLED)
		return TRUE;

	add_app_to_shell (plugin, app, cancellable, error);

	return TRUE;
}

gboolean
gs_plugin_launch (GsPlugin *plugin,
		  GsApp *app,
		  GCancellable *cancellable,
		  GError **error)
{
	/* only process the app if it belongs to this plugin */
	if (g_strcmp0 (gs_app_get_management_plugin (app),
		       gs_plugin_get_name (plugin)) != 0)
		return TRUE;

	return gs_plugin_app_launch (plugin, app, error);
}

gboolean
gs_plugin_refresh (GsPlugin *plugin,
		   guint cache_age,
		   GsPluginRefreshFlags flags,
		   GCancellable *cancellable,
		   GError **error)
{
	reload_default_branches (plugin);
	return TRUE;
}
