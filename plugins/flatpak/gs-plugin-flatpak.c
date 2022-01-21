/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 * vi:set noexpandtab tabstop=8 shiftwidth=8:
 *
 * Copyright (C) 2016 Joaquim Rocha <jrocha@endlessm.com>
 * Copyright (C) 2016-2018 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2017-2020 Kalev Lember <klember@redhat.com>
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

/* Notes:
 *
 * All GsApp's created have management-plugin set to flatpak
 * Some GsApp's created have have flatpak::kind of app or runtime
 * The GsApp:origin is the remote name, e.g. test-repo
 */

#include <config.h>

#include <flatpak.h>
#include <glib/gi18n.h>
#include <gnome-software.h>

#include "gs-appstream.h"
#include "gs-flatpak-app.h"
#include "gs-flatpak.h"
#include "gs-flatpak-transaction.h"
#include "gs-flatpak-utils.h"
#include "gs-metered.h"

struct GsPluginData {
	GPtrArray		*flatpaks; /* of GsFlatpak */
	gboolean		 has_system_helper;
	const gchar		*destdir_for_tests;
};

void
gs_plugin_initialize (GsPlugin *plugin)
{
	GsPluginData *priv = gs_plugin_alloc_data (plugin, sizeof(GsPluginData));
	const gchar *action_id = "org.freedesktop.Flatpak.appstream-update";
	g_autoptr(GError) error_local = NULL;
	g_autoptr(GPermission) permission = NULL;

	priv->flatpaks = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);

	/* getting app properties from appstream is quicker */
	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_AFTER, "appstream");

	/* like appstream, we need the icon plugin to load cached icons into pixbufs */
	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_BEFORE, "icons");

	/* prioritize over packages */
	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_BETTER_THAN, "packagekit");

	/* set name of MetaInfo file */
	gs_plugin_set_appstream_id (plugin, "org.gnome.Software.Plugin.Flatpak");

	/* if we can't update the AppStream database system-wide don't even
	 * pull the data as we can't do anything with it */
	permission = gs_utils_get_permission (action_id, NULL, &error_local);
	if (permission == NULL) {
		g_debug ("no permission for %s: %s", action_id, error_local->message);
		g_clear_error (&error_local);
	} else {
		priv->has_system_helper = g_permission_get_allowed (permission) ||
					  g_permission_get_can_acquire (permission);
	}

	/* used for self tests */
	priv->destdir_for_tests = g_getenv ("GS_SELF_TEST_FLATPAK_DATADIR");
}

static gboolean
_as_component_scope_is_compatible (AsComponentScope scope1, AsComponentScope scope2)
{
	if (scope1 == AS_COMPONENT_SCOPE_UNKNOWN)
		return TRUE;
	if (scope2 == AS_COMPONENT_SCOPE_UNKNOWN)
		return TRUE;
	return scope1 == scope2;
}

void
gs_plugin_destroy (GsPlugin *plugin)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	g_ptr_array_unref (priv->flatpaks);
}

void
gs_plugin_adopt_app (GsPlugin *plugin, GsApp *app)
{
	if (gs_app_get_bundle_kind (app) == AS_BUNDLE_KIND_FLATPAK)
		gs_app_set_management_plugin (app, gs_plugin_get_name (plugin));
}

static gboolean
gs_plugin_flatpak_add_installation (GsPlugin *plugin,
				    FlatpakInstallation *installation,
				    GCancellable *cancellable,
				    GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	g_autoptr(GsFlatpak) flatpak = NULL;

	/* create and set up */
	flatpak = gs_flatpak_new (plugin, installation, GS_FLATPAK_FLAG_NONE);
	if (!gs_flatpak_setup (flatpak, cancellable, error))
		return FALSE;
	g_debug ("successfully set up %s", gs_flatpak_get_id (flatpak));

	/* add objects that set up correctly */
	g_ptr_array_add (priv->flatpaks, g_steal_pointer (&flatpak));
	return TRUE;
}

static void
gs_plugin_flatpak_report_warning (GsPlugin *plugin,
				  GError **error)
{
	g_autoptr(GsPluginEvent) event = gs_plugin_event_new ();
	g_assert (error != NULL);
	if (*error != NULL && (*error)->domain != GS_PLUGIN_ERROR)
		gs_flatpak_error_convert (error);
	gs_plugin_event_set_error (event, *error);
	gs_plugin_event_add_flag (event,
				  GS_PLUGIN_EVENT_FLAG_WARNING);
	gs_plugin_report_event (plugin, event);
}

gboolean
gs_plugin_setup (GsPlugin *plugin, GCancellable *cancellable, GError **error)
{
	g_autoptr(GPtrArray) installations = NULL;
	GsPluginData *priv = gs_plugin_get_data (plugin);

	/* clear in case we're called from resetup in the self tests */
	g_ptr_array_set_size (priv->flatpaks, 0);

	/* if we're not just running the tests */
	if (priv->destdir_for_tests == NULL) {
		g_autoptr(GError) error_local = NULL;
		g_autoptr(FlatpakInstallation) installation = NULL;

		/* include the system installations */
		if (priv->has_system_helper) {
			installations = flatpak_get_system_installations (cancellable,
									  &error_local);

			if (installations == NULL) {
				gs_plugin_flatpak_report_warning (plugin, &error_local);
				g_clear_error (&error_local);
			}
		}

		/* include the user installation */
		installation = flatpak_installation_new_user (cancellable,
							      &error_local);
		if (installation == NULL) {
			/* if some error happened, report it as an event, but
			 * do not return it, otherwise it will disable the whole
			 * plugin (meaning that support for Flatpak will not be
			 * possible even if a system installation is working) */
			gs_plugin_flatpak_report_warning (plugin, &error_local);
		} else {
			if (installations == NULL)
				installations = g_ptr_array_new_with_free_func (g_object_unref);

			g_ptr_array_add (installations, g_steal_pointer (&installation));
		}
	} else {
		/* use the test installation */
		g_autofree gchar *full_path = g_build_filename (priv->destdir_for_tests,
								"flatpak",
								NULL);
		g_autoptr(GFile) file = g_file_new_for_path (full_path);
		g_autoptr(FlatpakInstallation) installation = NULL;
		g_debug ("using custom flatpak path %s", full_path);
		installation = flatpak_installation_new_for_path (file, TRUE,
								  cancellable,
								  error);
		if (installation == NULL) {
			gs_flatpak_error_convert (error);
			return FALSE;
		}

		installations = g_ptr_array_new_with_free_func (g_object_unref);
		g_ptr_array_add (installations, g_steal_pointer (&installation));
	}

	/* add the installations */
	for (guint i = 0; installations != NULL && i < installations->len; i++) {
		g_autoptr(GError) error_local = NULL;

		FlatpakInstallation *installation = g_ptr_array_index (installations, i);
		if (!gs_plugin_flatpak_add_installation (plugin,
							 installation,
							 cancellable,
							 &error_local)) {
			gs_plugin_flatpak_report_warning (plugin,
							  &error_local);
			continue;
		}
	}

	/* when no installation has been loaded, return the error so the
	 * plugin gets disabled */
	if (priv->flatpaks->len == 0) {
		g_set_error (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_FAILED,
			     "Failed to load any Flatpak installations");
		return FALSE;
	}

	return TRUE;
}

gboolean
gs_plugin_add_installed (GsPlugin *plugin,
			 GsAppList *list,
			 GCancellable *cancellable,
			 GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	for (guint i = 0; i < priv->flatpaks->len; i++) {
		GsFlatpak *flatpak = g_ptr_array_index (priv->flatpaks, i);
		if (!gs_flatpak_add_installed (flatpak, list, cancellable, error))
			return FALSE;
	}
	return TRUE;
}

gboolean
gs_plugin_add_sources (GsPlugin *plugin,
		       GsAppList *list,
		       GCancellable *cancellable,
		       GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	for (guint i = 0; i < priv->flatpaks->len; i++) {
		GsFlatpak *flatpak = g_ptr_array_index (priv->flatpaks, i);
		if (!gs_flatpak_add_sources (flatpak, list, cancellable, error))
			return FALSE;
	}
	return TRUE;
}

