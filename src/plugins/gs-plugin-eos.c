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
#include <sys/types.h>
#include <sys/xattr.h>

#include "gs-flatpak.h"

#define ENDLESS_ID_PREFIX "com.endlessm."

#define EOS_IMAGE_VERSION_XATTR "user.eos-image-version"
#define EOS_IMAGE_VERSION_PATH "/sysroot"
#define EOS_IMAGE_VERSION_ALT_PATH "/"

#define EOS_PROXY_APP_PREFIX ENDLESS_ID_PREFIX "proxy"

#define EOS_APPS_REMOTE_NAME "eos-apps"

#define METADATA_SYS_DESKTOP_FILE "flatpak-3rdparty::system-desktop-file"

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
	GHashTable *usr_default_branches;
	GHashTable *sys_default_branches;
	char *personality;
	GsFlatpak	*usr_flatpak;
	GsFlatpak	*sys_flatpak;
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

static void
reload_default_branches (GsPlugin *plugin, GsFlatpakScope scope)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	GHashTable *default_branches = NULL;
	GsFlatpak *flatpak = NULL;

	if (scope == GS_FLATPAK_SCOPE_USER) {
		flatpak = priv->usr_flatpak;
		default_branches = priv->usr_default_branches;
	} else {
		flatpak = priv->sys_flatpak;
		default_branches = priv->sys_default_branches;
	}

	g_hash_table_remove_all (default_branches);
	gs_flatpak_fill_default_branches (flatpak, default_branches);

	if (scope == GS_FLATPAK_SCOPE_SYSTEM &&
	    !g_hash_table_lookup (default_branches, EOS_APPS_REMOTE_NAME)) {
		g_warning ("No default branches configured for Endless' apps "
			   "remote '%s'! Using fallback branches for Endless "
			   "remotes (eos3)...",
			   EOS_APPS_REMOTE_NAME);
		g_hash_table_insert (priv->sys_default_branches,
				     g_strdup (EOS_APPS_REMOTE_NAME),
				     g_strdup ("eos3"));
	}
}

static char *
get_image_version_for_path (const char *path)
{
	ssize_t xattr_size = 0;
	char *image_version = NULL;

	xattr_size = getxattr (path, EOS_IMAGE_VERSION_XATTR, NULL, 0);

	if (xattr_size == -1) {
		return NULL;
	}

	image_version = g_malloc0 (xattr_size + 1);

	xattr_size = getxattr (path, EOS_IMAGE_VERSION_XATTR,
			       image_version, xattr_size);

	/* this check is just in case the xattr has changed in between the
	 * size checks */
	if (xattr_size == -1) {
		g_warning ("Error when getting the 'eos-image-version' from %s",
			   path);
		return NULL;
	}

	return image_version;
}

static char *
get_image_version (void)
{
	char *image_version =
		get_image_version_for_path (EOS_IMAGE_VERSION_PATH);

	if (!image_version)
		image_version =
			get_image_version_for_path (EOS_IMAGE_VERSION_ALT_PATH);

	return image_version;
}

