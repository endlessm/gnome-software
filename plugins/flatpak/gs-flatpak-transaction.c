/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2018 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2018 Kalev Lember <klember@redhat.com>
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#include <config.h>

#include "gs-flatpak-app.h"
#include "gs-flatpak-transaction.h"

struct _GsFlatpakTransaction {
	FlatpakTransaction	 parent_instance;
	GHashTable		*refhash;	/* ref:GsApp */
	GError			*first_operation_error;
#if !FLATPAK_CHECK_VERSION(1,5,1)
	gboolean		 no_deploy;
#endif
};


#if !FLATPAK_CHECK_VERSION(1,5,1)
typedef enum {
  PROP_NO_DEPLOY = 1,
} GsFlatpakTransactionProperty;
#endif

enum {
	SIGNAL_REF_TO_APP,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

G_DEFINE_TYPE (GsFlatpakTransaction, gs_flatpak_transaction, FLATPAK_TYPE_TRANSACTION)

static void
gs_flatpak_transaction_finalize (GObject *object)
{
	GsFlatpakTransaction *self;
	g_return_if_fail (GS_IS_FLATPAK_TRANSACTION (object));
	self = GS_FLATPAK_TRANSACTION (object);

	g_assert (self != NULL);
	g_hash_table_unref (self->refhash);
	if (self->first_operation_error != NULL)
		g_error_free (self->first_operation_error);

	G_OBJECT_CLASS (gs_flatpak_transaction_parent_class)->finalize (object);
}


#if !FLATPAK_CHECK_VERSION(1,5,1)
void
gs_flatpak_transaction_set_no_deploy (FlatpakTransaction *transaction, gboolean no_deploy)
{
	GsFlatpakTransaction *self;

	g_return_if_fail (GS_IS_FLATPAK_TRANSACTION (transaction));

	self = GS_FLATPAK_TRANSACTION (transaction);
	if (self->no_deploy == no_deploy)
		return;
	self->no_deploy = no_deploy;
	flatpak_transaction_set_no_deploy (transaction, no_deploy);

	g_object_notify (G_OBJECT (self), "no-deploy");
}
#endif

/* Sets installed app(s) back to installed state. Flatpak can return apps as updatable
 * (for installing a missing runtime); if it is detected that the runtime was missing
 * at the first place.
 *
 * We can determine whether a GsApp is only being updated due to a missing runtime, by checking
 * if the current operation's ref is the GsApp's runtime and also the GsApp is already deployed.
 */
static void
set_installed_app_state_if_missing_runtime_is_installed (FlatpakTransaction          *transaction,
							  FlatpakTransactionOperation *operation)
{
	GsFlatpakTransaction *self = GS_FLATPAK_TRANSACTION (transaction);
	FlatpakInstallation *installation = flatpak_transaction_get_installation (transaction);
	const gchar *op_ref = flatpak_transaction_operation_get_ref (operation);
	g_autoptr(GList) apps = NULL;

	if (g_str_has_prefix (op_ref, "app/"))
		return;

	apps = g_hash_table_get_values (self->refhash);
	for (GList *l = apps; l != NULL; l = l->next) {
		GsApp *app = l->data;
		GsApp *app_runtime;
		g_autofree gchar *app_runtime_ref = NULL;

		app_runtime = gs_app_get_runtime (app);
		if (app_runtime == NULL)
			continue;

		app_runtime_ref = gs_flatpak_app_get_ref_display (app_runtime);
		if (app_runtime_ref != NULL &&
		    g_strcmp0 (app_runtime_ref, op_ref) == 0) {
			g_autoptr(FlatpakInstalledRef) app_ref = NULL;
			g_autoptr(GBytes) metadata = NULL;

			app_ref = flatpak_installation_get_installed_ref (installation,
									  FLATPAK_REF_KIND_APP,
									  gs_flatpak_app_get_ref_name (app),
									  gs_flatpak_app_get_ref_arch (app),
									  gs_flatpak_app_get_ref_branch (app),
									  NULL, NULL);
			if (app_ref == NULL)
				continue;

			metadata = flatpak_installed_ref_load_metadata (app_ref, NULL, NULL);
			/* This makes sure that the app is already deployed. */
			if (metadata != NULL)
				gs_app_set_state (app, AS_APP_STATE_INSTALLED);
		}
	}
}

/* Checks if a ref is a related ref to one of the installed ref.
 * If yes, return the GsApp corresponding to the installed ref,
 * NULL otherwise.
 */
static GsApp *
get_installed_main_app_of_related_ref (FlatpakTransaction          *transaction,
                                       FlatpakTransactionOperation *operation)
{
	FlatpakInstallation *installation = flatpak_transaction_get_installation (transaction);
	const gchar *remote = flatpak_transaction_operation_get_remote (operation);
	const gchar *op_ref = flatpak_transaction_operation_get_ref (operation);
	GsFlatpakTransaction *self = GS_FLATPAK_TRANSACTION (transaction);
	g_autoptr(GList) keys = NULL;

	if (g_str_has_prefix (op_ref, "app/"))
		return NULL;

	keys = g_hash_table_get_keys (self->refhash);
	for (GList *l = keys; l != NULL; l = l->next) {
		g_autoptr(GPtrArray) related_refs = NULL;
		related_refs = flatpak_installation_list_installed_related_refs_sync (installation, remote,
										      l->data, NULL, NULL);
		if (related_refs == NULL)
			continue;

		for (guint i = 0; i < related_refs->len; i++) {
			g_autofree gchar *rref = flatpak_ref_format_ref (g_ptr_array_index (related_refs, i));
			if (g_strcmp0 (rref, op_ref) == 0) {
				return g_hash_table_lookup (self->refhash, l->data);
			}
		}
	}
	return NULL;
}

GsApp *
gs_flatpak_transaction_get_app_by_ref (FlatpakTransaction *transaction, const gchar *ref)
{
	GsFlatpakTransaction *self = GS_FLATPAK_TRANSACTION (transaction);
	return g_hash_table_lookup (self->refhash, ref);
}

static void
gs_flatpak_transaction_add_app_internal (GsFlatpakTransaction *self, GsApp *app)
{
	g_autofree gchar *ref = gs_flatpak_app_get_ref_display (app);
	g_hash_table_insert (self->refhash, g_steal_pointer (&ref), g_object_ref (app));
}

void
gs_flatpak_transaction_add_app (FlatpakTransaction *transaction, GsApp *app)
{
	GsFlatpakTransaction *self = GS_FLATPAK_TRANSACTION (transaction);
	gs_flatpak_transaction_add_app_internal (self, app);
	if (gs_app_get_runtime (app) != NULL)
		gs_flatpak_transaction_add_app_internal (self, gs_app_get_runtime (app));
}

static GsApp *
_ref_to_app (GsFlatpakTransaction *self, const gchar *ref)
{
	GsApp *app = g_hash_table_lookup (self->refhash, ref);
	if (app != NULL)
		return g_object_ref (app);
	g_signal_emit (self, signals[SIGNAL_REF_TO_APP], 0, ref, &app);
	return app;
}

static void
_transaction_operation_set_app (FlatpakTransactionOperation *op, GsApp *app)
{
	g_object_set_data_full (G_OBJECT (op), "GsApp",
				g_object_ref (app), (GDestroyNotify) g_object_unref);
}

static GsApp *
_transaction_operation_get_app (FlatpakTransactionOperation *op)
{
	return g_object_get_data (G_OBJECT (op), "GsApp");
}

gboolean
gs_flatpak_transaction_run (FlatpakTransaction *transaction,
                            GCancellable *cancellable,
                            GError **error)

{
	GsFlatpakTransaction *self = GS_FLATPAK_TRANSACTION (transaction);
	g_autoptr(GError) error_local = NULL;

	if (!flatpak_transaction_run (transaction, cancellable, &error_local)) {
		/* whole transaction failed; restore the state for all the apps involved */
		g_autolist(GObject) ops = flatpak_transaction_get_operations (transaction);
		for (GList *l = ops; l != NULL; l = l->next) {
			FlatpakTransactionOperation *op = l->data;
			const gchar *ref = flatpak_transaction_operation_get_ref (op);
			g_autoptr(GsApp) app = _ref_to_app (self, ref);
			if (app == NULL) {
				g_warning ("failed to find app for %s", ref);
				continue;
			}
			gs_app_set_state_recover (app);
		}

		if (self->first_operation_error != NULL) {
			g_propagate_error (error, g_steal_pointer (&self->first_operation_error));
			return FALSE;
		} else {
			g_propagate_error (error, g_steal_pointer (&error_local));
			return FALSE;
		}
	}

	return TRUE;
}

static gboolean
_transaction_ready (FlatpakTransaction *transaction)
{
	GsFlatpakTransaction *self = GS_FLATPAK_TRANSACTION (transaction);
	g_autolist(GObject) ops = NULL;

	/* nothing to do */
	ops = flatpak_transaction_get_operations (transaction);
	if (ops == NULL)
		return TRUE; // FIXME: error?
	for (GList *l = ops; l != NULL; l = l->next) {
		FlatpakTransactionOperation *op = l->data;
		const gchar *ref = flatpak_transaction_operation_get_ref (op);
		g_autoptr(GsApp) app = _ref_to_app (self, ref);
		if (app != NULL) {
			_transaction_operation_set_app (op, app);
			/* if we're updating a component, then mark all the apps
			 * involved to ensure updating the button state */
			if (flatpak_transaction_operation_get_operation_type (op) ==
					FLATPAK_TRANSACTION_OPERATION_UPDATE)
				gs_app_set_state (app, AS_APP_STATE_INSTALLING);
		}
	}
	return TRUE;
}

typedef struct
{
	GsFlatpakTransaction *transaction;  /* (owned) */
	FlatpakTransactionOperation *operation;  /* (owned) */
	GsApp *app;  /* (owned) */
} ProgressData;

static void
progress_data_free (ProgressData *data)
{
	g_clear_object (&data->operation);
	g_clear_object (&data->app);
	g_clear_object (&data->transaction);
	g_free (data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ProgressData, progress_data_free)

#if FLATPAK_CHECK_VERSION(1, 7, 3)
static gboolean
op_is_related_to_op (FlatpakTransactionOperation *op,
                     FlatpakTransactionOperation *root_op)
{
	GPtrArray *related_to_ops;  /* (element-type FlatpakTransactionOperation) */

	if (op == root_op)
		return TRUE;

	related_to_ops = flatpak_transaction_operation_get_related_to_ops (op);
	for (gsize i = 0; related_to_ops != NULL && i < related_to_ops->len; i++) {
		FlatpakTransactionOperation *related_to_op = g_ptr_array_index (related_to_ops, i);
		if (related_to_op == root_op || op_is_related_to_op (related_to_op, root_op))
			return TRUE;
	}

	return FALSE;
}

static guint64
saturated_uint64_add (guint64 a, guint64 b)
{
	return (a <= G_MAXUINT64 - b) ? a + b : G_MAXUINT64;
}

/*
 * update_progress_for_op:
 * @self: a #GsFlatpakTransaction
 * @current_progress: progress reporting object
 * @ops: results of calling flatpak_transaction_get_operations() on @self, for performance
 * @current_op: the #FlatpakTransactionOperation which the @current_progress is
 *    for; this is the operation currently being run by libflatpak
 * @root_op: the #FlatpakTransactionOperation at the root of the operation subtree
 *    to calculate progress for
 *
 * Calculate and update the #GsApp:progress for each app associated with
 * @root_op in a flatpak transaction. This will include the #GsApp for the app
 * being installed (for example), but also the #GsApps for all of its runtimes
 * and locales, and any other dependencies of them.
 *
 * Each #GsApp:progress is calculated based on the sum of the progress of all
 * the apps related to that one — so the progress for an app will factor in the
 * progress for all its runtimes.
 */
static void
update_progress_for_op (GsFlatpakTransaction        *self,
                        FlatpakTransactionProgress  *current_progress,
                        GList                       *ops,
                        FlatpakTransactionOperation *current_op,
                        FlatpakTransactionOperation *root_op)
{
	GsApp *root_app = _transaction_operation_get_app (root_op);
	guint64 related_prior_download_bytes = 0;
	guint64 related_download_bytes = 0;
	guint64 current_bytes_transferred = flatpak_transaction_progress_get_bytes_transferred (current_progress);
	gboolean seen_current_op = FALSE, seen_root_op = FALSE;
	guint percent;

	/* This relies on ops in a #FlatpakTransaction being run in the order
	 * they’re returned by flatpak_transaction_get_operations(), which is true. */
	for (GList *l = ops; l != NULL; l = l->next) {
		FlatpakTransactionOperation *op = FLATPAK_TRANSACTION_OPERATION (l->data);
		guint64 op_download_size = flatpak_transaction_operation_get_download_size (op);

		if (op == current_op)
			seen_current_op = TRUE;
		if (op == root_op)
			seen_root_op = TRUE;

		if (op_is_related_to_op (op, root_op)) {
			/* Saturate instead of overflowing */
			related_download_bytes = saturated_uint64_add (related_download_bytes, op_download_size);
			if (!seen_current_op)
				related_prior_download_bytes = saturated_uint64_add (related_prior_download_bytes, op_download_size);
		}
	}

	g_assert (related_prior_download_bytes <= related_download_bytes);
	g_assert (seen_root_op);

	/* Avoid overflows when converting to percent, at the cost of losing
	 * some precision in the least significant digits. */
	if (related_prior_download_bytes > G_MAXUINT64 / 100 ||
	    current_bytes_transferred > G_MAXUINT64 / 100) {
		related_prior_download_bytes /= 100;
		    current_bytes_transferred /= 100;
		    related_download_bytes /= 100;
	}

	/* Update the progress of @root_app. */
	if (related_download_bytes > 0)
		percent = ((related_prior_download_bytes * 100 / related_download_bytes) +
		           (current_bytes_transferred * 100 / related_download_bytes));
	else
		percent = 0;

	if (gs_app_get_progress (root_app) == 100 ||
	    gs_app_get_progress (root_app) <= percent) {
		gs_app_set_progress (root_app, percent);
	} else {
		g_warning ("ignoring percentage %u%% -> %u%% as going down on app %s",
			   gs_app_get_progress (root_app), percent,
			   gs_app_get_unique_id (root_app));
	}
}
#endif  /* flatpak 1.7.3 */

#if FLATPAK_CHECK_VERSION(1, 7, 3)
static void
update_progress_for_op_recurse_up (GsFlatpakTransaction        *self,
				   FlatpakTransactionProgress  *progress,
				   GList                       *ops,
				   FlatpakTransactionOperation *root_op,
				   FlatpakTransactionOperation *op)
{
	GPtrArray *related_to_ops = flatpak_transaction_operation_get_related_to_ops (op);

	if (!flatpak_transaction_operation_get_is_skipped (op))
		update_progress_for_op (self, progress, ops, root_op, op);

	for (gsize i = 0; related_to_ops != NULL && i < related_to_ops->len; i++) {
		FlatpakTransactionOperation *related_to_op = g_ptr_array_index (related_to_ops, i);
		update_progress_for_op_recurse_up (self, progress, ops, root_op, related_to_op);
	}
}
#endif  /* flatpak 1.7.3 */

static void
_transaction_progress_changed_cb (FlatpakTransactionProgress *progress,
				  gpointer user_data)
{
	ProgressData *data = user_data;
	GsApp *app = data->app;
#if FLATPAK_CHECK_VERSION(1, 7, 3)
	GsFlatpakTransaction *self = data->transaction;
	g_autolist(FlatpakTransactionOperation) ops = NULL;
#else
	guint percent;
#endif

	if (flatpak_transaction_progress_get_is_estimating (progress)) {
		/* "Estimating" happens while fetching the metadata, which
		 * flatpak arbitrarily decides happens during the first 5% of
		 * each operation. Often there are two install operations,
		 * for the flatpak and its locale data.
		 * However, "estimating" may also mean bogus values. We have to
		 * arbitrarily decide whether to show this value to the user. */
		if (percent > 10) {
			g_debug ("Ignoring estimated progress of %u%%", percent);
			return;
		}
	}

#if FLATPAK_CHECK_VERSION(1, 7, 3)
	/* Update the progress on this app, and then do the same for each
	 * related parent app up the hierarchy. For example, @data->operation
	 * could be for a runtime which was added to the transaction because of
	 * an app — so we need to update the progress on the app too.
	 *
	 * It’s important to note that a new @data->progress is created by
	 * libflatpak for each @data->operation, and there are multiple
	 * operations in a transaction. There is no #FlatpakTransactionProgress
	 * which represents the progress of the whole transaction.
	 *
	 * There may be arbitrary many levels of related-to ops. For example,
	 * one common situation would be to install an app which needs a new
	 * runtime, and that runtime needs a locale to be installed, which would
	 * give three levels of related-to relation:
	 *    locale → runtime → app → (null)
	 *
	 * In addition, libflatpak may decide to skip some operations (if they
	 * turn out to not be necessary). These skipped operations are not
	 * included in the list returned by flatpak_transaction_get_operations(),
	 * but they can be accessed via
	 * flatpak_transaction_operation_get_related_to_ops(), so have to be
	 * ignored manually.
	 */
	ops = flatpak_transaction_get_operations (FLATPAK_TRANSACTION (self));
	update_progress_for_op_recurse_up (self, progress, ops, data->operation, data->operation);
#else  /* if !flatpak 1.7.3 */
	percent = flatpak_transaction_progress_get_progress (progress);

	if (gs_app_get_progress (app) != 100 &&
	    gs_app_get_progress (app) > percent) {
		g_warning ("ignoring percentage %u%% -> %u%% as going down...",
			   gs_app_get_progress (app), percent);
		return;
	}
	gs_app_set_progress (app, percent);
#endif  /* !flatpak 1.7.3 */
}

static const gchar *
_flatpak_transaction_operation_type_to_string (FlatpakTransactionOperationType ot)
{
	if (ot == FLATPAK_TRANSACTION_OPERATION_INSTALL)
		return "install";
	if (ot == FLATPAK_TRANSACTION_OPERATION_UPDATE)
		return "update";
	if (ot == FLATPAK_TRANSACTION_OPERATION_INSTALL_BUNDLE)
		return "install-bundle";
	if (ot == FLATPAK_TRANSACTION_OPERATION_UNINSTALL)
		return "uninstall";
	return NULL;
}

static void
progress_data_free_closure (gpointer  user_data,
                            GClosure *closure)
{
	progress_data_free (user_data);
}

static void
_transaction_new_operation (FlatpakTransaction *transaction,
			    FlatpakTransactionOperation *operation,
			    FlatpakTransactionProgress *progress)
{
	GsApp *app;
	g_autoptr(ProgressData) progress_data = NULL;

	/* find app */
	app = _transaction_operation_get_app (operation);
	if (app == NULL) {
		FlatpakTransactionOperationType ot;
		ot = flatpak_transaction_operation_get_operation_type (operation);
		g_warning ("failed to find app for %s during %s",
			   flatpak_transaction_operation_get_ref (operation),
			   _flatpak_transaction_operation_type_to_string (ot));
		return;
	}

	/* report progress */
	progress_data = g_new0 (ProgressData, 1);
	progress_data->transaction = GS_FLATPAK_TRANSACTION (g_object_ref (transaction));
	progress_data->app = g_object_ref (app);
	progress_data->operation = g_object_ref (operation);

	g_signal_connect_data (progress, "changed",
			       G_CALLBACK (_transaction_progress_changed_cb),
			       g_steal_pointer (&progress_data),
			       progress_data_free_closure,
			       0  /* flags */);
	flatpak_transaction_progress_set_update_frequency (progress, 500); /* FIXME? */

	/* set app status */
	switch (flatpak_transaction_operation_get_operation_type (operation)) {
	case FLATPAK_TRANSACTION_OPERATION_INSTALL:
		if (gs_app_get_state (app) == AS_APP_STATE_UNKNOWN)
			gs_app_set_state (app, AS_APP_STATE_AVAILABLE);
		gs_app_set_state (app, AS_APP_STATE_INSTALLING);
		break;
	case FLATPAK_TRANSACTION_OPERATION_INSTALL_BUNDLE:
		if (gs_app_get_state (app) == AS_APP_STATE_UNKNOWN)
			gs_app_set_state (app, AS_APP_STATE_AVAILABLE_LOCAL);
		gs_app_set_state (app, AS_APP_STATE_INSTALLING);
		break;
	case FLATPAK_TRANSACTION_OPERATION_UPDATE:
		if (gs_app_get_state (app) == AS_APP_STATE_UNKNOWN)
			gs_app_set_state (app, AS_APP_STATE_UPDATABLE_LIVE);
		gs_app_set_state (app, AS_APP_STATE_INSTALLING);
		break;
	case FLATPAK_TRANSACTION_OPERATION_UNINSTALL:
		gs_app_set_state (app, AS_APP_STATE_REMOVING);
		break;
	default:
		break;
	}
}

static void
_transaction_operation_done (FlatpakTransaction *transaction,
			     FlatpakTransactionOperation *operation,
			     const gchar *commit,
			     FlatpakTransactionResult details)
{
#if !FLATPAK_CHECK_VERSION(1,5,1)
	GsFlatpakTransaction *self = GS_FLATPAK_TRANSACTION (transaction);
#endif
	GsApp *main_app = NULL;

	/* invalidate */
	GsApp *app = _transaction_operation_get_app (operation);
	if (app == NULL) {
		g_warning ("failed to find app for %s",
			   flatpak_transaction_operation_get_ref (operation));
		return;
	}
	switch (flatpak_transaction_operation_get_operation_type (operation)) {
	case FLATPAK_TRANSACTION_OPERATION_INSTALL:
		/* Handle special snowflake where "should-download" related refs for an installed ref
		 * goes missing. In that case, libflatpak marks the main app ref as updatable
		 * and then FlatpakTransaction resolves one of its ops to install the related ref(s).
		 *
		 * We can depend on libflatpak till here. Since, libflatpak returns the main app
		 * ref as updatable (instead of the related ref), we need to sync the main app's
		 * state for UI/UX.
		 *
		 * Map the current op's ref (which is related ref) to its main app ref (which is
		 * currently shown in the UI) and set the state of the main GsApp object back
		 * to INSTALLED here.
		 *
		 * This detection whether a related ref belongs to a main ref is quite sub-optimal as
		 * of now.
		 */
		main_app = get_installed_main_app_of_related_ref (transaction, operation);
		if (main_app != NULL)
			gs_app_set_state (main_app, AS_APP_STATE_INSTALLED);

		/* Do the same as above but if the main app is missing its runtime.
		 * Multiple GsApp can depend on one (missing) runtime, hence set state to "installed"
		 * state for all those apps too.
		 */
		set_installed_app_state_if_missing_runtime_is_installed (transaction, operation);

		/* For all other trivial cases. */
		gs_app_set_state (app, AS_APP_STATE_INSTALLED);
		break;
	case FLATPAK_TRANSACTION_OPERATION_INSTALL_BUNDLE:
		gs_app_set_state (app, AS_APP_STATE_INSTALLED);
		break;
	case FLATPAK_TRANSACTION_OPERATION_UPDATE:
		gs_app_set_version (app, gs_app_get_update_version (app));
		gs_app_set_update_details (app, NULL);
		gs_app_set_update_urgency (app, AS_URGENCY_KIND_UNKNOWN);
		gs_app_set_update_version (app, NULL);
		/* force getting the new runtime */
		gs_app_remove_kudo (app, GS_APP_KUDO_SANDBOXED);
                /* downloaded, but not yet installed */
#if !FLATPAK_CHECK_VERSION(1,5,1)
		if (self->no_deploy)
#else
		if (flatpak_transaction_get_no_deploy (transaction))
#endif
			gs_app_set_state (app, AS_APP_STATE_UPDATABLE_LIVE);
		else
			gs_app_set_state (app, AS_APP_STATE_INSTALLED);
		break;
	case FLATPAK_TRANSACTION_OPERATION_UNINSTALL:
		/* we don't actually know if this app is re-installable */
		gs_flatpak_app_set_commit (app, NULL);
		gs_app_set_state (app, AS_APP_STATE_UNKNOWN);
		break;
	default:
		gs_app_set_state (app, AS_APP_STATE_UNKNOWN);
		break;
	}
}

static gboolean
_transaction_operation_error (FlatpakTransaction *transaction,
			      FlatpakTransactionOperation *operation,
			      const GError *error,
			      FlatpakTransactionErrorDetails detail)
{
	GsFlatpakTransaction *self = GS_FLATPAK_TRANSACTION (transaction);
	FlatpakTransactionOperationType operation_type = flatpak_transaction_operation_get_operation_type (operation);
	GsApp *app = _transaction_operation_get_app (operation);
	const gchar *ref = flatpak_transaction_operation_get_ref (operation);

	if (g_error_matches (error, FLATPAK_ERROR, FLATPAK_ERROR_SKIPPED)) {
		g_debug ("skipped to %s %s: %s",
		         _flatpak_transaction_operation_type_to_string (operation_type),
		         ref,
		         error->message);
		return TRUE; /* continue */
	}

	if (detail & FLATPAK_TRANSACTION_ERROR_DETAILS_NON_FATAL) {
		g_warning ("failed to %s %s (non fatal): %s",
		           _flatpak_transaction_operation_type_to_string (operation_type),
		           ref,
		           error->message);
		return TRUE; /* continue */
	}

	if (self->first_operation_error == NULL) {
		g_propagate_error (&self->first_operation_error,
		                   g_error_copy (error));
		if (app != NULL)
			gs_utils_error_add_app_id (&self->first_operation_error, app);
	}
	return FALSE; /* stop */
}

static int
_transaction_choose_remote_for_ref (FlatpakTransaction *transaction,
				    const char *for_ref,
				    const char *runtime_ref,
				    const char * const *remotes)
{
	//FIXME: do something smarter
	return 0;
}

static void
_transaction_end_of_lifed (FlatpakTransaction *transaction,
			   const gchar *ref,
			   const gchar *reason,
			   const gchar *rebase)
{
	if (rebase) {
		g_printerr ("%s is end-of-life, in preference of %s\n", ref, rebase);
	} else if (reason) {
		g_printerr ("%s is end-of-life, with reason: %s\n", ref, reason);
	}
	//FIXME: show something in the UI
}

static gboolean
_transaction_add_new_remote (FlatpakTransaction *transaction,
			     FlatpakTransactionRemoteReason reason,
			     const char *from_id,
			     const char *remote_name,
			     const char *url)
{
	/* additional applications */
	if (reason == FLATPAK_TRANSACTION_REMOTE_GENERIC_REPO) {
		g_debug ("configuring %s as new generic remote", url);
		return TRUE; //FIXME?
	}

	/* runtime deps always make sense */
	if (reason == FLATPAK_TRANSACTION_REMOTE_RUNTIME_DEPS) {
		g_debug ("configuring %s as new remote for deps", url);
		return TRUE;
	}

	return FALSE;
}

#if !FLATPAK_CHECK_VERSION(1,5,1)
static void
gs_flatpak_transaction_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
	FlatpakTransaction *transaction = FLATPAK_TRANSACTION (object);

	switch ((GsFlatpakTransactionProperty) prop_id) {
	case PROP_NO_DEPLOY:
		gs_flatpak_transaction_set_no_deploy (transaction, g_value_get_boolean (value));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}
#endif

static void
gs_flatpak_transaction_class_init (GsFlatpakTransactionClass *klass)
{

#if !FLATPAK_CHECK_VERSION(1,5,1)
	GParamSpec *pspec;
#endif
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	FlatpakTransactionClass *transaction_class = FLATPAK_TRANSACTION_CLASS (klass);
	object_class->finalize = gs_flatpak_transaction_finalize;
	transaction_class->ready = _transaction_ready;
	transaction_class->add_new_remote = _transaction_add_new_remote;
	transaction_class->new_operation = _transaction_new_operation;
	transaction_class->operation_done = _transaction_operation_done;
	transaction_class->operation_error = _transaction_operation_error;
	transaction_class->choose_remote_for_ref = _transaction_choose_remote_for_ref;
	transaction_class->end_of_lifed = _transaction_end_of_lifed;
#if !FLATPAK_CHECK_VERSION(1,5,1)
	object_class->set_property = gs_flatpak_transaction_set_property;

	pspec = g_param_spec_boolean ("no-deploy", NULL,
				      "Whether the current transaction will deploy the downloaded objects",
				      FALSE, G_PARAM_WRITABLE | G_PARAM_CONSTRUCT);
	g_object_class_install_property (object_class, PROP_NO_DEPLOY, pspec);
#endif

	signals[SIGNAL_REF_TO_APP] =
		g_signal_new ("ref-to-app",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      0, NULL, NULL, NULL, G_TYPE_OBJECT, 1, G_TYPE_STRING);
}

static void
gs_flatpak_transaction_init (GsFlatpakTransaction *self)
{
	self->refhash = g_hash_table_new_full (g_str_hash, g_str_equal,
					       g_free, (GDestroyNotify) g_object_unref);
}

FlatpakTransaction *
gs_flatpak_transaction_new (FlatpakInstallation	*installation,
			    GCancellable *cancellable,
			    GError **error)
{
	GsFlatpakTransaction *self;
	self = g_initable_new (GS_TYPE_FLATPAK_TRANSACTION,
			       cancellable, error,
			       "installation", installation,
			       NULL);
	if (self == NULL)
		return NULL;
	return FLATPAK_TRANSACTION (self);
}