gboolean
gs_plugin_add_updates (GsPlugin *plugin,
		       GsAppList *list,
		       GCancellable *cancellable,
		       GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	for (guint i = 0; i < priv->flatpaks->len; i++) {
		GsFlatpak *flatpak = g_ptr_array_index (priv->flatpaks, i);
		if (!gs_flatpak_add_updates (flatpak, list, cancellable, error))
			return FALSE;
	}
	gs_plugin_cache_lookup_by_state (plugin, list, GS_APP_STATE_INSTALLING);
	return TRUE;
}

gboolean
gs_plugin_refresh (GsPlugin *plugin,
		   guint cache_age,
		   GCancellable *cancellable,
		   GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	for (guint i = 0; i < priv->flatpaks->len; i++) {
		GsFlatpak *flatpak = g_ptr_array_index (priv->flatpaks, i);
		if (!gs_flatpak_refresh (flatpak, cache_age, cancellable, error))
			return FALSE;
	}
	return TRUE;
}

static GsFlatpak *
gs_plugin_flatpak_get_handler (GsPlugin *plugin, GsApp *app)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	const gchar *object_id;

	/* only process this app if was created by this plugin */
	if (g_strcmp0 (gs_app_get_management_plugin (app),
		       gs_plugin_get_name (plugin)) != 0) {
		return NULL;
	}

	/* specified an explicit name */
	object_id = gs_flatpak_app_get_object_id (app);
	if (object_id != NULL) {
		for (guint i = 0; i < priv->flatpaks->len; i++) {
			GsFlatpak *flatpak = g_ptr_array_index (priv->flatpaks, i);
			if (g_strcmp0 (gs_flatpak_get_id (flatpak), object_id) == 0)
				return flatpak;
		}
	}

	/* find a scope that matches */
	for (guint i = 0; i < priv->flatpaks->len; i++) {
		GsFlatpak *flatpak = g_ptr_array_index (priv->flatpaks, i);
		if (_as_component_scope_is_compatible (gs_flatpak_get_scope (flatpak),
						 gs_app_get_scope (app)))
			return flatpak;
	}
	return NULL;
}

static gboolean
gs_plugin_flatpak_refine_app (GsPlugin *plugin,
			      GsApp *app,
			      GsPluginRefineFlags flags,
			      GCancellable *cancellable,
			      GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	GsFlatpak *flatpak = NULL;

	/* not us */
	if (gs_app_get_bundle_kind (app) != AS_BUNDLE_KIND_FLATPAK) {
		g_debug ("%s not a package, ignoring", gs_app_get_unique_id (app));
		return TRUE;
	}

	/* we have to look for the app in all GsFlatpak stores */
	if (gs_app_get_scope (app) == AS_COMPONENT_SCOPE_UNKNOWN) {
		for (guint i = 0; i < priv->flatpaks->len; i++) {
			GsFlatpak *flatpak_tmp = g_ptr_array_index (priv->flatpaks, i);
			g_autoptr(GError) error_local = NULL;
			if (gs_flatpak_refine_app_state (flatpak_tmp, app,
							 cancellable, &error_local)) {
				flatpak = flatpak_tmp;
				break;
			} else {
				g_debug ("%s", error_local->message);
			}
		}
	} else {
		flatpak = gs_plugin_flatpak_get_handler (plugin, app);
	}
	if (flatpak == NULL)
		return TRUE;
	return gs_flatpak_refine_app (flatpak, app, flags, cancellable, error);
}


static gboolean
refine_app (GsPlugin             *plugin,
	    GsApp                *app,
	    GsPluginRefineFlags   flags,
	    GCancellable         *cancellable,
	    GError              **error)
{
	/* only process this app if was created by this plugin */
	if (g_strcmp0 (gs_app_get_management_plugin (app),
		       gs_plugin_get_name (plugin)) != 0) {
		return TRUE;
	}

	/* get the runtime first */
	if (!gs_plugin_flatpak_refine_app (plugin, app, flags, cancellable, error))
		return FALSE;

	/* the runtime might be installed in a different scope */
	if (flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_RUNTIME) {
		GsApp *runtime = gs_app_get_runtime (app);
		if (runtime != NULL) {
			if (!gs_plugin_flatpak_refine_app (plugin, app,
							   flags,
							   cancellable,
							   error)) {
				return FALSE;
			}
		}
	}
	return TRUE;
}

gboolean
gs_plugin_refine (GsPlugin             *plugin,
		  GsAppList            *list,
		  GsPluginRefineFlags   flags,
		  GCancellable         *cancellable,
		  GError              **error)
{
	for (guint i = 0; i < gs_app_list_length (list); i++) {
		GsApp *app = gs_app_list_index (list, i);
		if (!refine_app (plugin, app, flags, cancellable, error))
			return FALSE;
	}

	return TRUE;
}

gboolean
gs_plugin_refine_wildcard (GsPlugin *plugin,
			   GsApp *app,
			   GsAppList *list,
			   GsPluginRefineFlags flags,
			   GCancellable *cancellable,
			   GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	for (guint i = 0; i < priv->flatpaks->len; i++) {
		GsFlatpak *flatpak = g_ptr_array_index (priv->flatpaks, i);
		if (!gs_flatpak_refine_wildcard (flatpak, app, list, flags,
						 cancellable, error)) {
			return FALSE;
		}
	}
	return TRUE;
}

gboolean
gs_plugin_launch (GsPlugin *plugin,
		  GsApp *app,
		  GCancellable *cancellable,
		  GError **error)
{
	GsFlatpak *flatpak = gs_plugin_flatpak_get_handler (plugin, app);
	if (flatpak == NULL)
		return TRUE;
	return gs_flatpak_launch (flatpak, app, cancellable, error);
}

/* ref full */
static GsApp *
gs_plugin_flatpak_find_app_by_ref (GsPlugin *plugin, const gchar *ref,
				   GCancellable *cancellable, GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);

	g_debug ("finding ref %s", ref);
	for (guint i = 0; i < priv->flatpaks->len; i++) {
		GsFlatpak *flatpak_tmp = g_ptr_array_index (priv->flatpaks, i);
		g_autoptr(GsApp) app = NULL;
		g_autoptr(GError) error_local = NULL;

		app = gs_flatpak_ref_to_app (flatpak_tmp, ref, cancellable, &error_local);
		if (app == NULL) {
			g_debug ("%s", error_local->message);
			continue;
		}
		g_debug ("found ref=%s->%s", ref, gs_app_get_unique_id (app));
		return g_steal_pointer (&app);
	}
	return NULL;
}

/* ref full */
static GsApp *
_ref_to_app (FlatpakTransaction *transaction, const gchar *ref, GsPlugin *plugin)
{
	g_return_val_if_fail (GS_IS_FLATPAK_TRANSACTION (transaction), NULL);
	g_return_val_if_fail (ref != NULL, NULL);
	g_return_val_if_fail (GS_IS_PLUGIN (plugin), NULL);

	/* search through each GsFlatpak */
	return gs_plugin_flatpak_find_app_by_ref (plugin, ref, NULL, NULL);
}

static void
_group_apps_by_installation_recurse (GsPlugin *plugin,
				     GsAppList *list,
				     GHashTable *applist_by_flatpaks)
{
	if (!list)
		return;

	for (guint i = 0; i < gs_app_list_length (list); i++) {
		GsApp *app = gs_app_list_index (list, i);
		GsFlatpak *flatpak = gs_plugin_flatpak_get_handler (plugin, app);
		if (flatpak != NULL) {
			GsAppList *list_tmp = g_hash_table_lookup (applist_by_flatpaks, flatpak);
			GsAppList *related_list;
			if (list_tmp == NULL) {
				list_tmp = gs_app_list_new ();
				g_hash_table_insert (applist_by_flatpaks,
						     g_object_ref (flatpak),
						     list_tmp);
			}
			gs_app_list_add (list_tmp, app);

			/* Add also related apps, which can be those recognized for update,
			   while the 'app' is already up to date. */
			related_list = gs_app_get_related (app);
			_group_apps_by_installation_recurse (plugin, related_list, applist_by_flatpaks);
		}
	}
}

