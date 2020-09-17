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

#define METADATA_SYS_DESKTOP_FILE "EndlessOS::system-desktop-file"

/*
 * SECTION:
 * Plugin to improve GNOME Software integration in the EOS desktop.
 */

static gboolean app_is_flatpak (GsApp *app);

struct GsPluginData
{
	SoupSession *soup_session;
};

gboolean
gs_plugin_setup (GsPlugin *plugin,
		 GCancellable *cancellable,
		 GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);

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

/*
 * gs_flatpak_get_services_app_if_needed:
 * Specific to Endless runtimes, returns a GsApp reference for the
 * "EknServicesMultiplexer" app that provides system services for apps that
 * use the SDK, or use the old runtime and appear to be an Endless app.
 *
 * Returns NULL if no services app was required or there was an error.
 * Does not take a GError since any error should be recoverable. However, in
 * order to make sure the operation was not cancelled, you should check the
 * status of the GCancellable after calling this.
 */
static GsApp *
gs_flatpak_get_services_app_if_needed (GsPlugin *plugin,
				       GsApp *app,
				       GsApp *runtime,
				       GCancellable *cancellable)
{
	const gchar *app_id;
	const gchar *runtime_id;
	gboolean needed = FALSE;
	const gchar *services_id = "com.endlessm.EknServicesMultiplexer";
	const gchar *services_branch = "stable";
	g_autofree gchar *description = NULL;
	g_autoptr(GError) error_local = NULL;
	g_autoptr(GsApp) services_app = NULL;

	app_id = gs_app_get_id (app);
	runtime_id = gs_app_get_id (runtime);

	if (g_strcmp0 (runtime_id, "com.endlessm.apps.Platform") == 0)
		needed = TRUE;
	else if (g_strcmp0 (runtime_id, "com.endlessm.Platform") == 0 &&
		 g_str_has_prefix (app_id, "com.endlessm."))
		needed = TRUE;

	if (!needed)
		return NULL;

	/* Construct a GsApp for EknServicesMultiplexer */
	services_app = gs_app_new (services_id);
	gs_app_set_kind (services_app, AS_APP_KIND_DESKTOP);
	gs_app_set_branch (services_app, services_branch);
	gs_app_add_quirk (services_app, GS_APP_QUIRK_IS_WILDCARD);

	return g_steal_pointer (&services_app);
}

static void
gs_plugin_eos_refine_ekn_services_for_app (GsPlugin     *plugin,
                                           GsApp        *app,
                                           GCancellable *cancellable)
{
	GsApp *runtime;
	g_autoptr(GsApp) services_app = NULL;

	runtime = gs_app_get_runtime (app);
	if (runtime == NULL)
		return;

	/* add services flatpak */
	services_app = gs_flatpak_get_services_app_if_needed (plugin, app, runtime, cancellable);
	if (services_app != NULL)
		gs_app_add_related (app, services_app);
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

		gs_plugin_eos_refine_popular_app (plugin, app);

		gs_plugin_eos_refine_ekn_services_for_app (plugin, app, cancellable);
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
