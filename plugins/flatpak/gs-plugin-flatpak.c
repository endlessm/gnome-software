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
#include <libmogwai-schedule-client/schedule-entry.h>
#include <libmogwai-schedule-client/scheduler.h>

#include "gs-appstream.h"
#include "gs-flatpak-app.h"
#include "gs-flatpak.h"
#include "gs-flatpak-transaction.h"
#include "gs-flatpak-utils.h"

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
	GsPluginData *priv = gs_plugin_get_data (plugin);

	/* clear in case we're called from resetup in the self tests */
	g_ptr_array_set_size (priv->flatpaks, 0);

	/* we use a permissions helper to elevate privs */
	if (priv->has_system_helper && priv->destdir_for_tests == NULL) {
		g_autoptr(GError) local_error = NULL;
		g_autoptr(GPtrArray) installations = NULL;

		installations = flatpak_get_system_installations (cancellable,
								  &local_error);
		if (installations == NULL) {
			gs_plugin_flatpak_report_warning (plugin, &local_error);
		} else {
			for (guint i = 0; i < installations->len; i++) {
				FlatpakInstallation *installation =
					g_ptr_array_index (installations, i);
				if (!gs_plugin_flatpak_add_installation (plugin,
									 installation,
									 cancellable,
									 &local_error)) {
					gs_plugin_flatpak_report_warning (plugin,
									  &local_error);
					g_clear_error (&local_error);
					continue;
				}
			}
		}
	}

	/* in gs-self-test */
	if (priv->destdir_for_tests != NULL) {
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
		if (!gs_plugin_flatpak_add_installation (plugin, installation,
							 cancellable, error)) {
			return FALSE;
		}
	}

	/* per-user installations always available when not in self tests */
	if (priv->destdir_for_tests == NULL) {
		g_autoptr(FlatpakInstallation) installation = NULL;
		g_autoptr(GError) local_error = NULL;

		installation = flatpak_installation_new_user (cancellable,
							      &local_error);
		if (installation != NULL)
			gs_plugin_flatpak_add_installation (plugin, installation,
							    cancellable, &local_error);

		if (local_error != NULL) {
			/* if some error happened, report it as an event, but
			 * do not return it, otherwise it will disable the whole
			 * plugin (meaning that support for Flatpak will not be
			 * possible even if a system installation is working) */
			gs_plugin_flatpak_report_warning (plugin, &local_error);
		}
	}

	if (priv->flatpaks->len == 0) {
		/* when no installation has been loaded, return the error so the
		 * plugin gets disabled */
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
	g_autoptr(GError) error_local = NULL;

	g_debug ("finding ref %s", ref);
	for (guint i = 0; i < priv->flatpaks->len; i++) {
		GsFlatpak *flatpak_tmp = g_ptr_array_index (priv->flatpaks, i);
		g_autoptr(GsApp) app = NULL;
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

static FlatpakTransaction *
_build_transaction (GsPlugin *plugin, GsFlatpak *flatpak,
		    GCancellable *cancellable, GError **error)
{
	FlatpakInstallation *installation;
	g_autoptr(FlatpakTransaction) transaction = NULL;

	/* create transaction */
	installation = gs_flatpak_get_installation (flatpak);
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

static void
download_now_cb (GObject    *obj,
                 GParamSpec *pspec,
                 gpointer    user_data)
{
	gboolean *out_download_now = user_data;
	*out_download_now = mwsc_schedule_entry_get_download_now (MWSC_SCHEDULE_ENTRY (obj));
}

static void
invalidated_cb (MwscScheduleEntry *entry,
                const GError      *error,
                gpointer           user_data)
{
	GError **out_error = user_data;
	*out_error = g_error_copy (error);
}

static void
async_result_cb (GObject      *obj,
                 GAsyncResult *result,
                 gpointer      user_data)
{
	GAsyncResult **result_out = user_data;
	*result_out = g_object_ref (result);
}

gboolean
gs_plugin_download (GsPlugin *plugin, GsAppList *list,
		    GCancellable *cancellable, GError **error)
{
	GsFlatpak *flatpak = NULL;
	g_autoptr(FlatpakTransaction) transaction = NULL;
	g_autoptr(GsAppList) list_tmp = gs_app_list_new ();
	MwscScheduler *scheduler;

	/* not supported */
	for (guint i = 0; i < gs_app_list_length (list); i++) {
		GsApp *app = gs_app_list_index (list, i);
		flatpak = gs_plugin_flatpak_get_handler (plugin, app);
		if (flatpak != NULL)
			gs_app_list_add (list_tmp, app);
	}
	if (flatpak == NULL)
		return TRUE;

	/* Wait until the download can be scheduled.
	 * FIXME: In future, downloads could be split up by app, so they can all
	 * be scheduled separately and, for example, higher priority ones could
	 * be scheduled with a higher priority. This would have to be aware of
	 * dependencies. */
	scheduler = gs_plugin_get_download_scheduler (plugin);

	if (!gs_plugin_has_flags (plugin, GS_PLUGIN_FLAGS_INTERACTIVE) &&
	    scheduler != NULL) {
		g_auto(GVariantDict) parameters_dict = G_VARIANT_DICT_INIT (NULL);
		g_autoptr(GVariant) parameters = NULL;
		g_autoptr(MwscScheduleEntry) schedule_entry = NULL;
		g_autoptr(GMainContext) context = NULL;
		g_autoptr(GAsyncResult) construct_result = NULL;
		g_autoptr(GAsyncResult) schedule_result = NULL;

		context = g_main_context_new ();
		g_main_context_push_thread_default (context);

		/* Create a schedule entry for the group of downloads.
		 * FIXME: The underlying OSTree code supports resuming downloads
		 * (at a granularity of individual objects), so it should be
		 * possible to plumb through here. */
		g_variant_dict_insert (&parameters_dict, "resumable", "b", FALSE);
		parameters = g_variant_ref_sink (g_variant_dict_end (&parameters_dict));

		mwsc_scheduler_schedule_async (scheduler, parameters, cancellable,
					       async_result_cb, &schedule_result);
		while (schedule_result == NULL)
			g_main_context_iteration (context, TRUE);
		schedule_entry = mwsc_scheduler_schedule_finish (scheduler, schedule_result, error);
		if (schedule_entry == NULL) {
			/* FIXME: Add an auto MainContextThreadHolder in GLib master */
			g_main_context_pop_thread_default (context);
			return FALSE;
		}

		/* Wait until the download is allowed to proceed. */
		if (!mwsc_schedule_entry_get_download_now (schedule_entry)) {
			gboolean download_now = FALSE;
			g_autoptr(GError) invalidated_error = NULL;
			gulong notify_id, invalidated_id;

			notify_id = g_signal_connect (schedule_entry, "notify::download-now",
						      (GCallback) download_now_cb, &download_now);
			invalidated_id = g_signal_connect (schedule_entry, "invalidated",
							   (GCallback) invalidated_cb, &invalidated_error);

			while (!download_now && invalidated_error == NULL &&
			       !g_cancellable_is_cancelled (cancellable))
				g_main_context_iteration (context, TRUE);

			g_signal_handler_disconnect (schedule_entry, invalidated_id);
			g_signal_handler_disconnect (schedule_entry, notify_id);

			if (!download_now && invalidated_error != NULL) {
				g_propagate_error (error, g_steal_pointer (&invalidated_error));
				g_main_context_pop_thread_default (context);
				return FALSE;
			} else if (!download_now && g_cancellable_set_error_if_cancelled (cancellable, error)) {
				g_main_context_pop_thread_default (context);
				return FALSE;
			}

			g_assert (download_now);
		}

		g_main_context_pop_thread_default (context);
	}

	/* build and run non-deployed transaction */
	transaction = _build_transaction (plugin, flatpak, cancellable, error);
	if (transaction == NULL) {
		gs_flatpak_error_convert (error);
		return FALSE;
	}
	flatpak_transaction_set_no_deploy (transaction, TRUE);
	for (guint i = 0; i < gs_app_list_length (list_tmp); i++) {
		GsApp *app = gs_app_list_index (list_tmp, i);
		g_autofree gchar *ref = NULL;

		ref = gs_flatpak_app_get_ref_display (app);
		if (!flatpak_transaction_add_update (transaction, ref, NULL, NULL, error)) {
			gs_flatpak_error_convert (error);
			return FALSE;
		}
	}
	if (!gs_flatpak_transaction_run (transaction, cancellable, error)) {
		gs_flatpak_error_convert (error);
		return FALSE;
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
	return gs_app_has_category (app, "USB") ||
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
						      ref, NULL, error)) {
			gs_flatpak_error_convert (error);
			return FALSE;
		}
	}

	/* run transaction */
	gs_app_set_state (app, AS_APP_STATE_INSTALLING);
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
		g_prefix_error (error, "failed to run refine for %s: ",
				gs_app_get_unique_id (app));
		gs_flatpak_error_convert (error);
		return FALSE;
	}
	return TRUE;
}