/*
 * Returns: (transfer full) (element-type GsFlatpak GsAppList):
 *  a map from GsFlatpak to non-empty lists of apps from @list associated
 *  with that installation.
 */
static GHashTable *
_group_apps_by_installation (GsPlugin *plugin,
                             GsAppList *list)
{
	g_autoptr(GHashTable) applist_by_flatpaks = NULL;

	/* list of apps to be handled by each flatpak installation */
	applist_by_flatpaks = g_hash_table_new_full (g_direct_hash, g_direct_equal,
						     (GDestroyNotify) g_object_unref,
						     (GDestroyNotify) g_object_unref);

	/* put each app into the correct per-GsFlatpak list */
	_group_apps_by_installation_recurse (plugin, list, applist_by_flatpaks);

	return g_steal_pointer (&applist_by_flatpaks);
}

#if FLATPAK_CHECK_VERSION(1,6,0)
typedef struct {
	FlatpakTransaction *transaction;
	guint id;
} BasicAuthData;

static void
basic_auth_data_free (BasicAuthData *data)
{
	g_object_unref (data->transaction);
	g_slice_free (BasicAuthData, data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(BasicAuthData, basic_auth_data_free)

static void
_basic_auth_cb (const gchar *user, const gchar *password, gpointer user_data)
{
	g_autoptr(BasicAuthData) data = user_data;

	g_debug ("Submitting basic auth data");

	/* NULL user aborts the basic auth request */
	flatpak_transaction_complete_basic_auth (data->transaction, data->id, user, password, NULL /* options */);
}

static gboolean
_basic_auth_start (FlatpakTransaction *transaction,
                   const char *remote,
                   const char *realm,
                   GVariant *options,
                   guint id,
                   GsPlugin *plugin)
{
	BasicAuthData *data;

	if (!gs_plugin_has_flags (plugin, GS_PLUGIN_FLAGS_INTERACTIVE))
		return FALSE;

	data = g_slice_new0 (BasicAuthData);
	data->transaction = g_object_ref (transaction);
	data->id = id;

	g_debug ("Login required remote %s (realm %s)\n", remote, realm);
	gs_plugin_basic_auth_start (plugin, remote, realm, G_CALLBACK (_basic_auth_cb), data);
	return TRUE;
}

static gboolean
_webflow_start (FlatpakTransaction *transaction,
                const char *remote,
                const char *url,
                GVariant *options,
                guint id,
                GsPlugin *plugin)
{
	const char *browser;
	g_autoptr(GError) error_local = NULL;

	if (!gs_plugin_has_flags (plugin, GS_PLUGIN_FLAGS_INTERACTIVE))
		return FALSE;

	g_debug ("Authentication required for remote '%s'", remote);

	/* Allow hard overrides with $BROWSER */
	browser = g_getenv ("BROWSER");
	if (browser != NULL) {
		const char *args[3] = { NULL, url, NULL };
		args[0] = browser;
		if (!g_spawn_async (NULL, (char **)args, NULL, G_SPAWN_SEARCH_PATH,
		                    NULL, NULL, NULL, &error_local)) {
			g_autoptr(GsPluginEvent) event = NULL;

			g_warning ("Failed to start browser %s: %s", browser, error_local->message);

			event = gs_plugin_event_new ();
			gs_flatpak_error_convert (&error_local);
			gs_plugin_event_set_error (event, error_local);
			gs_plugin_event_add_flag (event, GS_PLUGIN_EVENT_FLAG_WARNING);
			gs_plugin_report_event (plugin, event);

			return FALSE;
		}
	} else {
		if (!g_app_info_launch_default_for_uri (url, NULL, &error_local)) {
			g_autoptr(GsPluginEvent) event = NULL;

			g_warning ("Failed to show url: %s", error_local->message);

			event = gs_plugin_event_new ();
			gs_flatpak_error_convert (&error_local);
			gs_plugin_event_set_error (event, error_local);
			gs_plugin_event_add_flag (event, GS_PLUGIN_EVENT_FLAG_WARNING);
			gs_plugin_report_event (plugin, event);

			return FALSE;
		}
	}

	g_debug ("Waiting for browser...");

	return TRUE;
}

static void
_webflow_done (FlatpakTransaction *transaction,
               GVariant *options,
               guint id,
               GsPlugin *plugin)
{
	g_debug ("Browser done");
}
#endif

static FlatpakTransaction *
_build_transaction (GsPlugin *plugin, GsFlatpak *flatpak,
		    GCancellable *cancellable, GError **error)
{
	FlatpakInstallation *installation;
#if !FLATPAK_CHECK_VERSION(1, 7, 3)
	g_autoptr(GFile) installation_path = NULL;
#endif  /* flatpak < 1.7.3 */
	g_autoptr(FlatpakInstallation) installation_clone = NULL;
	g_autoptr(FlatpakTransaction) transaction = NULL;

	installation = gs_flatpak_get_installation (flatpak);

#if !FLATPAK_CHECK_VERSION(1, 7, 3)
	/* Operate on a copy of the installation so we can set the interactive
	 * flag for the duration of this transaction. */
	installation_path = flatpak_installation_get_path (installation);
	installation_clone = flatpak_installation_new_for_path (installation_path,
								flatpak_installation_get_is_user (installation),
								cancellable, error);
	if (installation_clone == NULL)
		return NULL;

	/* Let flatpak know if it is a background operation */
	flatpak_installation_set_no_interaction (installation_clone,
						 !gs_plugin_has_flags (plugin, GS_PLUGIN_FLAGS_INTERACTIVE));
#else  /* if flatpak ≥ 1.7.3 */
	installation_clone = g_object_ref (installation);
#endif  /* flatpak ≥ 1.7.3 */

	/* create transaction */
	transaction = gs_flatpak_transaction_new (installation_clone, cancellable, error);
	if (transaction == NULL) {
		g_prefix_error (error, "failed to build transaction: ");
		gs_flatpak_error_convert (error);
		return NULL;
	}

#if FLATPAK_CHECK_VERSION(1, 7, 3)
	/* Let flatpak know if it is a background operation */
	flatpak_transaction_set_no_interaction (transaction,
						!gs_plugin_has_flags (plugin, GS_PLUGIN_FLAGS_INTERACTIVE));
#endif  /* flatpak ≥ 1.7.3 */

	/* connect up signals */
	g_signal_connect (transaction, "ref-to-app",
			  G_CALLBACK (_ref_to_app), plugin);
#if FLATPAK_CHECK_VERSION(1,6,0)
	g_signal_connect (transaction, "basic-auth-start",
			  G_CALLBACK (_basic_auth_start), plugin);
	g_signal_connect (transaction, "webflow-start",
			  G_CALLBACK (_webflow_start), plugin);
	g_signal_connect (transaction, "webflow-done",
			  G_CALLBACK (_webflow_done), plugin);
#endif

	/* use system installations as dependency sources for user installations */
	flatpak_transaction_add_default_dependency_sources (transaction);

	return g_steal_pointer (&transaction);
}

static void
remove_schedule_entry (gpointer schedule_entry_handle)
{
	g_autoptr(GError) error_local = NULL;

	if (!gs_metered_remove_from_download_scheduler (schedule_entry_handle, NULL, &error_local))
		g_warning ("Failed to remove schedule entry: %s", error_local->message);
}

static gboolean
get_installation_dir_free_space (GsFlatpak *flatpak, guint64 *free_space, GError **error)
{
	FlatpakInstallation *installation;
	g_autoptr (GFile) installation_dir = NULL;
	g_autoptr (GFileInfo) info = NULL;

	installation = gs_flatpak_get_installation (flatpak);
	installation_dir = flatpak_installation_get_path (installation);

	info = g_file_query_filesystem_info (installation_dir,
					     G_FILE_ATTRIBUTE_FILESYSTEM_FREE,
					     NULL,
					     error);
	if (info == NULL)
		return FALSE;

	*free_space = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
	return TRUE;
}

static gboolean
gs_flatpak_has_space_to_install (GsFlatpak *flatpak, GsApp *app)
{
	g_autoptr(GError) error = NULL;
	guint64 free_space = 0;
	guint64 min_free_space = 0;
	guint64 space_required = 0;
	FlatpakInstallation *installation;

	space_required = gs_app_get_size_download (app);
	if (space_required == GS_APP_SIZE_UNKNOWABLE) {
		g_warning ("Failed to query download size: %s", gs_app_get_unique_id (app));
		space_required = 0;
	}

	installation = gs_flatpak_get_installation (flatpak);

	if (!flatpak_installation_get_min_free_space_bytes (installation, &min_free_space, &error)) {
		g_autoptr(GFile) installation_file = flatpak_installation_get_path (installation);
		g_autofree gchar *path = g_file_get_path (installation_file);
		g_warning ("Error getting min-free-space config value of OSTree repo at %s:%s", path, error->message);
		g_clear_error (&error);
	}
	space_required = space_required + min_free_space;

	if (!get_installation_dir_free_space (flatpak, &free_space, &error)) {
		g_warning ("Error getting the free space available for installing %s: %s",
			   gs_app_get_unique_id (app), error->message);
		g_clear_error (&error);
		/* Even if we fail to get free space, we don't want to block this user-intiated
		 * install action. It might happen that there is enough space to install but
		 * an error happened during querying the filesystem info. */
		return TRUE;
	}

	return free_space >= space_required;
}

static gboolean
gs_flatpak_has_space_to_update (GsFlatpak *flatpak, GsAppList *list, gboolean is_auto_update)
{
	g_autoptr(GError) error = NULL;
	guint64 free_space = 0;
	guint64 min_free_space = 0;
	guint64 space_required = 0;
	FlatpakInstallation *installation;

	if (is_auto_update) {
		for (guint i = 0; i < gs_app_list_length (list); i++) {
			GsApp *app_temp = gs_app_list_index (list, i);
			space_required += gs_app_get_size_installed (app_temp);
		}
	}

	installation = gs_flatpak_get_installation (flatpak);

	if (!flatpak_installation_get_min_free_space_bytes (installation, &min_free_space, &error)) {
		g_autoptr(GFile) installation_file = flatpak_installation_get_path (installation);
		g_autofree gchar *path = g_file_get_path (installation_file);
		g_warning ("Error getting min-free-space config value of OSTree repo at %s:%s", path, error->message);
		g_clear_error (&error);
	}
	space_required = space_required + min_free_space;
	if (!get_installation_dir_free_space (flatpak, &free_space, &error)) {
		g_warning ("Error getting the free space available for updating an app list: %s",
			   error->message);
		g_clear_error (&error);
		/* Only fail autoupdates if we cannot query the filesystem info.
		 * Manual updates follow the pattern similar to install's case. */
		return is_auto_update ? FALSE : TRUE;
	}

	return free_space >= space_required;
}

gboolean
gs_plugin_download (GsPlugin *plugin, GsAppList *list,
		    GCancellable *cancellable, GError **error)
{
	gboolean is_auto_update = !gs_plugin_has_flags (plugin, GS_PLUGIN_FLAGS_INTERACTIVE);
	g_autoptr(GHashTable) applist_by_flatpaks = NULL;
	GHashTableIter iter;
	gpointer key, value;

	/* build and run transaction for each flatpak installation */
	applist_by_flatpaks = _group_apps_by_installation (plugin, list);
	g_hash_table_iter_init (&iter, applist_by_flatpaks);
	while (g_hash_table_iter_next (&iter, &key, &value)) {
		GsFlatpak *flatpak = GS_FLATPAK (key);
		GsAppList *list_tmp = GS_APP_LIST (value);
		g_autoptr(FlatpakTransaction) transaction = NULL;
		gpointer schedule_entry_handle = NULL;

		g_assert (GS_IS_FLATPAK (flatpak));
		g_assert (list_tmp != NULL);
		g_assert (gs_app_list_length (list_tmp) > 0);

		if (!gs_plugin_has_flags (plugin, GS_PLUGIN_FLAGS_INTERACTIVE)) {
			g_autoptr(GError) error_local = NULL;

			if (!gs_metered_block_app_list_on_download_scheduler (list_tmp, &schedule_entry_handle, cancellable, &error_local)) {
				g_warning ("Failed to block on download scheduler: %s",
					   error_local->message);
				g_clear_error (&error_local);
			}
		}

		/* Is there enough disk space to download updates in this
		 * installation?
		 */
		if (!gs_flatpak_has_space_to_update (flatpak, list_tmp, is_auto_update)) {
			g_debug ("Skipping %s for %s: not enough space on disk",
				 (is_auto_update ? "automatic update" : "update"),
				 gs_flatpak_get_id (flatpak));
			if (is_auto_update) {
				/* If we're performing automatic updates in the
				 * background, don't return an error: we don't
				 * want an error banner showing up out of the
				 * blue. Continue to the next installation (if
				 * any).
				 */
				continue;
			}
			g_set_error (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_NO_SPACE,
				     _("You don’t have enough space to update these apps. Please remove apps or documents to create more space."));
			return FALSE;
		}

		/* build and run non-deployed transaction */
		transaction = _build_transaction (plugin, flatpak, cancellable, error);
		if (transaction == NULL) {
			gs_flatpak_error_convert (error);
			return FALSE;
		}
#if !FLATPAK_CHECK_VERSION(1,5,1)
		gs_flatpak_transaction_set_no_deploy (transaction, TRUE);
#else
		flatpak_transaction_set_no_deploy (transaction, TRUE);
#endif
		for (guint i = 0; i < gs_app_list_length (list_tmp); i++) {
			GsApp *app = gs_app_list_index (list_tmp, i);
			g_autofree gchar *ref = NULL;
			g_autoptr(GError) error_local = NULL;

			ref = gs_flatpak_app_get_ref_display (app);
			if (flatpak_transaction_add_update (transaction, ref, NULL, NULL, &error_local))
				continue;

			/* Errors about missing remotes are not fatal, as that’s
			 * a not-uncommon situation. */
			if (g_error_matches (error_local, FLATPAK_ERROR, FLATPAK_ERROR_REMOTE_NOT_FOUND)) {
				g_autoptr(GsPluginEvent) event = NULL;

				g_warning ("Skipping update for ‘%s’: %s", ref, error_local->message);

				event = gs_plugin_event_new ();
				gs_flatpak_error_convert (&error_local);
				gs_plugin_event_set_error (event, error_local);
				gs_plugin_event_add_flag (event, GS_PLUGIN_EVENT_FLAG_WARNING);
				gs_plugin_report_event (plugin, event);
			} else {
				gs_flatpak_error_convert (&error_local);
				g_propagate_error (error, g_steal_pointer (&error_local));
				return FALSE;
			}
		}

		if (!gs_flatpak_transaction_run (transaction, cancellable, error)) {
			gs_flatpak_error_convert (error);
			remove_schedule_entry (schedule_entry_handle);
			return FALSE;
		}

		remove_schedule_entry (schedule_entry_handle);

		/* Traverse over the GsAppList again and set that the update has been already downloaded
		 * for the apps. */
		for (guint i = 0; i < gs_app_list_length (list_tmp); i++) {
			GsApp *app = gs_app_list_index (list_tmp, i);
			gs_app_set_is_update_downloaded (app, TRUE);
		}
	}

	return TRUE;
}

static void
gs_flatpak_cover_addons_in_transaction (GsPlugin *plugin,
					FlatpakTransaction *transaction,
					GsApp *parent_app,
					GsAppState state)
{
	GsAppList *addons;
	g_autoptr(GString) errors = NULL;
	guint ii, sz;

	g_return_if_fail (transaction != NULL);
	g_return_if_fail (GS_IS_APP (parent_app));

	addons = gs_app_get_addons (parent_app);
	sz = addons ? gs_app_list_length (addons) : 0;

	for (ii = 0; ii < sz; ii++) {
		GsApp *addon = gs_app_list_index (addons, ii);
		g_autoptr(GError) local_error = NULL;

		if (state == GS_APP_STATE_INSTALLING && gs_app_get_to_be_installed (addon)) {
			g_autofree gchar *ref = NULL;

			ref = gs_flatpak_app_get_ref_display (addon);
			if (flatpak_transaction_add_install (transaction, gs_app_get_origin (addon), ref, NULL, &local_error)) {
				gs_app_set_state (addon, state);
			} else {
				if (errors)
					g_string_append_c (errors, '\n');
				else
					errors = g_string_new (NULL);
				g_string_append_printf (errors, _("Failed to add to install for addon ‘%s’: %s"),
					gs_app_get_name (addon), local_error->message);
			}
		} else if (state == GS_APP_STATE_REMOVING && gs_app_get_state (addon) == GS_APP_STATE_INSTALLED) {
			g_autofree gchar *ref = NULL;

			ref = gs_flatpak_app_get_ref_display (addon);
			if (flatpak_transaction_add_uninstall (transaction, ref, &local_error)) {
				gs_app_set_state (addon, state);
			} else {
				if (errors)
					g_string_append_c (errors, '\n');
				else
					errors = g_string_new (NULL);
				g_string_append_printf (errors, _("Failed to add to uninstall for addon ‘%s’: %s"),
					gs_app_get_name (addon), local_error->message);
			}
		}
	}

	if (errors) {
		g_autoptr(GsPluginEvent) event = NULL;
		g_autoptr(GError) error_local = g_error_new_literal (GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_FAILED,
			errors->str);

		event = gs_plugin_event_new ();
		gs_plugin_event_set_error (event, error_local);
		gs_plugin_event_add_flag (event, GS_PLUGIN_EVENT_FLAG_WARNING);
		gs_plugin_report_event (plugin, event);
	}
}

gboolean
gs_plugin_app_remove (GsPlugin *plugin,
		      GsApp *app,
		      GCancellable *cancellable,
		      GError **error)
{
	GsFlatpak *flatpak;
	g_autoptr(FlatpakTransaction) transaction = NULL;
	g_autofree gchar *ref = NULL;

	/* not supported */
	flatpak = gs_plugin_flatpak_get_handler (plugin, app);
	if (flatpak == NULL)
		return TRUE;

	/* is a source, handled by dedicated function */
	g_return_val_if_fail (gs_app_get_kind (app) != AS_COMPONENT_KIND_REPOSITORY, FALSE);

	/* build and run transaction */
	transaction = _build_transaction (plugin, flatpak, cancellable, error);
	if (transaction == NULL) {
		gs_flatpak_error_convert (error);
		return FALSE;
	}

	/* add to the transaction cache for quick look up -- other unrelated
	 * refs will be matched using gs_plugin_flatpak_find_app_by_ref() */
	gs_flatpak_transaction_add_app (transaction, app);

	ref = gs_flatpak_app_get_ref_display (app);
	if (!flatpak_transaction_add_uninstall (transaction, ref, error)) {
		gs_flatpak_error_convert (error);
		return FALSE;
	}

	gs_flatpak_cover_addons_in_transaction (plugin, transaction, app, GS_APP_STATE_REMOVING);

	/* run transaction */
	gs_app_set_state (app, GS_APP_STATE_REMOVING);
	if (!gs_flatpak_transaction_run (transaction, cancellable, error)) {
		gs_flatpak_error_convert (error);
		gs_app_set_state_recover (app);
		return FALSE;
	}

	/* get any new state */
	if (!gs_flatpak_refresh (flatpak, G_MAXUINT, cancellable, error)) {
		gs_flatpak_error_convert (error);
		return FALSE;
	}
	if (!gs_flatpak_refine_app (flatpak, app,
				    GS_PLUGIN_REFINE_FLAGS_DEFAULT,
				    cancellable, error)) {
		g_prefix_error (error, "failed to run refine for %s: ", ref);
		gs_flatpak_error_convert (error);
		return FALSE;
	}

	gs_flatpak_refine_addons (flatpak, app, GS_PLUGIN_REFINE_FLAGS_DEFAULT, GS_APP_STATE_REMOVING, cancellable);

	return TRUE;
}

static gboolean
app_has_local_source (GsApp *app)
{
	const gchar *url = gs_app_get_origin_hostname (app);

	if (gs_flatpak_app_get_file_kind (app) == GS_FLATPAK_APP_FILE_KIND_BUNDLE)
		return TRUE;

	if (gs_flatpak_app_get_file_kind (app) == GS_FLATPAK_APP_FILE_KIND_REF &&
	    g_strcmp0 (url, "localhost") == 0)
		return TRUE;

	return FALSE;
}

static void
gs_plugin_flatpak_ensure_scope (GsPlugin *plugin,
				GsApp *app)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);

	if (gs_app_get_scope (app) == AS_COMPONENT_SCOPE_UNKNOWN) {
		g_autoptr(GSettings) settings = g_settings_new ("org.gnome.software");

		/* get the new GsFlatpak for handling of local files */
		gs_app_set_scope (app, g_settings_get_boolean (settings, "install-bundles-system-wide") ?
					AS_COMPONENT_SCOPE_SYSTEM : AS_COMPONENT_SCOPE_USER);
		if (!priv->has_system_helper) {
			g_info ("no flatpak system helper is available, using user");
			gs_app_set_scope (app, AS_COMPONENT_SCOPE_USER);
		}
		if (priv->destdir_for_tests != NULL) {
			g_debug ("in self tests, using user");
			gs_app_set_scope (app, AS_COMPONENT_SCOPE_USER);
		}
	}
}

