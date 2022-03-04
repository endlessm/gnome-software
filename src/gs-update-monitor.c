/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 * vi:set noexpandtab tabstop=8 shiftwidth=8:
 *
 * Copyright (C) 2013-2018 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2013 Matthias Clasen <mclasen@redhat.com>
 * Copyright (C) 2014-2018 Kalev Lember <klember@redhat.com>
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#include "config.h"

#include <string.h>
#include <glib/gi18n.h>
#include <gsettings-desktop-schemas/gdesktop-enums.h>
#include <locale.h>

#include "gs-update-monitor.h"
#include "gs-common.h"

#define SECONDS_IN_AN_HOUR (60 * 60)
#define SECONDS_IN_A_DAY (SECONDS_IN_AN_HOUR * 24)

struct _GsUpdateMonitor {
	GObject		 parent;

	GApplication	*application;
	GCancellable    *cancellable;
	GCancellable	*refresh_cancellable;
	GSettings	*settings;
	GsPluginLoader	*plugin_loader;
	GDBusProxy	*proxy_upower;
	GError		*last_offline_error;

	GNetworkMonitor *network_monitor;
	guint		 network_changed_handler;

#if GLIB_CHECK_VERSION(2, 69, 1)
	GPowerProfileMonitor	*power_profile_monitor;  /* (owned) (nullable) */
	gulong			 power_profile_changed_handler;
#endif

	guint		 cleanup_notifications_id;	/* at startup */
	guint		 check_startup_id;		/* 60s after startup */
	guint		 check_hourly_id;		/* and then every hour */
	guint		 check_daily_id;		/* every 3rd day */

	gint64		 last_notification_time_usec;	/* to notify once per day only */
};

G_DEFINE_TYPE (GsUpdateMonitor, gs_update_monitor, G_TYPE_OBJECT)

typedef struct {
	GsUpdateMonitor		*monitor;
} DownloadUpdatesData;

