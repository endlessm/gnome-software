/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2016 Joaquim Rocha <jrocha@endlessm.com>
 * Copyright (C) 2016-2018 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2017-2018 Kalev Lember <klember@redhat.com>
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
_as_app_scope_is_compatible (AsAppScope scope1, AsAppScope scope2)
{
	if (scope1 == AS_APP_SCOPE_UNKNOWN)
		return TRUE;
	if (scope2 == AS_APP_SCOPE_UNKNOWN)
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
				  GError **local_error)
{
	g_autoptr(GsPluginEvent) event = gs_plugin_event_new ();
	gs_flatpak_error_convert (local_error);
	gs_plugin_event_set_error (event, *local_error);
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
		g_autoptr(GError) local_error = NULL;
		g_autoptr(FlatpakInstallation) installation = NULL;

		/* include the system installations */
		if (priv->has_system_helper) {
			installations = flatpak_get_system_installations (cancellable,
									  &local_error);
			if (installations == NULL) {
				gs_plugin_flatpak_report_warning (plugin, &local_error);
				g_clear_error (&local_error);
			}
		}

		/* include the user installation */
		installation = flatpak_installation_new_user (cancellable,
							      &local_error);
		if (installation == NULL) {
			/* if some error happened, report it as an event, but
			 * do not return it, otherwise it will disable the whole
			 * plugin (meaning that support for Flatpak will not be
			 * possible even if a system installation is working) */
			gs_plugin_flatpak_report_warning (plugin, &local_error);
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
		if (installation == NULL)
			return FALSE;

		installations = g_ptr_array_new_with_free_func (g_object_unref);
		g_ptr_array_add (installations, g_steal_pointer (&installation));
	}

	/* add the installations */
	for (guint i = 0; installations != NULL && i < installations->len; i++) {
		g_autoptr(GError) local_error = NULL;
		FlatpakInstallation *installation = g_ptr_array_index (installations, i);
		if (!gs_plugin_flatpak_add_installation (plugin,
							 installation,
							 cancellable,
							 &local_error)) {
			gs_plugin_flatpak_report_warning (plugin,
							  &local_error);
			continue;
		}
	}

	/* when no installation has been loaded, return the error so the
	 * plugin gets disabled */
	if (priv->flatpaks->len == 0) {
		g_set_error (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_FAILED,
			     "Failed to load any Flatpak installation...");
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
		if (_as_app_scope_is_compatible (gs_flatpak_get_scope (flatpak),
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
	if (gs_app_get_scope (app) == AS_APP_SCOPE_UNKNOWN) {
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


gboolean
gs_plugin_refine_app (GsPlugin *plugin,
		      GsApp *app,
		      GsPluginRefineFlags flags,
		      GCancellable *cancellable,
		      GError **error)
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
	for (guint i = 0; i < gs_app_list_length (list); i++) {
		GsApp *app = gs_app_list_index (list, i);
		GsFlatpak *flatpak = gs_plugin_flatpak_get_handler (plugin, app);
		if (flatpak != NULL) {
			GsAppList *list_tmp = g_hash_table_lookup (applist_by_flatpaks, flatpak);
			if (list_tmp == NULL) {
				list_tmp = gs_app_list_new ();
				g_hash_table_insert (applist_by_flatpaks,
						     g_object_ref (flatpak),
						     list_tmp);
			}
			gs_app_list_add (list_tmp, app);
		}
	}

	return g_steal_pointer (&applist_by_flatpaks);
}

static FlatpakTransaction *
_build_transaction (GsPlugin *plugin, GsFlatpak *flatpak,
		    GCancellable *cancellable, GError **error)
{
	FlatpakInstallation *installation;
	g_autoptr(FlatpakTransaction) transaction = NULL;

	installation = gs_flatpak_get_installation (flatpak);

	/* Let flatpak know if it is a background operation */
	if (!gs_plugin_has_flags (plugin, GS_PLUGIN_FLAGS_INTERACTIVE))
		flatpak_installation_set_no_interaction (installation, TRUE);

	/* create transaction */
	transaction = gs_flatpak_transaction_new (installation, cancellable, error);
	if (transaction == NULL) {
		g_prefix_error (error, "failed to build transaction: ");
		gs_flatpak_error_convert (error);
		return NULL;
	}

	/* connect up signals */
	g_signal_connect (transaction, "ref-to-app",
			  G_CALLBACK (_ref_to_app), plugin);

	/* use system installations as dependency sources for user installations */
	flatpak_transaction_add_default_dependency_sources (transaction);

	return g_steal_pointer (&transaction);
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

static void
remove_schedule_entry (gpointer schedule_entry_handle)
{
	g_autoptr(GError) error_local = NULL;

	if (!gs_metered_remove_from_download_scheduler (schedule_entry_handle, NULL, &error_local))
		g_warning ("Failed to remove schedule entry: %s", error_local->message);
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

			ref = gs_flatpak_app_get_ref_display (app);
			if (!flatpak_transaction_add_update (transaction, ref, NULL, NULL, error)) {
				gs_flatpak_error_convert (error);
				return FALSE;
			}
		}

		if (!gs_plugin_has_flags (plugin, GS_PLUGIN_FLAGS_INTERACTIVE)) {
			g_autoptr(GError) error_local = NULL;

			if (!gs_metered_block_app_list_on_download_scheduler (list_tmp, &schedule_entry_handle, cancellable, &error_local)) {
				g_warning ("Failed to block on download scheduler: %s",
					   error_local->message);
				g_clear_error (&error_local);
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

	/* is a source */
	if (gs_app_get_kind (app) == AS_APP_KIND_SOURCE)
		return gs_flatpak_app_remove_source (flatpak, app, cancellable, error);

	/* build and run transaction */
	transaction = _build_transaction (plugin, flatpak, cancellable, error);
	if (transaction == NULL) {
		gs_flatpak_error_convert (error);
		return FALSE;
	}
	ref = gs_flatpak_app_get_ref_display (app);
	if (!flatpak_transaction_add_uninstall (transaction, ref, error)) {
		gs_flatpak_error_convert (error);
		return FALSE;
	}

	/* run transaction */
	gs_app_set_state (app, AS_APP_STATE_REMOVING);
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
	return TRUE;
}

static gboolean
app_has_local_source (GsApp *app)
{
	const gchar *url = gs_app_get_origin_hostname (app);
	return gs_app_has_category (app, "usb") ||
		(url != NULL && g_str_has_prefix (url, "file://"));
}

gboolean
gs_plugin_app_install (GsPlugin *plugin,
		       GsApp *app,
		       GCancellable *cancellable,
		       GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	GsFlatpak *flatpak;
	g_autoptr(FlatpakTransaction) transaction = NULL;
	g_autoptr(GError) error_local = NULL;
	gboolean already_installed = FALSE;
	gpointer schedule_entry_handle = NULL;

	/* queue for install if installation needs the network */
	if (!app_has_local_source (app) &&
	    !gs_plugin_get_network_available (plugin)) {
		gs_app_set_state (app, AS_APP_STATE_QUEUED_FOR_INSTALL);
		return TRUE;
	}

	/* set the app scope */
	if (gs_app_get_scope (app) == AS_APP_SCOPE_UNKNOWN) {
		g_autoptr(GSettings) settings = g_settings_new ("org.gnome.software");

		/* get the new GsFlatpak for handling of local files */
		gs_app_set_scope (app, g_settings_get_boolean (settings, "install-bundles-system-wide") ?
					AS_APP_SCOPE_SYSTEM : AS_APP_SCOPE_USER);
		if (!priv->has_system_helper) {
			g_info ("no flatpak system helper is available, using user");
			gs_app_set_scope (app, AS_APP_SCOPE_USER);
		}
		if (priv->destdir_for_tests != NULL) {
			g_debug ("in self tests, using user");
			gs_app_set_scope (app, AS_APP_SCOPE_USER);
		}
	}

	/* not supported */
	flatpak = gs_plugin_flatpak_get_handler (plugin, app);
	if (flatpak == NULL)
		return TRUE;

	/* is a source */
	if (gs_app_get_kind (app) == AS_APP_KIND_SOURCE)
		return gs_flatpak_app_install_source (flatpak, app, cancellable, error);

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
		gs_app_set_state (app, AS_APP_STATE_INSTALLING);
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

	remove_schedule_entry (schedule_entry_handle);

	if (already_installed) {
		g_debug ("App %s is already installed", gs_app_get_unique_id (app));
		gs_app_set_state (app, AS_APP_STATE_INSTALLED);
	}

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
	gboolean is_auto_update;
	gpointer schedule_entry_handle = NULL;

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

	/* build and run transaction */
	transaction = _build_transaction (plugin, flatpak, cancellable, error);
	if (transaction == NULL) {
		gs_flatpak_error_convert (error);
		return FALSE;
	}

	for (guint i = 0; i < gs_app_list_length (list_tmp); i++) {
		GsApp *app = gs_app_list_index (list_tmp, i);
		g_autofree gchar *ref = NULL;

		ref = gs_flatpak_app_get_ref_display (app);
		if (!flatpak_transaction_add_update (transaction, ref, NULL, NULL, error)) {
			gs_flatpak_error_convert (error);
			return FALSE;
		}

		/* Add the update applist for easier lookup */
		gs_flatpak_transaction_add_app (transaction, app);
	}

	/* run transaction */
	for (guint i = 0; i < gs_app_list_length (list_tmp); i++) {
		GsApp *app = gs_app_list_index (list_tmp, i);
		gs_app_set_state (app, AS_APP_STATE_INSTALLING);

		/* If all apps' update are previously downloaded and available locally,
		 * FlatpakTransaction should run with no-pull flag. This is the case
		 * for apps' autoupdates. */
		is_update_downloaded &= gs_app_get_is_update_downloaded (app);
	}

	if (is_update_downloaded) {
		flatpak_transaction_set_no_pull (transaction, TRUE);
	} else if (!gs_plugin_has_flags (plugin, GS_PLUGIN_FLAGS_INTERACTIVE)) {
		g_autoptr(GError) error_local = NULL;

		if (!gs_metered_block_app_list_on_download_scheduler (list_tmp, &schedule_entry_handle, cancellable, &error_local)) {
			g_warning ("Failed to block on download scheduler: %s",
				   error_local->message);
			g_clear_error (&error_local);
		}
	}

	if (!gs_flatpak_transaction_run (transaction, cancellable, error)) {
		for (guint i = 0; i < gs_app_list_length (list_tmp); i++) {
			GsApp *app = gs_app_list_index (list_tmp, i);
			gs_app_set_state_recover (app);
		}
		gs_flatpak_error_convert (error);
		remove_schedule_entry (schedule_entry_handle);
		return FALSE;
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

		g_assert (GS_IS_FLATPAK (flatpak));
		g_assert (list_tmp != NULL);
		g_assert (gs_app_list_length (list_tmp) > 0);

		if (!gs_plugin_flatpak_update (plugin, flatpak, list_tmp, cancellable, error))
			return FALSE;
	}
	return TRUE;
}

gboolean
gs_plugin_app_get_copyable (GsPlugin *plugin,
			    GsApp *app,
			    GFile *copy_dest,
			    gboolean *copyable,
			    GCancellable *cancellable,
			    GError **error)
{
	GsFlatpak *flatpak;

	g_assert (copyable != NULL);

	flatpak = gs_plugin_flatpak_get_handler (plugin, app);
	if (flatpak == NULL) {
		*copyable = FALSE;
		return TRUE;
	}
	return gs_flatpak_app_get_copyable (flatpak, app, copyable, cancellable,
					    error);
}

gboolean
gs_plugin_app_copy (GsPlugin *plugin,
		    GsApp *app,
		    GFile *copy_dest,
		    GCancellable *cancellable,
		    GError **error)
{
	GsFlatpak *flatpak = gs_plugin_flatpak_get_handler (plugin, app);
	if (flatpak == NULL)
		return TRUE;
	return gs_flatpak_app_copy (flatpak, app, copy_dest, cancellable,
				    error);
}

static gchar *
get_dir_mount_point_name (GFile *dir,
			  GCancellable *cancellable,
			  GError **error)
{
	g_autoptr(GMount) mount = g_file_find_enclosing_mount (dir, cancellable, error);
	if (mount == NULL)
		return NULL;
	return g_mount_get_name (mount);
}

static GsApp *
gs_plugin_flatpak_file_to_app_repo (GsPlugin *plugin,
				    GFile *file,
				    GCancellable *cancellable,
				    GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	g_autoptr(GsApp) app = NULL;
	GFileType file_type = G_FILE_TYPE_UNKNOWN;

	file_type = g_file_query_file_type (file, G_FILE_QUERY_INFO_NONE, cancellable);

	/* check if this is actually a directory */
	if (file_type == G_FILE_TYPE_DIRECTORY) {
		for (guint i = 0; i < priv->flatpaks->len; ++i) {
			g_autoptr(GError) error_local = NULL;
			GsFlatpak *flatpak = g_ptr_array_index (priv->flatpaks, i);
			app = gs_flatpak_create_app_from_repo_dir (flatpak, file, cancellable,
								   &error_local);
			if (app != NULL)
				break;

			/* if the error is "not found" than it's just that
			 * no remotes in this Flatpak installation matched
			 * others in the USB remote, but other installations
			 * can still have them... */
			if (g_error_matches (error_local, G_IO_ERROR,
					     G_IO_ERROR_NOT_FOUND))
				continue;

			gs_flatpak_error_convert (&error_local);
			g_propagate_error (error, g_steal_pointer (&error_local));
			return NULL;
		}

		if (app == NULL) {
			g_autoptr(GError) error_local = NULL;
			g_autofree gchar *mount_name = get_dir_mount_point_name (file,
										 cancellable,
										 &error_local);
			if (mount_name == NULL) {
				mount_name = g_file_get_path (file);
				g_debug ("Failed to get mount for %s: %s", mount_name,
					 error_local->message);
				g_clear_error (&error_local);
			}
			/* TRANSLATORS: error message with the name of the USB
			 * mount point or path, to inform the user we failed
			 * to load apps from that location */
			g_set_error (error, GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_INVALID_FORMAT,
				     _("No sources of applications found in the USB drive ‘%s’"),
				     mount_name);
			return NULL;
		}

		return g_steal_pointer (&app);
	}

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
							 GS_UTILS_CACHE_FLAG_ENSURE_EMPTY,
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
	gs_app_set_scope (app, AS_APP_SCOPE_UNKNOWN);

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
	gs_app_set_scope (app, AS_APP_SCOPE_UNKNOWN);

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
				gs_app_set_state (runtime, AS_APP_STATE_AVAILABLE_LOCAL);
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
		"inode/directory",
		"x-content/ostree-repository",
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

gboolean
gs_plugin_add_search (GsPlugin *plugin,
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