gboolean
gs_plugin_app_install (GsPlugin *plugin,
		       GsApp *app,
		       GCancellable *cancellable,
		       GError **error)
{
	GsFlatpak *flatpak;
	g_autoptr(FlatpakTransaction) transaction = NULL;
	g_autoptr(GError) error_local = NULL;
	gpointer schedule_entry_handle = NULL;
	gboolean already_installed = FALSE;

	/* queue for install if installation needs the network */
	if (!app_has_local_source (app) &&
	    !gs_plugin_get_network_available (plugin)) {
		gs_app_set_state (app, GS_APP_STATE_QUEUED_FOR_INSTALL);
		return TRUE;
	}

	/* set the app scope */
	gs_plugin_flatpak_ensure_scope (plugin, app);

	/* not supported */
	flatpak = gs_plugin_flatpak_get_handler (plugin, app);
	if (flatpak == NULL)
		return TRUE;

	/* is a source, handled by dedicated function */
	g_return_val_if_fail (gs_app_get_kind (app) != AS_COMPONENT_KIND_REPOSITORY, FALSE);

	/* build */
	transaction = _build_transaction (plugin, flatpak, cancellable, error);
	if (transaction == NULL) {
		gs_flatpak_error_convert (error);
		return FALSE;
	}

	/* Is there enough disk space free to install? */
	if (!gs_flatpak_has_space_to_install (flatpak, app)) {
		g_debug ("Skipping installation for %s: not enough space on disk",
			 gs_app_get_unique_id (app));
		gs_app_set_state_recover (app);
		g_set_error (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_NO_SPACE,
			     _("You don’t have enough space to install %s. Please remove apps or documents to create more space."),
			     gs_app_get_unique_id (app));
		return FALSE;
	}

	/* add to the transaction cache for quick look up -- other unrelated
	 * refs will be matched using gs_plugin_flatpak_find_app_by_ref() */
	gs_flatpak_transaction_add_app (transaction, app);

	/* add flatpakref */
	if (gs_flatpak_app_get_file_kind (app) == GS_FLATPAK_APP_FILE_KIND_REF) {
		GFile *file = gs_app_get_local_file (app);
		g_autoptr(GBytes) blob = NULL;
		if (file == NULL) {
			g_set_error (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_NOT_SUPPORTED,
				     "no local file set for bundle %s",
				     gs_app_get_unique_id (app));
			return FALSE;
		}
		blob = g_file_load_bytes (file, cancellable, NULL, error);
		if (blob == NULL) {
			gs_flatpak_error_convert (error);
			return FALSE;
		}
		if (!flatpak_transaction_add_install_flatpakref (transaction, blob, error)) {
			gs_flatpak_error_convert (error);
			return FALSE;
		}

	/* add bundle */
	} else if (gs_flatpak_app_get_file_kind (app) == GS_FLATPAK_APP_FILE_KIND_BUNDLE) {
		GFile *file = gs_app_get_local_file (app);
		if (file == NULL) {
			g_set_error (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_NOT_SUPPORTED,
				     "no local file set for bundle %s",
				     gs_app_get_unique_id (app));
			return FALSE;
		}
		if (!flatpak_transaction_add_install_bundle (transaction, file,
							     NULL, error)) {
			gs_flatpak_error_convert (error);
			return FALSE;
		}

	/* add normal ref */
	} else {
		g_autofree gchar *ref = gs_flatpak_app_get_ref_display (app);
		if (!flatpak_transaction_add_install (transaction,
						      gs_app_get_origin (app),
						      ref, NULL, &error_local)) {
			/* Somehow, the app might already be installed. */
			if (g_error_matches (error_local, FLATPAK_ERROR,
					     FLATPAK_ERROR_ALREADY_INSTALLED)) {
				already_installed = TRUE;
				g_clear_error (&error_local);
			} else {
				g_propagate_error (error, g_steal_pointer (&error_local));
				gs_flatpak_error_convert (error);
				return FALSE;
			}
		}
	}

	gs_flatpak_cover_addons_in_transaction (plugin, transaction, app, GS_APP_STATE_INSTALLING);

	if (!gs_plugin_has_flags (plugin, GS_PLUGIN_FLAGS_INTERACTIVE)) {
		/* FIXME: Add additional details here, especially the download
		 * size bounds (using `size-minimum` and `size-maximum`, both
		 * type `t`). */
		if (!gs_metered_block_app_on_download_scheduler (app, &schedule_entry_handle, cancellable, &error_local)) {
			g_warning ("Failed to block on download scheduler: %s",
				   error_local->message);
			g_clear_error (&error_local);
		}
	}

	/* run transaction */
	if (!already_installed) {
		gs_app_set_state (app, GS_APP_STATE_INSTALLING);
		if (!gs_flatpak_transaction_run (transaction, cancellable, &error_local)) {
			/* Somehow, the app might already be installed. */
			if (g_error_matches (error_local, FLATPAK_ERROR,
					     FLATPAK_ERROR_ALREADY_INSTALLED)) {
				already_installed = TRUE;
				g_clear_error (&error_local);
			} else {
				g_propagate_error (error, g_steal_pointer (&error_local));
				gs_flatpak_error_convert (error);
				gs_app_set_state_recover (app);
				remove_schedule_entry (schedule_entry_handle);
				return FALSE;
			}
		}
	}

	if (already_installed) {
		/* Set the app back to UNKNOWN so that refining it gets all the right details. */
		g_debug ("App %s is already installed", gs_app_get_unique_id (app));
		gs_app_set_state (app, GS_APP_STATE_UNKNOWN);
	}

	remove_schedule_entry (schedule_entry_handle);

	/* get any new state */
	if (!gs_flatpak_refresh (flatpak, G_MAXUINT, cancellable, error)) {
		gs_flatpak_error_convert (error);
		return FALSE;
	}
	if (!gs_flatpak_refine_app (flatpak, app,
				    GS_PLUGIN_REFINE_FLAGS_DEFAULT,
				    cancellable, error)) {
		g_prefix_error (error, "failed to run refine for %s: ",
				gs_app_get_unique_id (app));
		gs_flatpak_error_convert (error);
		return FALSE;
	}

	gs_flatpak_refine_addons (flatpak, app, GS_PLUGIN_REFINE_FLAGS_DEFAULT, GS_APP_STATE_INSTALLING, cancellable);

	return TRUE;
}