static void
download_updates_data_free (DownloadUpdatesData *data)
{
	g_clear_object (&data->monitor);
	g_slice_free (DownloadUpdatesData, data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(DownloadUpdatesData, download_updates_data_free);

typedef struct {
	GsUpdateMonitor		*monitor;
	GsApp			*app;
} WithAppData;

static WithAppData *
with_app_data_new (GsUpdateMonitor	*monitor,
		   GsApp		*app)
{
	WithAppData *data;
	data = g_slice_new0 (WithAppData);
	data->monitor = g_object_ref (monitor);
	data->app = g_object_ref (app);
	return data;
}

static void
with_app_data_free (WithAppData *data)
{
	g_clear_object (&data->monitor);
	g_clear_object (&data->app);
	g_slice_free (WithAppData, data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(WithAppData, with_app_data_free);

static void
check_updates_kind (GsAppList *apps,
		    gboolean *out_has_important,
		    gboolean *out_any_important_downloaded,
		    gboolean *out_all_downloaded,
		    gboolean *out_any_downloaded)
{
	gboolean has_important, any_important_downloaded, all_downloaded, any_downloaded;
	guint ii, len;
	GsApp *app;

	len = gs_app_list_length (apps);
	has_important = FALSE;
	any_important_downloaded = FALSE;
	all_downloaded = len > 0;
	any_downloaded = FALSE;

	for (ii = 0; ii < len && (!has_important || all_downloaded || !any_downloaded); ii++) {
		gboolean is_important;

		app = gs_app_list_index (apps, ii);

		is_important = gs_app_get_update_urgency (app) == AS_URGENCY_KIND_CRITICAL;
		has_important = has_important || is_important;

		/* took from gs-updates-section.c: _all_offline_updates_downloaded();
		   the app is considered downloaded, when its download size is 0 */
		if (gs_app_get_size_download (app)) {
			all_downloaded = FALSE;
		} else {
			any_downloaded = TRUE;
			if (is_important)
				any_important_downloaded = TRUE;
		}
	}

	*out_has_important = has_important;
	*out_any_important_downloaded = any_important_downloaded;
	*out_all_downloaded = all_downloaded;
	*out_any_downloaded = any_downloaded;
}

static gboolean
get_timestamp_difference_days (GsUpdateMonitor *monitor, const gchar *timestamp, gint64 *out_days)
{
	gint64 tmp;
	g_autoptr(GDateTime) last_update = NULL;
	g_autoptr(GDateTime) now = NULL;

	g_return_val_if_fail (out_days != NULL, FALSE);

	g_settings_get (monitor->settings, timestamp, "x", &tmp);
	if (tmp == 0)
		return FALSE;

	last_update = g_date_time_new_from_unix_local (tmp);
	if (last_update == NULL) {
		g_warning ("failed to set timestamp %" G_GINT64_FORMAT, tmp);
		return FALSE;
	}

	now = g_date_time_new_now_local ();

	*out_days = g_date_time_difference (now, last_update) / G_TIME_SPAN_DAY;

	return TRUE;
}

static gboolean
check_if_timestamp_more_than_days_ago (GsUpdateMonitor *monitor, const gchar *timestamp, guint days)
{
	gint64 timestamp_days;

	if (!get_timestamp_difference_days (monitor, timestamp, &timestamp_days))
		return TRUE;

	return timestamp_days >= days;
}

static gboolean
should_download_updates (GsUpdateMonitor *monitor)
{
#ifdef HAVE_MOGWAI
	return TRUE;
#else
	return g_settings_get_boolean (monitor->settings, "download-updates");
#endif
}

/* The days below are discussed at https://gitlab.gnome.org/GNOME/gnome-software/-/issues/947 */
static gboolean
should_notify_about_pending_updates (GsUpdateMonitor *monitor,
				     GsAppList *apps,
				     const gchar **out_title,
				     const gchar **out_body)
{
	gboolean has_important = FALSE, any_important_downloaded = FALSE, all_downloaded = FALSE, any_downloaded = FALSE;
	gboolean should_download, res = FALSE;
	gint64 timestamp_days;

	if (!get_timestamp_difference_days (monitor, "update-notification-timestamp", &timestamp_days)) {
		/* Large-enough number to succeed for the initial test */
		timestamp_days = 365;
	}

	should_download = should_download_updates (monitor);
	check_updates_kind (apps, &has_important, &any_important_downloaded, &all_downloaded, &any_downloaded);

	if (!gs_app_list_length (apps)) {
		/* Notify only when the download is disabled and it's the 4th day or it's more than 7 days */
		if (!should_download && (timestamp_days >= 7 || timestamp_days == 4)) {
			*out_title = _("Software Updates Are Out of Date");
			*out_body = _("Please check for software updates.");
			res = TRUE;
		}
	} else if (has_important) {
		if (timestamp_days >= 1) {
			if (any_important_downloaded) {
				*out_title = _("Critical Software Update Ready to Install");
				*out_body = _("An important software update is ready to be installed.");
				res = TRUE;
			} else {
				*out_title = _("Critical Software Updates Available to Download");
				*out_body = _("Important: critical software updates are waiting.");
				res = TRUE;
			}
		}
		/* When automatic updates are on and there are things ready to be installed, then rather claim
		 * about things to be installed, than things to be downloaded. */
	} else if (all_downloaded || (any_downloaded && should_download)) {
		if (timestamp_days >= 3) {
			*out_title = _("Software Updates Ready to Install");
			*out_body = _("Software updates are waiting and ready to be installed.");
			res = TRUE;
		}
	/* To not hide downloaded updates for 14 days when new updates were discovered meanwhile */
	} else if (timestamp_days >= (any_downloaded ? 3 : 14)) {
		*out_title = _("Software Updates Available to Download");
		*out_body = _("Please download waiting software updates.");
		res = TRUE;
	}

	g_debug ("%s: last_test_days:%" G_GINT64_FORMAT " n-apps:%u should_download:%d has_important:%d any_important_downloaded:%d "
		"all_downloaded:%d any_downloaded:%d res:%d%s%s%s%s", G_STRFUNC,
		timestamp_days, gs_app_list_length (apps), should_download, has_important, any_important_downloaded,
		all_downloaded, any_downloaded, res,
		res ? " reason:" : "",
		res ? *out_title : "",
		res ? "|" : "",
		res ? *out_body : "");

	return res;
}

static void
reset_update_notification_timestamp (GsUpdateMonitor *monitor)
{
	g_autoptr(GDateTime) now = NULL;

	now = g_date_time_new_now_local ();
	g_settings_set (monitor->settings, "update-notification-timestamp", "x",
	                g_date_time_to_unix (now));
}

static void
notify_about_pending_updates (GsUpdateMonitor *monitor,
			      GsAppList *apps)
{
	const gchar *title = NULL, *body = NULL;
	gint64 time_diff_sec;
	g_autoptr(GNotification) nn = NULL;

	time_diff_sec = (g_get_real_time () - monitor->last_notification_time_usec) / G_USEC_PER_SEC;
	if (time_diff_sec < SECONDS_IN_A_DAY) {
		g_debug ("Skipping update notification daily check, because made one only %" G_GINT64_FORMAT "s ago",
			 time_diff_sec);
		return;
	}

	if (!should_notify_about_pending_updates (monitor, apps, &title, &body)) {
		g_debug ("No update notification needed");
		return;
	}

	monitor->last_notification_time_usec = g_get_real_time ();

	g_debug ("Notify about update: '%s'", title);

	nn = g_notification_new (title);
	g_notification_set_body (nn, body);
	g_notification_set_default_action_and_target (nn, "app.set-mode", "s", "updates");
	g_application_send_notification (monitor->application, "updates-available", nn);

	/* Keep the old notification time when there are no updates and the update download is disabled,
	   to notify the user every day after 7 days of no update check */
	if (gs_app_list_length (apps) ||
	    should_download_updates (monitor))
		reset_update_notification_timestamp (monitor);
}

static gboolean
_filter_by_app_kind (GsApp *app, gpointer user_data)
{
	AsComponentKind kind = GPOINTER_TO_UINT (user_data);
	return gs_app_get_kind (app) == kind;
}

static gboolean
_sort_by_rating_cb (GsApp *app1, GsApp *app2, gpointer user_data)
{
	if (gs_app_get_rating (app1) < gs_app_get_rating (app2))
		return -1;
	if (gs_app_get_rating (app1) > gs_app_get_rating (app2))
		return 1;
	return 0;
}

static GNotification *
_build_autoupdated_notification (GsUpdateMonitor *monitor, GsAppList *list)
{
	guint need_restart_cnt = 0;
	g_autoptr(GsAppList) list_apps = NULL;
	g_autoptr(GNotification) n = NULL;
	g_autoptr(GString) body = g_string_new (NULL);
	g_autofree gchar *title = NULL;

	/* filter out apps */
	list_apps = gs_app_list_copy (list);
	gs_app_list_filter (list_apps,
			    _filter_by_app_kind,
			    GUINT_TO_POINTER(AS_COMPONENT_KIND_DESKTOP_APP));
	gs_app_list_sort (list_apps, _sort_by_rating_cb, NULL);
	/* FIXME: add the applications that are currently active that use one
	 * of the updated runtimes */
	if (gs_app_list_length (list_apps) == 0) {
		g_debug ("no desktop apps in updated list, ignoring");
		return NULL;
	}

	/* how many apps needs updating */
	for (guint i = 0; i < gs_app_list_length (list_apps); i++) {
		GsApp *app = gs_app_list_index (list_apps, i);
		if (gs_app_has_quirk (app, GS_APP_QUIRK_NEEDS_REBOOT))
			need_restart_cnt++;
	}

	/* >1 app updated */
	if (gs_app_list_length (list_apps) > 0) {
		if (need_restart_cnt > 0) {
			/* TRANSLATORS: apps were auto-updated and restart is required */
			title = g_strdup_printf (ngettext ("%u Application Updated — Restart Required",
			                                   "%u Applications Updated — Restart Required",
			                                   gs_app_list_length (list_apps)),
			                         gs_app_list_length (list_apps));
		} else {
			/* TRANSLATORS: apps were auto-updated */
			title = g_strdup_printf (ngettext ("%u Application Updated",
			                                   "%u Applications Updated",
			                                   gs_app_list_length (list_apps)),
			                         gs_app_list_length (list_apps));
		}
	}

	/* 1 app updated */
	if (gs_app_list_length (list_apps) == 1) {
		GsApp *app = gs_app_list_index (list_apps, 0);
		/* TRANSLATORS: %1 is an application name, e.g. Firefox */
		g_string_append_printf (body, _("%s has been updated."), gs_app_get_name (app));
		if (need_restart_cnt > 0) {
			/* TRANSLATORS: the app needs restarting */
			g_string_append_printf (body, " %s", _("Please restart the application."));
		}

	/* 2 apps updated */
	} else if (gs_app_list_length (list_apps) == 2) {
		GsApp *app1 = gs_app_list_index (list_apps, 0);
		GsApp *app2 = gs_app_list_index (list_apps, 1);
		/* TRANSLATORS: %1 and %2 are both application names, e.g. Firefox */
		g_string_append_printf (body, _("%s and %s have been updated."),
					gs_app_get_name (app1),
					gs_app_get_name (app2));
		if (need_restart_cnt > 0) {
			g_string_append (body, " ");
			/* TRANSLATORS: at least one application needs restarting */
			g_string_append_printf (body, ngettext ("%u application requires a restart.",
								"%u applications require a restart.",
								need_restart_cnt),
						need_restart_cnt);
		}

	/* 3+ apps */
	} else if (gs_app_list_length (list_apps) >= 3) {
		GsApp *app1 = gs_app_list_index (list_apps, 0);
		GsApp *app2 = gs_app_list_index (list_apps, 1);
		GsApp *app3 = gs_app_list_index (list_apps, 2);
		/* TRANSLATORS: %1, %2 and %3 are all application names, e.g. Firefox */
		g_string_append_printf (body, _("Includes %s, %s and %s."),
					gs_app_get_name (app1),
					gs_app_get_name (app2),
					gs_app_get_name (app3));
		if (need_restart_cnt > 0) {
			g_string_append (body, " ");
			/* TRANSLATORS: at least one application needs restarting */
			g_string_append_printf (body, ngettext ("%u application requires a restart.",
								"%u applications require a restart.",
								need_restart_cnt),
						need_restart_cnt);
		}
	}

	/* create the notification */
	n = g_notification_new (title);
	if (body->len > 0)
		g_notification_set_body (n, body->str);
	g_notification_set_default_action_and_target (n, "app.set-mode", "s", "updated");
	return g_steal_pointer (&n);
}

static void
update_finished_cb (GObject *object, GAsyncResult *res, gpointer data)
{
	GsUpdateMonitor *monitor = GS_UPDATE_MONITOR (data);
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (object);
	g_autoptr(GError) error = NULL;
	g_autoptr(GsAppList) list = NULL;

	/* get result */
	list = gs_plugin_loader_job_process_finish (plugin_loader, res, &error);
	if (list == NULL) {
		gs_plugin_loader_claim_error (plugin_loader,
					      NULL,
					      GS_PLUGIN_ACTION_UPDATE,
					      NULL,
					      TRUE,
					      error);
		return;
	}

	/* notifications are optional */
	if (g_settings_get_boolean (monitor->settings, "download-updates-notify")) {
		g_autoptr(GNotification) n = NULL;
		g_application_withdraw_notification (monitor->application,
						     "updates-installed");
		n = _build_autoupdated_notification (monitor, list);
		if (n != NULL)
			g_application_send_notification (monitor->application,
							 "updates-installed", n);
	}
}

static gboolean
_should_auto_update (GsApp *app)
{
	if (gs_app_get_state (app) != GS_APP_STATE_UPDATABLE_LIVE)
		return FALSE;
	if (gs_app_has_quirk (app, GS_APP_QUIRK_NEW_PERMISSIONS))
		return FALSE;
	if (gs_app_has_quirk (app, GS_APP_QUIRK_DO_NOT_AUTO_UPDATE))
		return FALSE;
	return TRUE;
}

static void
download_finished_cb (GObject *object, GAsyncResult *res, gpointer data)
{
	GsUpdateMonitor *monitor = GS_UPDATE_MONITOR (data);
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (object);
	g_autoptr(GError) error = NULL;
	g_autoptr(GsAppList) list = NULL;
	g_autoptr(GsAppList) update_online = NULL;
	g_autoptr(GsAppList) update_offline = NULL;

	/* get result */
	list = gs_plugin_loader_job_process_finish (plugin_loader, res, &error);
	if (list == NULL) {
		gs_plugin_loader_claim_error (plugin_loader,
					      NULL,
					      GS_PLUGIN_ACTION_DOWNLOAD,
					      NULL,
					      TRUE,
					      error);
		return;
	}

	update_online = gs_app_list_new ();
	update_offline = gs_app_list_new ();
	for (guint i = 0; i < gs_app_list_length (list); i++) {
		GsApp *app = gs_app_list_index (list, i);
		if (_should_auto_update (app)) {
			g_debug ("auto-updating %s", gs_app_get_unique_id (app));
			gs_app_list_add (update_online, app);
		} else {
			gs_app_list_add (update_offline, app);
		}
	}

	/* install any apps that can be installed LIVE */
	if (gs_app_list_length (update_online) > 0) {
		g_autoptr(GsPluginJob) plugin_job = NULL;
		plugin_job = gs_plugin_job_newv (GS_PLUGIN_ACTION_UPDATE,
						 "list", update_online,
						 "propagate-error", TRUE,
						 NULL);
		gs_plugin_loader_job_process_async (monitor->plugin_loader,
						    plugin_job,
						    monitor->cancellable,
						    update_finished_cb,
						    monitor);
	}

	/* show a notification for offline updates */
	if (gs_app_list_length (update_offline) > 0)
		notify_about_pending_updates (monitor, update_offline);
}

static void
get_updates_finished_cb (GObject *object, GAsyncResult *res, gpointer data)
{
	g_autoptr(DownloadUpdatesData) download_updates_data = (DownloadUpdatesData *) data;
	GsUpdateMonitor *monitor = download_updates_data->monitor;
	guint64 security_timestamp = 0;
	guint64 security_timestamp_old = 0;
	g_autoptr(GError) error = NULL;
	g_autoptr(GsAppList) apps = NULL;
	gboolean should_download;

	/* get result */
	apps = gs_plugin_loader_job_process_finish (GS_PLUGIN_LOADER (object), res, &error);
	if (apps == NULL) {
		if (!g_error_matches (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_CANCELLED) &&
		    !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			g_warning ("failed to get updates: %s", error->message);
		return;
	}

	/* no updates */
	if (gs_app_list_length (apps) == 0) {
		g_debug ("no updates; withdrawing updates-available notification");
		g_application_withdraw_notification (monitor->application,
						     "updates-available");
		return;
	}

	/* find security updates, or clear timestamp if there are now none */
	g_settings_get (monitor->settings,
			"security-timestamp", "x", &security_timestamp_old);
	for (guint i = 0; i < gs_app_list_length (apps); i++) {
		GsApp *app = gs_app_list_index (apps, i);
		if (gs_app_get_update_urgency (app) == AS_URGENCY_KIND_CRITICAL &&
		    gs_app_get_size_download (app) > 0 &&
		    gs_app_get_size_download (app) != GS_APP_SIZE_UNKNOWABLE) {
			security_timestamp = (guint64) g_get_monotonic_time ();
			break;
		}
	}
	if (security_timestamp_old != security_timestamp) {
		g_settings_set (monitor->settings,
				"security-timestamp", "x", security_timestamp);
	}

	g_debug ("got %u updates", gs_app_list_length (apps));

	should_download = should_download_updates (monitor);

	if (should_download &&
	    (security_timestamp_old != security_timestamp ||
	    check_if_timestamp_more_than_days_ago (monitor, "install-timestamp", 14))) {
		g_autoptr(GsPluginJob) plugin_job = NULL;

		/* download any updates; individual plugins are responsible for deciding
		 * whether it’s appropriate to unconditionally download the updates, or
		 * to schedule the download in accordance with the user’s metered data
		 * preferences */
		plugin_job = gs_plugin_job_newv (GS_PLUGIN_ACTION_DOWNLOAD,
						 "list", apps,
						 "propagate-error", TRUE,
						 NULL);
		g_debug ("Getting updates");
		gs_plugin_loader_job_process_async (monitor->plugin_loader,
						    plugin_job,
						    monitor->refresh_cancellable,
						    download_finished_cb,
						    monitor);
	} else {
		g_autoptr(GsAppList) update_online = NULL;
		g_autoptr(GsAppList) update_offline = NULL;
		GsAppList *notify_list;

		update_online = gs_app_list_new ();
		update_offline = gs_app_list_new ();
		for (guint i = 0; i < gs_app_list_length (apps); i++) {
			GsApp *app = gs_app_list_index (apps, i);
			if (_should_auto_update (app)) {
				g_debug ("download for auto-update %s", gs_app_get_unique_id (app));
				gs_app_list_add (update_online, app);
			} else {
				gs_app_list_add (update_offline, app);
			}
		}

		g_debug ("Received %u apps to update, %u are online and %u offline updates; will%s download online updates",
			gs_app_list_length (apps),
			gs_app_list_length (update_online),
			gs_app_list_length (update_offline),
			should_download ? "" : " not");

		if (should_download && gs_app_list_length (update_online) > 0) {
			g_autoptr(GsPluginJob) plugin_job = NULL;

			plugin_job = gs_plugin_job_newv (GS_PLUGIN_ACTION_DOWNLOAD,
							 "list", update_online,
							 "propagate-error", TRUE,
							 NULL);
			g_debug ("Getting %u online updates", gs_app_list_length (update_online));
			gs_plugin_loader_job_process_async (monitor->plugin_loader,
							    plugin_job,
							    monitor->refresh_cancellable,
							    download_finished_cb,
							    monitor);
		}

		if (should_download)
			notify_list = update_offline;
		else
			notify_list = apps;

		notify_about_pending_updates (monitor, notify_list);
	}
}

static gboolean
should_show_upgrade_notification (GsUpdateMonitor *monitor)
{
	return check_if_timestamp_more_than_days_ago (monitor, "upgrade-notification-timestamp", 7);
}

static void
get_system_finished_cb (GObject *object, GAsyncResult *res, gpointer data)
{
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (object);
	GsUpdateMonitor *monitor = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(GNotification) n = NULL;
	g_autoptr(GsApp) app = NULL;

	/* get result */
	app = gs_plugin_loader_get_system_app_finish (plugin_loader, res, &error);
	if (app == NULL) {
		if (!g_error_matches (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_CANCELLED) &&
		    !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			g_warning ("failed to get system: %s", error->message);
		return;
	}

	/* might be already showing, so just withdraw it and re-issue it */
	g_application_withdraw_notification (monitor->application, "eol");

	/* do not show when the main window is active */
	if (gs_application_has_active_window (GS_APPLICATION (monitor->application)))
		return;

	/* is not EOL */
	if (gs_app_get_state (app) != GS_APP_STATE_UNAVAILABLE)
		return;

	/* TRANSLATORS: this is when the current operating system version goes end-of-life */
	n = g_notification_new (_("Operating System Updates Unavailable"));
	/* TRANSLATORS: this is the message dialog for the distro EOL notice */
	g_notification_set_body (n, _("Upgrade to continue receiving security updates."));
	g_notification_set_default_action_and_target (n, "app.set-mode", "s", "updates");
	g_application_send_notification (monitor->application, "eol", n);
}

static void
get_upgrades_finished_cb (GObject *object,
			  GAsyncResult *res,
			  gpointer data)
{
	GsUpdateMonitor *monitor = GS_UPDATE_MONITOR (data);
	GsApp *app;
	g_autofree gchar *body = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GNotification) n = NULL;
	g_autoptr(GsAppList) apps = NULL;

	/* get result */
	apps = gs_plugin_loader_job_process_finish (GS_PLUGIN_LOADER (object), res, &error);
	if (apps == NULL) {
		if (!g_error_matches (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_CANCELLED) &&
		    !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			g_warning ("failed to get upgrades: %s",
				   error->message);
		}
		return;
	}

	/* no results */
	if (gs_app_list_length (apps) == 0) {
		g_debug ("no upgrades; withdrawing upgrades-available notification");
		g_application_withdraw_notification (monitor->application,
						     "upgrades-available");
		return;
	}

	/* do not show if gnome-software is already open */
	if (gs_application_has_active_window (GS_APPLICATION (monitor->application)))
		return;

	/* only nag about upgrades once per week */
	if (!should_show_upgrade_notification (monitor))
		return;

	g_debug ("showing distro upgrade notification");
	now = g_date_time_new_now_local ();
	g_settings_set (monitor->settings, "upgrade-notification-timestamp", "x",
	                g_date_time_to_unix (now));

	/* rely on the app list already being sorted with the
	 * chronologically newest release last */
	app = gs_app_list_index (apps, gs_app_list_length (apps) - 1);

	/* TRANSLATORS: this is a distro upgrade, the replacement would be the
	 * distro name, e.g. 'Fedora' */
	body = g_strdup_printf (_("A new version of %s is available to install"),
				gs_app_get_name (app));

	/* TRANSLATORS: this is a distro upgrade */
	n = g_notification_new (_("Software Upgrade Available"));
	g_notification_set_body (n, body);
	g_notification_set_default_action_and_target (n, "app.set-mode", "s", "updates");
	g_application_send_notification (monitor->application, "upgrades-available", n);
}

static void
get_updates (GsUpdateMonitor *monitor)
{
	g_autoptr(GsPluginJob) plugin_job = NULL;
	g_autoptr(DownloadUpdatesData) download_updates_data = NULL;

	/* disabled in gsettings or from a plugin */
	if (!gs_plugin_loader_get_allow_updates (monitor->plugin_loader)) {
		g_debug ("not getting updates as not enabled");
		return;
	}

	download_updates_data = g_slice_new0 (DownloadUpdatesData);
	download_updates_data->monitor = g_object_ref (monitor);

	/* NOTE: this doesn't actually do any network access */
	g_debug ("Getting updates");
	plugin_job = gs_plugin_job_newv (GS_PLUGIN_ACTION_GET_UPDATES,
					 "refine-flags", GS_PLUGIN_REFINE_FLAGS_REQUIRE_UPDATE_DETAILS |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_UPDATE_SEVERITY,
					 NULL);
	gs_plugin_loader_job_process_async (monitor->plugin_loader,
					    plugin_job,
					    monitor->cancellable,
					    get_updates_finished_cb,
					    g_steal_pointer (&download_updates_data));
}

void
gs_update_monitor_autoupdate (GsUpdateMonitor *monitor)
{
	get_updates (monitor);
}

static void
get_upgrades (GsUpdateMonitor *monitor)
{
	g_autoptr(GsPluginJob) plugin_job = NULL;

	/* disabled in gsettings or from a plugin */
	if (!gs_plugin_loader_get_allow_updates (monitor->plugin_loader)) {
		g_debug ("not getting upgrades as not enabled");
		return;
	}

	/* NOTE: this doesn't actually do any network access, it relies on the
	 * AppStream data being up to date, either by the appstream-data
	 * package being up-to-date, or the metadata being auto-downloaded */
	g_debug ("Getting upgrades");
	plugin_job = gs_plugin_job_list_distro_upgrades_new (GS_PLUGIN_LIST_DISTRO_UPGRADES_FLAGS_NONE,
							     GS_PLUGIN_REFINE_FLAGS_NONE);
	gs_plugin_loader_job_process_async (monitor->plugin_loader,
					    plugin_job,
					    monitor->cancellable,
					    get_upgrades_finished_cb,
					    monitor);
}

static void
get_system (GsUpdateMonitor *monitor)
{
	g_autoptr(GsApp) app = NULL;

	g_debug ("Getting system");
	gs_plugin_loader_get_system_app_async (monitor->plugin_loader, monitor->cancellable,
		get_system_finished_cb, monitor);
}

static void
refresh_cache_finished_cb (GObject *object,
			   GAsyncResult *res,
			   gpointer data)
{
	GsUpdateMonitor *monitor = data;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(GError) error = NULL;

	if (!gs_plugin_loader_job_action_finish (GS_PLUGIN_LOADER (object), res, &error)) {
		if (!g_error_matches (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_CANCELLED) &&
		    !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			g_warning ("failed to refresh the cache: %s", error->message);
		return;
	}

	/* update the last checked timestamp */
	now = g_date_time_new_now_local ();
	g_settings_set (monitor->settings, "check-timestamp", "x",
	                g_date_time_to_unix (now));

	get_updates (monitor);
}

typedef enum {
	UP_DEVICE_LEVEL_UNKNOWN,
	UP_DEVICE_LEVEL_NONE,
	UP_DEVICE_LEVEL_DISCHARGING,
	UP_DEVICE_LEVEL_LOW,
	UP_DEVICE_LEVEL_CRITICAL,
	UP_DEVICE_LEVEL_ACTION,
	UP_DEVICE_LEVEL_LAST
} UpDeviceLevel;

static void
install_language_pack_cb (GObject *object, GAsyncResult *res, gpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(WithAppData) with_app_data = data;

	if (!gs_plugin_loader_job_action_finish (GS_PLUGIN_LOADER (object), res, &error)) {
		if (!g_error_matches (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_CANCELLED) &&
		    !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			g_debug ("failed to install language pack: %s", error->message);
		return;
	} else {
		g_debug ("language pack for %s installed",
			 gs_app_get_name (with_app_data->app));
	}
}

static void
get_language_pack_cb (GObject *object, GAsyncResult *res, gpointer data)
{
	GsUpdateMonitor *monitor = GS_UPDATE_MONITOR (data);
	GsApp *app;
	g_autoptr(GError) error = NULL;
	g_autoptr(GsAppList) app_list = NULL;

	app_list = gs_plugin_loader_job_process_finish (GS_PLUGIN_LOADER (object), res, &error);
	if (app_list == NULL) {
		if (!g_error_matches (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_CANCELLED) &&
		    !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			g_debug ("failed to find language pack: %s", error->message);
		return;
	}

	/* none found */
	if (gs_app_list_length (app_list) == 0) {
		g_debug ("no language pack found");
		return;
	}

	/* there should be one langpack for a given locale */
	app = g_object_ref (gs_app_list_index (app_list, 0));
	if (!gs_app_is_installed (app)) {
		WithAppData *with_app_data;
		g_autoptr(GsPluginJob) plugin_job = NULL;

		with_app_data = with_app_data_new (monitor, app);

		plugin_job = gs_plugin_job_newv (GS_PLUGIN_ACTION_INSTALL,
							 "app", app,
							 NULL);
		gs_plugin_loader_job_process_async (monitor->plugin_loader,
						    plugin_job,
						    monitor->cancellable,
						    install_language_pack_cb,
						    with_app_data);
	}
}

/*
 * determines active locale and looks for langpacks
 * installs located language pack, if not already
 */
static void
check_language_pack (GsUpdateMonitor *monitor) {

	const gchar *locale;
	g_autoptr(GsPluginJob) plugin_job = NULL;

	locale = setlocale (LC_MESSAGES, NULL);
	plugin_job = gs_plugin_job_newv (GS_PLUGIN_ACTION_GET_LANGPACKS,
					 "search", locale,
					 "refine-flags", GS_PLUGIN_REFINE_FLAGS_REQUIRE_ICON,
					 NULL);
	gs_plugin_loader_job_process_async (monitor->plugin_loader,
					    plugin_job,
					    monitor->cancellable,
					    get_language_pack_cb,
					    monitor);
}

static void
check_updates (GsUpdateMonitor *monitor)
{
	gint64 tmp;
	gboolean refresh_on_metered;
	g_autoptr(GDateTime) last_refreshed = NULL;
	g_autoptr(GsPluginJob) plugin_job = NULL;

	/* never check for updates when offline */
	if (!gs_plugin_loader_get_network_available (monitor->plugin_loader))
		return;

	/* check for language pack */
	check_language_pack (monitor);

#ifdef HAVE_MOGWAI
	refresh_on_metered = TRUE;
#else
	refresh_on_metered = g_settings_get_boolean (monitor->settings,
						     "refresh-when-metered");
#endif

	if (!refresh_on_metered &&
	    gs_plugin_loader_get_network_metered (monitor->plugin_loader))
		return;

	/* never refresh when the battery is low */
	if (monitor->proxy_upower != NULL) {
		g_autoptr(GVariant) val = NULL;
		val = g_dbus_proxy_get_cached_property (monitor->proxy_upower,
							"WarningLevel");
		if (val != NULL) {
			guint32 level = g_variant_get_uint32 (val);
			if (level >= UP_DEVICE_LEVEL_LOW) {
				g_debug ("not getting updates on low power");
				return;
			}
		}
	} else {
		g_debug ("no UPower support, so not doing power level checks");
	}

#if GLIB_CHECK_VERSION(2, 69, 1)
	/* never refresh when in power saver mode */
	if (monitor->power_profile_monitor != NULL) {
		if (g_power_profile_monitor_get_power_saver_enabled (monitor->power_profile_monitor)) {
			g_debug ("Not getting updates with power saver enabled");
			return;
		}
	} else {
		g_debug ("No power profile monitor support, so not doing power profile checks");
	}
#endif

	g_settings_get (monitor->settings, "check-timestamp", "x", &tmp);
	last_refreshed = g_date_time_new_from_unix_local (tmp);
	if (last_refreshed != NULL) {
		gint now_year, now_month, now_day, now_hour;
		gint year, month, day;
		g_autoptr(GDateTime) now = NULL;

		now = g_date_time_new_now_local ();

		g_date_time_get_ymd (now, &now_year, &now_month, &now_day);
		now_hour = g_date_time_get_hour (now);

		g_date_time_get_ymd (last_refreshed, &year, &month, &day);

		/* check that it is the next day */
		if (!((now_year > year) ||
		      (now_year == year && now_month > month) ||
		      (now_year == year && now_month == month && now_day > day)))
			return;

		/* ...and past 6am */
		if (!(now_hour >= 6))
			return;
	}

	if (!should_download_updates (monitor)) {
		get_updates (monitor);
		return;
	}

	g_debug ("Daily update check due");
	plugin_job = gs_plugin_job_refresh_metadata_new (60 * 60 * 24,
							 GS_PLUGIN_REFRESH_METADATA_FLAGS_NONE);
	gs_plugin_loader_job_process_async (monitor->plugin_loader, plugin_job,
					    monitor->refresh_cancellable,
					    refresh_cache_finished_cb,
					    monitor);
}

static gboolean
check_hourly_cb (gpointer data)
{
	GsUpdateMonitor *monitor = data;

	g_debug ("Hourly updates check");
	check_updates (monitor);

	return G_SOURCE_CONTINUE;
}

static gboolean
check_thrice_daily_cb (gpointer data)
{
	GsUpdateMonitor *monitor = data;

	g_debug ("Daily upgrades check");
	get_upgrades (monitor);
	get_system (monitor);

	return G_SOURCE_CONTINUE;
}

static void
stop_upgrades_check (GsUpdateMonitor *monitor)
{
	if (monitor->check_daily_id == 0)
		return;

	g_source_remove (monitor->check_daily_id);
	monitor->check_daily_id = 0;
}

static void
restart_upgrades_check (GsUpdateMonitor *monitor)
{
	stop_upgrades_check (monitor);
	get_upgrades (monitor);

	monitor->check_daily_id = g_timeout_add_seconds (SECONDS_IN_A_DAY / 3,
							 check_thrice_daily_cb,
							 monitor);
}

static void
stop_updates_check (GsUpdateMonitor *monitor)
{
	if (monitor->check_hourly_id == 0)
		return;

	g_source_remove (monitor->check_hourly_id);
	monitor->check_hourly_id = 0;
}

static void
restart_updates_check (GsUpdateMonitor *monitor)
{
	stop_updates_check (monitor);
	check_updates (monitor);

	monitor->check_hourly_id = g_timeout_add_seconds (SECONDS_IN_AN_HOUR, check_hourly_cb,
							  monitor);
}

static gboolean
check_updates_on_startup_cb (gpointer data)
{
	GsUpdateMonitor *monitor = data;

	g_debug ("First hourly updates check");
	restart_updates_check (monitor);

	if (gs_plugin_loader_get_allow_updates (monitor->plugin_loader))
		restart_upgrades_check (monitor);

	monitor->check_startup_id = 0;
	return G_SOURCE_REMOVE;
}

static void
check_updates_upower_changed_cb (GDBusProxy *proxy,
				 GParamSpec *pspec,
				 GsUpdateMonitor *monitor)
{
	g_debug ("upower changed updates check");
	check_updates (monitor);
}

static void
network_available_notify_cb (GsPluginLoader *plugin_loader,
			     GParamSpec *pspec,
			     GsUpdateMonitor *monitor)
{
	check_updates (monitor);
}

static void
get_updates_historical_cb (GObject *object, GAsyncResult *res, gpointer data)
{
	GsUpdateMonitor *monitor = data;
	GsApp *app;
	const gchar *message;
	const gchar *title;
	guint64 time_last_notified;
	g_autoptr(GError) error = NULL;
	g_autoptr(GsAppList) apps = NULL;
	g_autoptr(GNotification) notification = NULL;

	/* get result */
	apps = gs_plugin_loader_job_process_finish (GS_PLUGIN_LOADER (object), res, &error);
	if (apps == NULL) {

		/* save this in case the user clicks the
		 * 'Show Details' button from the notification below */
		g_clear_error (&monitor->last_offline_error);
		monitor->last_offline_error = g_error_copy (error);

		/* TRANSLATORS: title when we offline updates have failed */
		notification = g_notification_new (_("Software Updates Failed"));
		/* TRANSLATORS: message when we offline updates have failed */
		g_notification_set_body (notification, _("An important operating system update failed to be installed."));
		g_notification_add_button (notification, _("Show Details"), "app.show-offline-update-error");
		g_notification_set_default_action (notification, "app.show-offline-update-error");
		g_application_send_notification (monitor->application, "offline-updates", notification);
		return;
	}

	/* no results */
	if (gs_app_list_length (apps) == 0) {
		g_debug ("no historical updates; withdrawing notification");
		g_application_withdraw_notification (monitor->application,
						     "updates-available");
		return;
	}

	/* have we notified about this before */
	app = gs_app_list_index (apps, 0);
	g_settings_get (monitor->settings,
			"install-timestamp", "x", &time_last_notified);
	if (time_last_notified >= gs_app_get_install_date (app))
		return;

	if (gs_app_get_kind (app) == AS_COMPONENT_KIND_OPERATING_SYSTEM) {
		/* TRANSLATORS: Notification title when we've done a distro upgrade */
		notification = g_notification_new (_("System Upgrade Complete"));

		/* TRANSLATORS: This is the notification body when we've done a
		 * distro upgrade. First %s is the distro name and the 2nd %s
		 * is the version, e.g. "Welcome to Fedora 28!" */
		message = g_strdup_printf (_("Welcome to %s %s!"),
		                           gs_app_get_name (app),
		                           gs_app_get_version (app));
		g_notification_set_body (notification, message);
	} else {
		/* TRANSLATORS: title when we've done offline updates */
		title = ngettext ("Software Update Installed",
				  "Software Updates Installed",
				  gs_app_list_length (apps));
		/* TRANSLATORS: message when we've done offline updates */
		message = ngettext ("An important operating system update has been installed.",
				    "Important operating system updates have been installed.",
				    gs_app_list_length (apps));

		notification = g_notification_new (title);
		g_notification_set_body (notification, message);
		/* TRANSLATORS: Button to look at the updates that were installed.
		 * Note that it has nothing to do with the application reviews, the
		 * users can't express their opinions here. In some languages
		 * "Review (evaluate) something" is a different translation than
		 * "Review (browse) something." */
		g_notification_add_button_with_target (notification, C_("updates", "Review"), "app.set-mode", "s", "updated");
		g_notification_set_default_action_and_target (notification, "app.set-mode", "s", "updated");
	}
	g_application_send_notification (monitor->application, "offline-updates", notification);

	/* update the timestamp so we don't show again */
	g_settings_set (monitor->settings,
			"install-timestamp", "x", gs_app_get_install_date (app));

	reset_update_notification_timestamp (monitor);
}

static gboolean
cleanup_notifications_cb (gpointer user_data)
{
	GsUpdateMonitor *monitor = user_data;
	g_autoptr(GsPluginJob) plugin_job = NULL;

	/* this doesn't do any network access */
	g_debug ("getting historical updates for fresh session");
	plugin_job = gs_plugin_job_newv (GS_PLUGIN_ACTION_GET_UPDATES_HISTORICAL,
					 "refine-flags", GS_PLUGIN_REFINE_FLAGS_REQUIRE_VERSION,
					 NULL);
	gs_plugin_loader_job_process_async (monitor->plugin_loader,
					    plugin_job,
					    monitor->cancellable,
					    get_updates_historical_cb,
					    monitor);

	/* wait until first check to show */
	g_application_withdraw_notification (monitor->application,
					     "updates-available");

	monitor->cleanup_notifications_id = 0;
	return G_SOURCE_REMOVE;
}

void
gs_update_monitor_show_error (GsUpdateMonitor *monitor, GtkWindow *window)
{
	const gchar *title;
	const gchar *msg;
	gboolean show_detailed_error;

	/* can this happen in reality? */
	if (monitor->last_offline_error == NULL)
		return;

	/* TRANSLATORS: this is when the offline update failed */
	title = _("Failed To Update");

	if (g_error_matches (monitor->last_offline_error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_NOT_SUPPORTED)) {
		/* TRANSLATORS: the user must have updated manually after
		 * the updates were prepared */
		msg = _("The system was already up to date.");
		show_detailed_error = TRUE;
	} else if (g_error_matches (monitor->last_offline_error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_CANCELLED) ||
		   g_error_matches (monitor->last_offline_error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		/* TRANSLATORS: the user aborted the update manually */
		msg = _("The update was cancelled.");
		show_detailed_error = FALSE;
	} else if (g_error_matches (monitor->last_offline_error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_NO_NETWORK)) {
		/* TRANSLATORS: the package manager needed to download
		 * something with no network available */
		msg = _("Internet access was required but wasn’t available. "
			"Please make sure that you have internet access and try again.");
		show_detailed_error = FALSE;
	} else if (g_error_matches (monitor->last_offline_error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_NO_SECURITY)) {
		/* TRANSLATORS: if the package is not signed correctly */
		msg = _("There were security issues with the update. "
			"Please consult your software provider for more details.");
		show_detailed_error = TRUE;
	} else if (g_error_matches (monitor->last_offline_error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_NO_SPACE)) {
		/* TRANSLATORS: we ran out of disk space */
		msg = _("There wasn’t enough disk space. Please free up some space and try again.");
		show_detailed_error = FALSE;
	} else {
		/* TRANSLATORS: We didn't handle the error type */
		msg = _("We’re sorry: the update failed to install. "
			"Please wait for another update and try again. "
			"If the problem persists, contact your software provider.");
		show_detailed_error = TRUE;
	}

	gs_utils_show_error_dialog (window,
	                            title,
	                            msg,
	                            show_detailed_error ? monitor->last_offline_error->message : NULL);
}

static void
allow_updates_notify_cb (GsPluginLoader *plugin_loader,
			 GParamSpec *pspec,
			 GsUpdateMonitor *monitor)
{
	if (gs_plugin_loader_get_allow_updates (plugin_loader)) {
		/* We restart the updates check here to avoid the user
		 * potentially waiting for the hourly check */
		restart_updates_check (monitor);
		restart_upgrades_check (monitor);
	} else {
		stop_upgrades_check (monitor);
	}
}

static void
gs_update_monitor_network_changed_cb (GNetworkMonitor *network_monitor,
				      gboolean available,
				      GsUpdateMonitor *monitor)
{
	/* cancel an on-going refresh if we're now in a metered connection */
	if (!g_settings_get_boolean (monitor->settings, "refresh-when-metered") &&
	    g_network_monitor_get_network_metered (network_monitor)) {
		g_cancellable_cancel (monitor->refresh_cancellable);
		g_object_unref (monitor->refresh_cancellable);
		monitor->refresh_cancellable = g_cancellable_new ();
	}
}

#if GLIB_CHECK_VERSION(2, 69, 1)
static void
gs_update_monitor_power_profile_changed_cb (GObject    *object,
                                            GParamSpec *pspec,
                                            gpointer    user_data)
{
	GsUpdateMonitor *self = GS_UPDATE_MONITOR (user_data);

	if (g_power_profile_monitor_get_power_saver_enabled (self->power_profile_monitor)) {
		/* Cancel ongoing jobs, if we’re now in power saving mode. */
		g_cancellable_cancel (self->refresh_cancellable);
		g_object_unref (self->refresh_cancellable);
		self->refresh_cancellable = g_cancellable_new ();

		g_cancellable_cancel (self->cancellable);
		g_object_unref (self->cancellable);
		self->cancellable = g_cancellable_new ();
	} else {
		/* Else, it might be time to check for updates */
		check_updates (self);
	}
}
#endif

static void
gs_update_monitor_init (GsUpdateMonitor *monitor)
{
	GNetworkMonitor *network_monitor;
	g_autoptr(GError) error = NULL;
	monitor->settings = g_settings_new ("org.gnome.software");

	/* cleanup at startup */
	monitor->cleanup_notifications_id =
		g_idle_add (cleanup_notifications_cb, monitor);

	/* do a first check 60 seconds after login, and then every hour */
	monitor->check_startup_id =
		g_timeout_add_seconds (60, check_updates_on_startup_cb, monitor);

	/* we use two cancellables because we want to be able to cancel refresh
	 * operations more opportunistically than other operations, since
	 * they’re less important and cancelling them doesn’t result in much
	 * wasted work */
	monitor->cancellable = g_cancellable_new ();
	monitor->refresh_cancellable = g_cancellable_new ();

	/* connect to UPower to get the system power state */
	monitor->proxy_upower = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
					G_DBUS_PROXY_FLAGS_NONE,
					NULL,
					"org.freedesktop.UPower",
					"/org/freedesktop/UPower/devices/DisplayDevice",
					"org.freedesktop.UPower.Device",
					NULL,
					&error);
	if (monitor->proxy_upower != NULL) {
		g_signal_connect (monitor->proxy_upower, "notify",
				  G_CALLBACK (check_updates_upower_changed_cb),
				  monitor);
	} else {
		g_warning ("failed to connect to upower: %s", error->message);
	}

	network_monitor = g_network_monitor_get_default ();
	if (network_monitor != NULL) {
		monitor->network_monitor = g_object_ref (network_monitor);
		monitor->network_changed_handler = g_signal_connect (monitor->network_monitor,
								     "network-changed",
								     G_CALLBACK (gs_update_monitor_network_changed_cb),
								     monitor);
	}

#if GLIB_CHECK_VERSION(2, 69, 1)
	monitor->power_profile_monitor = g_power_profile_monitor_dup_default ();
	if (monitor->power_profile_monitor != NULL)
		monitor->power_profile_changed_handler = g_signal_connect (monitor->power_profile_monitor,
									   "notify::power-saver-enabled",
									   G_CALLBACK (gs_update_monitor_power_profile_changed_cb),
									   monitor);
#endif
}

static void
gs_update_monitor_dispose (GObject *object)
{
	GsUpdateMonitor *monitor = GS_UPDATE_MONITOR (object);

	if (monitor->network_changed_handler != 0) {
		g_signal_handler_disconnect (monitor->network_monitor,
					     monitor->network_changed_handler);
		monitor->network_changed_handler = 0;
	}

#if GLIB_CHECK_VERSION(2, 69, 1)
	g_clear_signal_handler (&monitor->power_profile_changed_handler, monitor->power_profile_monitor);
	g_clear_object (&monitor->power_profile_monitor);
#endif

	g_cancellable_cancel (monitor->cancellable);
	g_clear_object (&monitor->cancellable);
	g_cancellable_cancel (monitor->refresh_cancellable);
	g_clear_object (&monitor->refresh_cancellable);

	stop_updates_check (monitor);
	stop_upgrades_check (monitor);

	if (monitor->check_startup_id != 0) {
		g_source_remove (monitor->check_startup_id);
		monitor->check_startup_id = 0;
	}
	if (monitor->cleanup_notifications_id != 0) {
		g_source_remove (monitor->cleanup_notifications_id);
		monitor->cleanup_notifications_id = 0;
	}
	if (monitor->plugin_loader != NULL) {
		g_signal_handlers_disconnect_by_func (monitor->plugin_loader,
						      network_available_notify_cb,
						      monitor);
		monitor->plugin_loader = NULL;
	}
	g_clear_object (&monitor->settings);
	g_clear_object (&monitor->proxy_upower);

	G_OBJECT_CLASS (gs_update_monitor_parent_class)->dispose (object);
}

static void
gs_update_monitor_finalize (GObject *object)
{
	GsUpdateMonitor *monitor = GS_UPDATE_MONITOR (object);

	g_application_release (monitor->application);
	g_clear_error (&monitor->last_offline_error);

	G_OBJECT_CLASS (gs_update_monitor_parent_class)->finalize (object);
}

static void
gs_update_monitor_class_init (GsUpdateMonitorClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->dispose = gs_update_monitor_dispose;
	object_class->finalize = gs_update_monitor_finalize;
}

GsUpdateMonitor *
gs_update_monitor_new (GsApplication *application)
{
	GsUpdateMonitor *monitor;

	monitor = GS_UPDATE_MONITOR (g_object_new (GS_TYPE_UPDATE_MONITOR, NULL));
	monitor->application = G_APPLICATION (application);
	g_application_hold (monitor->application);

	monitor->plugin_loader = gs_application_get_plugin_loader (application);
	g_signal_connect (monitor->plugin_loader, "notify::allow-updates",
			  G_CALLBACK (allow_updates_notify_cb), monitor);
	g_signal_connect (monitor->plugin_loader, "notify::network-available",
			  G_CALLBACK (network_available_notify_cb), monitor);

	return monitor;
}