static char *
get_personality (void)
{
	g_autofree char *image_version = get_image_version ();
	g_auto(GStrv) tokens = NULL;
	guint num_tokens = 0;
	char *personality = NULL;

	if (!image_version)
		return NULL;

	tokens = g_strsplit (image_version, ".", 0);
	num_tokens = g_strv_length (tokens);
	personality = tokens[num_tokens - 1];

	return g_strdup (personality);
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
	priv->usr_default_branches = g_hash_table_new_full (g_str_hash, g_str_equal,
							    g_free, g_free);
	priv->sys_default_branches = g_hash_table_new_full (g_str_hash, g_str_equal,
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
	priv->personality = get_personality ();

	if (!priv->personality)
		g_warning ("No system personality could be set!");

	priv->usr_flatpak = gs_flatpak_new (plugin, GS_FLATPAK_SCOPE_USER);
	priv->sys_flatpak = gs_flatpak_new (plugin, GS_FLATPAK_SCOPE_SYSTEM);

	/* XXX: we do not expect downloaded updates when using this plugin but
	 * this should be configured in a more independent way */
	gs_flatpak_set_download_updates (priv->usr_flatpak, FALSE);
	gs_flatpak_set_download_updates (priv->sys_flatpak, FALSE);
}

/**
 * gs_plugin_setup:
 */
gboolean
gs_plugin_setup (GsPlugin *plugin,
		 GCancellable *cancellable,
		 GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);

	if (!gs_flatpak_setup (priv->usr_flatpak, cancellable, error) ||
	    !gs_flatpak_setup (priv->sys_flatpak, cancellable, error))
		return FALSE;

	return TRUE;
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
	g_clear_object (&priv->usr_flatpak);
	g_clear_object (&priv->sys_flatpak);
	g_hash_table_destroy (priv->desktop_apps);
	g_hash_table_destroy (priv->usr_default_branches);
	g_hash_table_destroy (priv->sys_default_branches);
	g_free (priv->personality);
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

static gboolean
is_same_app (GsApp *app_a, GsApp *app_b)
{
	const char *app_a_id;
	const char *app_b_id;

	if (!app_a || !app_b)
		return FALSE;

	app_a_id = gs_app_get_unique_id (app_a);
	app_b_id = gs_app_get_unique_id (app_b);

	return (app_a == app_b) || (g_strcmp0 (app_a_id, app_b_id) == 0);
}

static void
gs_plugin_update_locale_cache_app (GsPlugin *plugin,
				   const char *locale_cache_key,
				   GsApp *app)
{
	GsApp *cached_app = gs_plugin_cache_lookup (plugin, locale_cache_key);

	/* avoid blacklisting the same app that's already cached */
	if (is_same_app (cached_app, app))
		return;

	if (cached_app && !gs_app_is_installed (cached_app)) {
		const char *app_id = gs_app_get_unique_id (app);
		const char *cached_app_id = gs_app_get_unique_id (cached_app);

		g_debug ("Blacklisting '%s': using '%s' due to its locale",
			 cached_app_id, app_id);
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
		if (gs_app_is_installed (app))
			return FALSE;

		g_debug ("Blacklisting '%s': incompatible with the current "
			 "locale", gs_app_get_unique_id (app));
		gs_app_add_category (app, "Blacklisted");

		return TRUE;
	}

	locale_cache_key = get_app_locale_cache_key (app_name);
	cached_app = gs_plugin_cache_lookup (plugin, locale_cache_key);

	if (is_same_app (cached_app, app))
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
app_is_banned_for_personality (GsPlugin *plugin, GsApp *app)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	const char *id = gs_app_get_id (app);

	/* only block apps based on personality if they are not installed */
	if (gs_app_get_state (app) == AS_APP_STATE_INSTALLED ||
	    gs_app_get_state (app) == AS_APP_STATE_UPDATABLE ||
	    gs_app_get_state (app) == AS_APP_STATE_UPDATABLE_LIVE)
		return FALSE;

	return ((g_strcmp0 (priv->personality, "es_GT") == 0) &&
	        (g_strcmp0 (id, "org.openarena.Openarena.desktop") == 0)) ||
	       ((g_strcmp0 (priv->personality, "zh_CN") == 0) &&
	        g_str_has_prefix (id, "com.endlessm.encyclopedia"));
}

static gboolean
app_is_proxy (GsApp *app)
{
	return g_str_has_prefix (gs_app_get_id (app), EOS_PROXY_APP_PREFIX);
}

static gboolean
gs_plugin_eos_blacklist_if_needed (GsPlugin *plugin, GsApp *app)
{
	gboolean blacklist_app = FALSE;
	const char *id = gs_app_get_id (app);

	blacklist_app = gs_app_get_kind (app) != AS_APP_KIND_DESKTOP &&
			gs_app_has_quirk (app, AS_APP_QUIRK_COMPULSORY) &&
			!app_is_proxy (app);

	if (!blacklist_app) {
		if (g_str_has_prefix (id, "eos-link-")) {
			blacklist_app = TRUE;
		} else if (gs_app_has_quirk (app, AS_APP_QUIRK_COMPULSORY) &&
			   g_strcmp0 (id, "org.gnome.Software.desktop") == 0) {
			blacklist_app = TRUE;
		} else if (app_is_renamed (app)) {
			blacklist_app = TRUE;
		} else if (app_is_banned_for_personality (plugin, app)) {
			blacklist_app = TRUE;
		} else {
			const char *metadata =
				gs_app_get_metadata_item (app,
							  "X-GnomeSoftware-NoDisplay");
			if (g_strcmp0 (metadata, "true") == 0)
				blacklist_app = TRUE;
		}
	}

	if (blacklist_app)
		gs_app_add_category (app, "Blacklisted");

	return blacklist_app;
}

static GDesktopAppInfo *
get_desktop_app_info (GsApp *app)
{
	const char *desktop_file_id =
		gs_app_get_metadata_item (app, METADATA_SYS_DESKTOP_FILE);

	if (!desktop_file_id)
		desktop_file_id = gs_app_get_id (app);

	return gs_utils_get_desktop_app_info (desktop_file_id);
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
	app_info = get_desktop_app_info (app);
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

	if (gs_app_get_scope (app) == AS_APP_SCOPE_SYSTEM)
		default_branch = g_hash_table_lookup (priv->sys_default_branches, origin);
	else
		default_branch = g_hash_table_lookup (priv->usr_default_branches, origin);

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

		if (gs_plugin_eos_blacklist_if_needed (plugin, app))
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
	g_autoptr (GDesktopAppInfo) app_info = get_desktop_app_info (app);
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
	g_autoptr (GDesktopAppInfo) app_info = get_desktop_app_info (app);
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
	reload_default_branches (plugin, GS_FLATPAK_SCOPE_USER);
	reload_default_branches (plugin, GS_FLATPAK_SCOPE_SYSTEM);
	return TRUE;
}

static gint
compare_app_ids (GsApp *a,
		 GsApp *b)
{
	return g_strcmp0 (gs_app_get_id (a), gs_app_get_id (b));
}

static gboolean
filter_proxied_apps (GsApp *app,
		     gpointer data)
{
	GSList *proxied_apps = (GSList *) data;
	return g_slist_find_custom (proxied_apps, app,
				(GCompareFunc) compare_app_ids) == NULL;
}

static GsApp *
gs_plugin_eos_create_updates_proxy_app (GsPlugin *plugin)
{
	const char *id = EOS_PROXY_APP_PREFIX ".EOSUpdatesProxy";
	GsApp *proxy = gs_app_new (id);
	g_autoptr(AsIcon) icon;

	gs_app_set_scope (proxy, AS_APP_SCOPE_SYSTEM);
	gs_app_set_kind (proxy, AS_APP_KIND_RUNTIME);
	/* TRANSLATORS: this is the name of the Endless Platform app */
	gs_app_set_name (proxy, GS_APP_QUALITY_NORMAL,
			 _("Endless Platform"));
	/* TRANSLATORS: this is the summary of the Endless Platform app */
	gs_app_set_summary (proxy, GS_APP_QUALITY_NORMAL,
			    _("Framework for applications"));
	gs_app_set_state (proxy, AS_APP_STATE_UPDATABLE_LIVE);
	gs_app_set_management_plugin (proxy, gs_plugin_get_name (plugin));

	icon = as_icon_new ();
	as_icon_set_kind (icon, AS_ICON_KIND_STOCK);
	as_icon_set_name (icon, "system-run-symbolic");
	gs_app_add_icon (proxy, icon);

	return proxy;
}

static GsFlatpak *
gs_plugin_get_gs_flatpak_for_app (GsPlugin *plugin,
				  GsApp *app)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);

	if (gs_app_get_scope (app) == AS_APP_SCOPE_SYSTEM)
		return priv->sys_flatpak;

	return priv->usr_flatpak;
}