static gboolean
gs_plugin_flatpak_update (GsPlugin *plugin,
			  GsFlatpak *flatpak,
			  GsAppList *list_tmp,
			  GCancellable *cancellable,
			  GError **error)
{
	g_autoptr(FlatpakTransaction) transaction = NULL;
	gboolean is_update_downloaded = TRUE;
	gpointer schedule_entry_handle = NULL;
	gboolean is_auto_update;

	/* Is there enough disk space to download updates in this
	 * installation?
	 */
	is_auto_update = !gs_plugin_has_flags (plugin, GS_PLUGIN_FLAGS_INTERACTIVE);
	if (!gs_flatpak_has_space_to_update (flatpak, list_tmp, is_auto_update)) {
		g_debug ("Skipping %s for %s: not enough space on disk",
			 (is_auto_update ? "automatic update" : "update"),
			 gs_flatpak_get_id (flatpak));
		if (is_auto_update) {
			/* If we're performing automatic updates in the
			 * background, don't return an error: we don't want an
			 * error banner showing up out of the blue. Continue to
			 * the next installation (if any).
			 */
			return TRUE;
		}
		g_set_error (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_NO_SPACE,
			     _("You don’t have enough space to update these apps. Please remove apps or documents to create more space."));
		return FALSE;
	}

	if (!gs_plugin_has_flags (plugin, GS_PLUGIN_FLAGS_INTERACTIVE)) {
		g_autoptr(GError) error_local = NULL;

		if (!gs_metered_block_app_list_on_download_scheduler (list_tmp, &schedule_entry_handle, cancellable, &error_local)) {
			g_warning ("Failed to block on download scheduler: %s",
				   error_local->message);
			g_clear_error (&error_local);
		}
	}

	/* build and run transaction */
	transaction = _build_transaction (plugin, flatpak, cancellable, error);
	if (transaction == NULL) {
		gs_flatpak_error_convert (error);
		return FALSE;
	}

	for (guint i = 0; i < gs_app_list_length (list_tmp); i++) {
		GsApp *app = gs_app_list_index (list_tmp, i);
		g_autofree gchar *ref = NULL;
		g_autoptr(GError) error_local = NULL;

		ref = gs_flatpak_app_get_ref_display (app);
		if (flatpak_transaction_add_update (transaction, ref, NULL, NULL, error)) {
			/* add to the transaction cache for quick look up -- other unrelated
			 * refs will be matched using gs_plugin_flatpak_find_app_by_ref() */
			gs_flatpak_transaction_add_app (transaction, app);

			continue;
		}

		/* Errors about missing remotes are not fatal, as that’s
		 * a not-uncommon situation. */
		if (g_error_matches (error_local, FLATPAK_ERROR, FLATPAK_ERROR_REMOTE_NOT_FOUND)) {
			g_autoptr(GsPluginEvent) event = NULL;

			g_warning ("Skipping update for ‘%s’: %s", ref, error_local->message);

			event = gs_plugin_event_new ();
			gs_flatpak_error_convert (&error_local);
			gs_plugin_event_set_error (event, error_local);
			gs_plugin_event_add_flag (event, GS_PLUGIN_EVENT_FLAG_WARNING);
			gs_plugin_report_event (plugin, event);
		} else {
			gs_flatpak_error_convert (&error_local);
			g_propagate_error (error, g_steal_pointer (&error_local));
			return FALSE;
		}
	}

	/* run transaction */
	for (guint i = 0; i < gs_app_list_length (list_tmp); i++) {
		GsApp *app = gs_app_list_index (list_tmp, i);
		gs_app_set_state (app, GS_APP_STATE_INSTALLING);

		/* If all apps' update are previously downloaded and available locally,
		 * FlatpakTransaction should run with no-pull flag. This is the case
		 * for apps' autoupdates. */
		is_update_downloaded &= gs_app_get_is_update_downloaded (app);
	}

	if (is_update_downloaded) {
		flatpak_transaction_set_no_pull (transaction, TRUE);
	}

#if FLATPAK_CHECK_VERSION(1, 9, 1)
	/* automatically clean up unused EOL runtimes when updating */
	flatpak_transaction_set_include_unused_uninstall_ops (transaction, TRUE);
#endif

	if (!gs_flatpak_transaction_run (transaction, cancellable, error)) {
		for (guint i = 0; i < gs_app_list_length (list_tmp); i++) {
			GsApp *app = gs_app_list_index (list_tmp, i);
			gs_app_set_state_recover (app);
		}
		gs_flatpak_error_convert (error);
		remove_schedule_entry (schedule_entry_handle);
		return FALSE;
	} else {
		/* Reset the state to have it updated */
		for (guint i = 0; i < gs_app_list_length (list_tmp); i++) {
			GsApp *app = gs_app_list_index (list_tmp, i);
			gs_app_set_state (app, GS_APP_STATE_UNKNOWN);
		}
	}

	remove_schedule_entry (schedule_entry_handle);
	gs_plugin_updates_changed (plugin);

	/* get any new state */
	if (!gs_flatpak_refresh (flatpak, G_MAXUINT, cancellable, error)) {
		gs_flatpak_error_convert (error);
		return FALSE;
	}
	for (guint i = 0; i < gs_app_list_length (list_tmp); i++) {
		GsApp *app = gs_app_list_index (list_tmp, i);
		g_autofree gchar *ref = NULL;

		ref = gs_flatpak_app_get_ref_display (app);
		if (!gs_flatpak_refine_app (flatpak, app,
					    GS_PLUGIN_REFINE_FLAGS_REQUIRE_RUNTIME,
					    cancellable, error)) {
			g_prefix_error (error, "failed to run refine for %s: ", ref);
			gs_flatpak_error_convert (error);
			return FALSE;
		}
	}
	return TRUE;
}