gboolean
gs_plugin_update (GsPlugin *plugin,
                  GsAppList *list,
                  GCancellable *cancellable,
                  GError **error)
{
	GsFlatpak *flatpak = NULL;
	g_autoptr(FlatpakTransaction) transaction = NULL;
	g_autoptr(GsAppList) list_tmp = gs_app_list_new ();

	/* not supported */
	for (guint i = 0; i < gs_app_list_length (list); i++) {
		GsApp *app = gs_app_list_index (list, i);
		flatpak = gs_plugin_flatpak_get_handler (plugin, app);
		if (flatpak != NULL)
			gs_app_list_add (list_tmp, app);
	}
	if (flatpak == NULL)
		return TRUE;

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
	}

	/* run transaction */
	for (guint i = 0; i < gs_app_list_length (list_tmp); i++) {
		GsApp *app = gs_app_list_index (list_tmp, i);
		gs_app_set_state (app, AS_APP_STATE_INSTALLING);
	}
	if (!gs_flatpak_transaction_run (transaction, cancellable, error)) {
		for (guint i = 0; i < gs_app_list_length (list_tmp); i++) {
			GsApp *app = gs_app_list_index (list_tmp, i);
			gs_app_set_state_recover (app);
		}
		gs_flatpak_error_convert (error);
		return FALSE;
	}
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