gboolean
gs_plugin_update_app (GsPlugin *plugin,
		      GsApp *proxy,
		      GCancellable *cancellable,
		      GError **error)
{
	GPtrArray *proxied_apps = NULL;
	guint i;
	guint num_apps_updated = 0;
	guint num_apps_to_update = 0;

	/* we only update proxy apps in this plugin */
	if (!app_is_proxy (proxy))
		return TRUE;

	proxied_apps = gs_app_get_related (proxy);

	if (proxied_apps->len == 0)
		return TRUE;

	gs_app_set_state (proxy, AS_APP_STATE_INSTALLING);

	num_apps_to_update = proxied_apps->len;
	for (i = 0; i < num_apps_to_update; ++i) {
		g_autofree char *management = NULL;
		GsApp *app = g_ptr_array_index (proxied_apps, i);
		GsFlatpak *flatpak = gs_plugin_get_gs_flatpak_for_app (plugin,
								       app);
		gboolean update_result = FALSE;

		g_debug ("Updating '%s' from proxy '%s' ",
			 gs_app_get_unique_id (app),
			 gs_app_get_unique_id (proxy));

		/* set the management plugin momentaneously so we can really
		 * update it; we reset it back later */
		management = g_strdup (gs_app_get_management_plugin (app));
		gs_app_set_management_plugin (app, gs_plugin_get_name (plugin));

		update_result = gs_flatpak_update_app (flatpak, app,
						       cancellable, error);

		gs_app_set_management_plugin (app, management);

		/* in case one of the updates failed we fail too */
		if (!update_result) {
			gs_app_set_state_recover (proxy);
			return FALSE;
		}

		++num_apps_updated;

		if (g_cancellable_is_cancelled (cancellable))
			break;
	}

	if (num_apps_updated != num_apps_to_update) {
		gs_app_set_state_recover (proxy);
		return TRUE;
	}

	gs_app_set_state (proxy, AS_APP_STATE_INSTALLED);

	return TRUE;
}

gboolean
gs_plugin_add_updates (GsPlugin *plugin,
		       GsAppList *list,
		       GCancellable *cancellable,
		       GError **error)
{
	guint i;
	g_autoptr(GsApp) updates_proxy_app = NULL;
	g_autoptr(GSList) proxied_updates = NULL;
	GSList *iter;
	const char *proxied_apps[] = {"com.endlessm.Platform.runtime",
				      "com.endlessm.EknServices.desktop",
				      NULL};

	for (i = 0; i < gs_app_list_length (list); ++i) {
		GsApp *app = gs_app_list_index (list, i);
		const char *id = gs_app_get_id (app);

		if (!g_strv_contains (proxied_apps, id))
			continue;

		proxied_updates = g_slist_prepend (proxied_updates, app);
	}

	if (!proxied_updates)
		return TRUE;

	/* remove proxied apps from updates list */
	gs_app_list_filter (list, filter_proxied_apps, proxied_updates);

	updates_proxy_app = gs_plugin_eos_create_updates_proxy_app (plugin);

	for (iter = proxied_updates; iter; iter = g_slist_next (iter)) {
		GsApp *app = GS_APP (iter->data);
		gs_app_add_related (updates_proxy_app, app);
	}

	gs_app_list_add (list, updates_proxy_app);

	return TRUE;
}