gboolean
gs_plugin_update (GsPlugin *plugin,
                  GsAppList *list,
                  GCancellable *cancellable,
                  GError **error)
{
	g_autoptr(GHashTable) applist_by_flatpaks = NULL;
	GHashTableIter iter;
	gpointer key, value;

	/* build and run transaction for each flatpak installation */
	applist_by_flatpaks = _group_apps_by_installation (plugin, list);
	g_hash_table_iter_init (&iter, applist_by_flatpaks);
	while (g_hash_table_iter_next (&iter, &key, &value)) {
		GsFlatpak *flatpak = GS_FLATPAK (key);
		GsAppList *list_tmp = GS_APP_LIST (value);
		gboolean success;

		g_assert (GS_IS_FLATPAK (flatpak));
		g_assert (list_tmp != NULL);
		g_assert (gs_app_list_length (list_tmp) > 0);

		gs_flatpak_set_busy (flatpak, TRUE);
		success = gs_plugin_flatpak_update (plugin, flatpak, list_tmp, cancellable, error);
		gs_flatpak_set_busy (flatpak, FALSE);
		if (!success)
			return FALSE;
	}
	return TRUE;
}

static GsApp *
gs_plugin_flatpak_file_to_app_repo (GsPlugin *plugin,
				    GFile *file,
				    GCancellable *cancellable,
				    GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	g_autoptr(GsApp) app = NULL;

	/* parse the repo file */
	app = gs_flatpak_app_new_from_repo_file (file, cancellable, error);
	if (app == NULL)
		return NULL;

	/* already exists */
	for (guint i = 0; i < priv->flatpaks->len; i++) {
		GsFlatpak *flatpak = g_ptr_array_index (priv->flatpaks, i);
		g_autoptr(GError) error_local = NULL;
		g_autoptr(GsApp) app_tmp = NULL;
		app_tmp = gs_flatpak_find_source_by_url (flatpak,
							 gs_flatpak_app_get_repo_url (app),
							 cancellable, &error_local);
		if (app_tmp == NULL) {
			g_debug ("%s", error_local->message);
			continue;
		}
		if (g_strcmp0 (gs_flatpak_app_get_repo_filter (app), gs_flatpak_app_get_repo_filter (app_tmp)) != 0)
			continue;
		return g_steal_pointer (&app_tmp);
	}

	/* this is new */
	gs_app_set_management_plugin (app, gs_plugin_get_name (plugin));
	return g_steal_pointer (&app);
}

static GsFlatpak *
gs_plugin_flatpak_create_temporary (GsPlugin *plugin, GCancellable *cancellable, GError **error)
{
	g_autofree gchar *installation_path = NULL;
	g_autoptr(FlatpakInstallation) installation = NULL;
	g_autoptr(GFile) installation_file = NULL;

	/* create new per-user installation in a cache dir */
	installation_path = gs_utils_get_cache_filename ("flatpak",
							 "installation-tmp",
							 GS_UTILS_CACHE_FLAG_WRITEABLE |
							 GS_UTILS_CACHE_FLAG_ENSURE_EMPTY |
							 GS_UTILS_CACHE_FLAG_CREATE_DIRECTORY,
							 error);
	if (installation_path == NULL)
		return NULL;
	installation_file = g_file_new_for_path (installation_path);
	installation = flatpak_installation_new_for_path (installation_file,
							  TRUE, /* user */
							  cancellable,
							  error);
	if (installation == NULL) {
		gs_flatpak_error_convert (error);
		return NULL;
	}
	return gs_flatpak_new (plugin, installation, GS_FLATPAK_FLAG_IS_TEMPORARY);
}

static GsApp *
gs_plugin_flatpak_file_to_app_bundle (GsPlugin *plugin,
				      GFile *file,
				      GCancellable *cancellable,
				      GError **error)
{
	g_autofree gchar *ref = NULL;
	g_autoptr(GsApp) app = NULL;
	g_autoptr(GsApp) app_tmp = NULL;
	g_autoptr(GsFlatpak) flatpak_tmp = NULL;

	/* only use the temporary GsFlatpak to avoid the auth dialog */
	flatpak_tmp = gs_plugin_flatpak_create_temporary (plugin, cancellable, error);
	if (flatpak_tmp == NULL)
		return NULL;

	/* add object */
	app = gs_flatpak_file_to_app_bundle (flatpak_tmp, file, cancellable, error);
	if (app == NULL)
		return NULL;

	/* is this already installed or available in a configured remote */
	ref = gs_flatpak_app_get_ref_display (app);
	app_tmp = gs_plugin_flatpak_find_app_by_ref (plugin, ref, cancellable, NULL);
	if (app_tmp != NULL)
		return g_steal_pointer (&app_tmp);

	/* force this to be 'any' scope for installation */
	gs_app_set_scope (app, AS_COMPONENT_SCOPE_UNKNOWN);

	/* this is new */
	return g_steal_pointer (&app);
}

static GsApp *
gs_plugin_flatpak_file_to_app_ref (GsPlugin *plugin,
				   GFile *file,
				   GCancellable *cancellable,
				   GError **error)
{
	GsApp *runtime;
	g_autofree gchar *ref = NULL;
	g_autoptr(GsApp) app = NULL;
	g_autoptr(GsApp) app_tmp = NULL;
	g_autoptr(GsFlatpak) flatpak_tmp = NULL;

	/* only use the temporary GsFlatpak to avoid the auth dialog */
	flatpak_tmp = gs_plugin_flatpak_create_temporary (plugin, cancellable, error);
	if (flatpak_tmp == NULL)
		return NULL;

	/* add object */
	app = gs_flatpak_file_to_app_ref (flatpak_tmp, file, cancellable, error);
	if (app == NULL)
		return NULL;

	/* is this already installed or available in a configured remote */
	ref = gs_flatpak_app_get_ref_display (app);
	app_tmp = gs_plugin_flatpak_find_app_by_ref (plugin, ref, cancellable, NULL);
	if (app_tmp != NULL)
		return g_steal_pointer (&app_tmp);

	/* force this to be 'any' scope for installation */
	gs_app_set_scope (app, AS_COMPONENT_SCOPE_UNKNOWN);

	/* do we have a system runtime available */
	runtime = gs_app_get_runtime (app);
	if (runtime != NULL) {
		g_autoptr(GsApp) runtime_tmp = NULL;
		g_autofree gchar *runtime_ref = gs_flatpak_app_get_ref_display (runtime);
		runtime_tmp = gs_plugin_flatpak_find_app_by_ref (plugin,
								 runtime_ref,
								 cancellable,
								 NULL);
		if (runtime_tmp != NULL) {
			gs_app_set_runtime (app, runtime_tmp);
		} else {
			/* the new runtime is available from the RuntimeRepo */
			if (gs_flatpak_app_get_runtime_url (runtime) != NULL)
				gs_app_set_state (runtime, GS_APP_STATE_AVAILABLE_LOCAL);
		}
	}

	/* this is new */
	return g_steal_pointer (&app);
}

gboolean
gs_plugin_file_to_app (GsPlugin *plugin,
		       GsAppList *list,
		       GFile *file,
		       GCancellable *cancellable,
		       GError **error)
{
	g_autofree gchar *content_type = NULL;
	g_autoptr(GsApp) app = NULL;
	const gchar *mimetypes_bundle[] = {
		"application/vnd.flatpak",
		NULL };
	const gchar *mimetypes_repo[] = {
		"application/vnd.flatpak.repo",
		NULL };
	const gchar *mimetypes_ref[] = {
		"application/vnd.flatpak.ref",
		NULL };

	/* does this match any of the mimetypes we support */
	content_type = gs_utils_get_content_type (file, cancellable, error);
	if (content_type == NULL)
		return FALSE;
	if (g_strv_contains (mimetypes_bundle, content_type)) {
		app = gs_plugin_flatpak_file_to_app_bundle (plugin, file,
							    cancellable, error);
		if (app == NULL)
			return FALSE;
	} else if (g_strv_contains (mimetypes_repo, content_type)) {
		app = gs_plugin_flatpak_file_to_app_repo (plugin, file,
							  cancellable, error);
		if (app == NULL)
			return FALSE;
	} else if (g_strv_contains (mimetypes_ref, content_type)) {
		app = gs_plugin_flatpak_file_to_app_ref (plugin, file,
							 cancellable, error);
		if (app == NULL)
			return FALSE;
	}
	if (app != NULL)
		gs_app_list_add (list, app);
	return TRUE;
}

static gboolean
gs_plugin_flatpak_do_search (GsPlugin *plugin,
			     gchar **values,
			     GsAppList *list,
			     GCancellable *cancellable,
			     GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	for (guint i = 0; i < priv->flatpaks->len; i++) {
		GsFlatpak *flatpak = g_ptr_array_index (priv->flatpaks, i);
		if (!gs_flatpak_search (flatpak, (const gchar * const *) values, list,
					cancellable, error)) {
			return FALSE;
		}
	}

	return TRUE;
}

gboolean
gs_plugin_add_search (GsPlugin *plugin,
		      gchar **values,
		      GsAppList *list,
		      GCancellable *cancellable,
		      GError **error)
{
	return gs_plugin_flatpak_do_search (plugin, values, list, cancellable, error);
}

gboolean
gs_plugin_add_search_what_provides (GsPlugin *plugin,
				    gchar **search,
				    GsAppList *list,
				    GCancellable *cancellable,
				    GError **error)
{
	return gs_plugin_flatpak_do_search (plugin, search, list, cancellable, error);
}

gboolean
gs_plugin_add_categories (GsPlugin *plugin,
			  GPtrArray *list,
			  GCancellable *cancellable,
			  GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	for (guint i = 0; i < priv->flatpaks->len; i++) {
		GsFlatpak *flatpak = g_ptr_array_index (priv->flatpaks, i);
		if (!gs_flatpak_add_categories (flatpak, list, cancellable, error))
			return FALSE;
	}
	return TRUE;
}

gboolean
gs_plugin_add_category_apps (GsPlugin *plugin,
			     GsCategory *category,
			     GsAppList *list,
			     GCancellable *cancellable,
			     GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	for (guint i = 0; i < priv->flatpaks->len; i++) {
		GsFlatpak *flatpak = g_ptr_array_index (priv->flatpaks, i);
		if (!gs_flatpak_add_category_apps (flatpak,
						   category,
						   list,
						   cancellable,
						   error)) {
			return FALSE;
		}
	}
	return TRUE;
}

gboolean
gs_plugin_add_popular (GsPlugin *plugin,
		       GsAppList *list,
		       GCancellable *cancellable,
		       GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	for (guint i = 0; i < priv->flatpaks->len; i++) {
		GsFlatpak *flatpak = g_ptr_array_index (priv->flatpaks, i);
		if (!gs_flatpak_add_popular (flatpak, list, cancellable, error))
			return FALSE;
	}
	return TRUE;
}

gboolean
gs_plugin_add_alternates (GsPlugin *plugin,
			  GsApp *app,
			  GsAppList *list,
			  GCancellable *cancellable,
			  GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	for (guint i = 0; i < priv->flatpaks->len; i++) {
		GsFlatpak *flatpak = g_ptr_array_index (priv->flatpaks, i);
		if (!gs_flatpak_add_alternates (flatpak, app, list, cancellable, error))
			return FALSE;
	}
	return TRUE;
}

gboolean
gs_plugin_add_featured (GsPlugin *plugin,
			GsAppList *list,
			GCancellable *cancellable,
			GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	for (guint i = 0; i < priv->flatpaks->len; i++) {
		GsFlatpak *flatpak = g_ptr_array_index (priv->flatpaks, i);
		if (!gs_flatpak_add_featured (flatpak, list, cancellable, error))
			return FALSE;
	}
	return TRUE;
}

gboolean
gs_plugin_add_recent (GsPlugin *plugin,
		      GsAppList *list,
		      guint64 age,
		      GCancellable *cancellable,
		      GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	for (guint i = 0; i < priv->flatpaks->len; i++) {
		GsFlatpak *flatpak = g_ptr_array_index (priv->flatpaks, i);
		if (!gs_flatpak_add_recent (flatpak, list, age, cancellable, error))
			return FALSE;
	}
	return TRUE;
}

gboolean
gs_plugin_url_to_app (GsPlugin *plugin,
		      GsAppList *list,
		      const gchar *url,
		      GCancellable *cancellable,
		      GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	for (guint i = 0; i < priv->flatpaks->len; i++) {
		GsFlatpak *flatpak = g_ptr_array_index (priv->flatpaks, i);
		if (!gs_flatpak_url_to_app (flatpak, list, url, cancellable, error))
			return FALSE;
	}
	return TRUE;
}

gboolean
gs_plugin_install_repo (GsPlugin *plugin,
			GsApp *repo,
			GCancellable *cancellable,
			GError **error)
{
	GsFlatpak *flatpak;

	/* queue for install if installation needs the network */
	if (!app_has_local_source (repo) &&
	    !gs_plugin_get_network_available (plugin)) {
		gs_app_set_state (repo, GS_APP_STATE_QUEUED_FOR_INSTALL);
		return TRUE;
	}

	gs_plugin_flatpak_ensure_scope (plugin, repo);

	flatpak = gs_plugin_flatpak_get_handler (plugin, repo);
	if (flatpak == NULL)
		return TRUE;

	/* is a source */
	g_return_val_if_fail (gs_app_get_kind (repo) == AS_COMPONENT_KIND_REPOSITORY, FALSE);

	return gs_flatpak_app_install_source (flatpak, repo, TRUE, cancellable, error);
}

gboolean
gs_plugin_remove_repo (GsPlugin *plugin,
		       GsApp *repo,
		       GCancellable *cancellable,
		       GError **error)
{
	GsFlatpak *flatpak;

	flatpak = gs_plugin_flatpak_get_handler (plugin, repo);
	if (flatpak == NULL)
		return TRUE;

	/* is a source */
	g_return_val_if_fail (gs_app_get_kind (repo) == AS_COMPONENT_KIND_REPOSITORY, FALSE);

	return gs_flatpak_app_remove_source (flatpak, repo, TRUE, cancellable, error);
}

gboolean
gs_plugin_enable_repo (GsPlugin *plugin,
		       GsApp *repo,
		       GCancellable *cancellable,
		       GError **error)
{
	GsFlatpak *flatpak;

	flatpak = gs_plugin_flatpak_get_handler (plugin, repo);
	if (flatpak == NULL)
		return TRUE;

	/* is a source */
	g_return_val_if_fail (gs_app_get_kind (repo) == AS_COMPONENT_KIND_REPOSITORY, FALSE);

	return gs_flatpak_app_install_source (flatpak, repo, FALSE, cancellable, error);
}

gboolean
gs_plugin_disable_repo (GsPlugin *plugin,
			GsApp *repo,
			GCancellable *cancellable,
			GError **error)
{
	GsFlatpak *flatpak;

	flatpak = gs_plugin_flatpak_get_handler (plugin, repo);
	if (flatpak == NULL)
		return TRUE;

	/* is a source */
	g_return_val_if_fail (gs_app_get_kind (repo) == AS_COMPONENT_KIND_REPOSITORY, FALSE);

	return gs_flatpak_app_remove_source (flatpak, repo, FALSE, cancellable, error);
}
