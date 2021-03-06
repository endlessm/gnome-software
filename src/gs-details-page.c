/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 * vi:set noexpandtab tabstop=8 shiftwidth=8:
 *
 * Copyright (C) 2013-2017 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2013 Matthias Clasen <mclasen@redhat.com>
 * Copyright (C) 2014-2019 Kalev Lember <klember@redhat.com>
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#include "config.h"

#include <locale.h>
#include <string.h>
#include <glib/gi18n.h>

#include "gs-common.h"
#include "gs-content-rating.h"
#include "gs-utils.h"

#include "gs-details-page.h"
#include "gs-app-addon-row.h"
#include "gs-history-dialog.h"
#include "gs-origin-popover-row.h"
#include "gs-screenshot-image.h"
#include "gs-star-widget.h"
#include "gs-review-histogram.h"
#include "gs-review-dialog.h"
#include "gs-review-row.h"

/* the number of reviews to show before clicking the 'More Reviews' button */
#define SHOW_NR_REVIEWS_INITIAL		4

static void gs_details_page_refresh_all (GsDetailsPage *self);

typedef enum {
	GS_DETAILS_PAGE_STATE_LOADING,
	GS_DETAILS_PAGE_STATE_READY,
	GS_DETAILS_PAGE_STATE_FAILED
} GsDetailsPageState;

struct _GsDetailsPage
{
	GsPage			 parent_instance;

	GsPluginLoader		*plugin_loader;
	GtkBuilder		*builder;
	GCancellable		*cancellable;
	GCancellable		*app_cancellable;
	GsApp			*app;
	gboolean		 app_copyable;
	GsApp			*app_local_file;
	GsShell			*shell;
	SoupSession		*session;
	gboolean		 enable_reviews;
	gboolean		 show_all_reviews;
	GSettings		*settings;
	GtkSizeGroup		*size_group_origin_popover;
	GPtrArray		*copy_dests;  /* (element-type GFile) (owned) */

	GtkWidget		*application_details_icon;
	GtkWidget		*application_details_summary;
	GtkWidget		*application_details_title;
	GtkWidget		*box_addons;
	GtkWidget		*box_details;
	GtkWidget		*box_details_description;
	GtkWidget		*box_details_support;
	GtkWidget		*box_progress;
	GtkWidget		*box_progress2;
	GtkWidget		*star;
	GtkWidget		*label_review_count;
	GtkWidget		*box_details_screenshot;
	GtkWidget		*box_details_screenshot_main;
	GtkWidget		*box_details_screenshot_scrolledwindow;
	GtkWidget		*box_details_screenshot_thumbnails;
	GtkWidget		*box_details_license_list;
	GtkWidget		*button_details_launch;
	GtkWidget		*button_details_add_shortcut;
	GtkWidget		*button_details_remove_shortcut;
	GtkWidget		*button_details_website;
	GtkWidget		*button_donate;
	GtkWidget		*button_install;
	GtkWidget		*button_update;
	GtkWidget		*button_remove;
	GtkWidget		*button_cancel;
	GtkWidget		*button_more_reviews;
	GtkWidget		*button_copy;
	GtkWidget		*infobar_details_app_norepo;
	GtkWidget		*infobar_details_app_repo;
	GtkWidget		*infobar_details_package_baseos;
	GtkWidget		*infobar_details_repo;
	GtkWidget		*label_progress_percentage;
	GtkWidget		*label_progress_status;
	GtkWidget		*label_addons_uninstalled_app;
	GtkWidget		*label_details_category_title;
	GtkWidget		*label_details_category_value;
	GtkWidget		*label_details_developer_title;
	GtkWidget		*label_details_developer_value;
	GtkWidget		*box_details_developer;
	GtkWidget		*image_details_developer_verified;
	GtkWidget		*button_details_license_free;
	GtkWidget		*button_details_license_nonfree;
	GtkWidget		*button_details_license_unknown;
	GtkWidget		*label_details_license_title;
	GtkWidget		*box_details_license_value;
	GtkWidget		*label_details_channel_title;
	GtkWidget		*label_details_channel_value;
	GtkWidget		*label_details_origin_title;
	GtkWidget		*label_details_origin_value;
	GtkWidget		*label_details_size_installed_title;
	GtkWidget		*label_details_size_installed_value;
	GtkWidget		*label_details_size_download_title;
	GtkWidget		*label_details_size_download_value;
	GtkWidget		*label_details_updated_title;
	GtkWidget		*label_details_updated_value;
	GtkWidget		*label_details_version_title;
	GtkWidget		*label_details_version_value;
	GtkWidget		*label_details_permissions_title;
	GtkWidget		*button_details_permissions_value;
	GtkWidget		*label_failed;
	GtkWidget		*label_license_nonfree_details;
	GtkWidget		*label_licenses_intro;
	GtkWidget		*list_box_addons;
	GtkWidget		*box_reviews;
	GtkWidget		*box_details_screenshot_fallback;
	GtkWidget		*histogram;
	GtkWidget		*button_review;
	GtkWidget		*list_box_reviews;
	GtkWidget		*scrolledwindow_details;
	GtkWidget		*spinner_details;
	GtkWidget		*spinner_remove;
	GtkWidget		*stack_details;
	GtkWidget		*grid_details_kudo;
	GtkWidget		*image_details_kudo_docs;
	GtkWidget		*image_details_kudo_sandboxed;
	GtkWidget		*image_details_kudo_integration;
	GtkWidget		*image_details_kudo_translated;
	GtkWidget		*image_details_kudo_updated;
	GtkWidget		*label_details_kudo_docs;
	GtkWidget		*label_details_kudo_sandboxed;
	GtkWidget		*label_details_kudo_integration;
	GtkWidget		*label_details_kudo_translated;
	GtkWidget		*label_details_kudo_updated;
	GtkWidget		*progressbar_top;
	guint			 progress_pulse_id;
	GtkWidget		*popover_license_free;
	GtkWidget		*popover_license_nonfree;
	GtkWidget		*popover_license_unknown;
	GtkWidget		*popover_content_rating;
	GtkWidget		*label_content_rating_title;
	GtkWidget		*label_content_rating_message;
	GtkWidget		*label_content_rating_none;
	GtkWidget		*button_details_rating_value;
	GtkStyleProvider	*button_details_rating_style_provider;
	GtkWidget		*label_details_rating_title;
	GtkWidget		*popover_permissions;
	GtkWidget		*box_permissions_details;
	GtkWidget		*star_eventbox;
};

G_DEFINE_TYPE (GsDetailsPage, gs_details_page, GS_TYPE_PAGE)

static void
gs_details_page_set_state (GsDetailsPage *self,
                           GsDetailsPageState state)
{
	/* spinner */
	switch (state) {
	case GS_DETAILS_PAGE_STATE_LOADING:
		gs_start_spinner (GTK_SPINNER (self->spinner_details));
		gtk_widget_show (self->spinner_details);
		break;
	case GS_DETAILS_PAGE_STATE_READY:
	case GS_DETAILS_PAGE_STATE_FAILED:
		gs_stop_spinner (GTK_SPINNER (self->spinner_details));
		gtk_widget_hide (self->spinner_details);
		break;
	default:
		g_assert_not_reached ();
	}

	/* stack */
	switch (state) {
	case GS_DETAILS_PAGE_STATE_LOADING:
		gtk_stack_set_visible_child_name (GTK_STACK (self->stack_details), "spinner");
		break;
	case GS_DETAILS_PAGE_STATE_READY:
		gtk_stack_set_visible_child_name (GTK_STACK (self->stack_details), "ready");
		break;
	case GS_DETAILS_PAGE_STATE_FAILED:
		gtk_stack_set_visible_child_name (GTK_STACK (self->stack_details), "failed");
		break;
	default:
		g_assert_not_reached ();
	}
}

static void
gs_details_page_update_shortcut_button (GsDetailsPage *self)
{
	gboolean add_shortcut_func;
	gboolean remove_shortcut_func;
	gboolean has_shortcut;

	gtk_widget_set_visible (self->button_details_add_shortcut,
				FALSE);
	gtk_widget_set_visible (self->button_details_remove_shortcut,
				FALSE);

	if (gs_app_get_kind (self->app) != AS_APP_KIND_DESKTOP)
		return;

	/* Leave the button hidden if the app can’t be launched by the current
	 * user. */
	if (gs_app_has_quirk (self->app, GS_APP_QUIRK_PARENTAL_NOT_LAUNCHABLE))
		return;

	/* leave the button hidden if there's a pending action or we're copying
	 * the app because the progress bar or spinner will be visible */
	if (gs_app_get_pending_action (self->app) != GS_PLUGIN_ACTION_UNKNOWN ||
	    gs_plugin_loader_app_copying (self->plugin_loader, self->app))
		return;

	/* only consider the shortcut button if the app is installed */
	switch (gs_app_get_state (self->app)) {
	case AS_APP_STATE_INSTALLED:
	case AS_APP_STATE_UPDATABLE:
	case AS_APP_STATE_UPDATABLE_LIVE:
		break;
	default:
		return;
	}

	add_shortcut_func =
		gs_plugin_loader_get_plugin_supported (self->plugin_loader,
						       "gs_plugin_add_shortcut");
	remove_shortcut_func =
		gs_plugin_loader_get_plugin_supported (self->plugin_loader,
						       "gs_plugin_remove_shortcut");

	has_shortcut = gs_app_has_quirk (self->app, GS_APP_QUIRK_HAS_SHORTCUT);

	if (add_shortcut_func) {
		gtk_widget_set_visible (self->button_details_add_shortcut,
					!has_shortcut || !remove_shortcut_func);
		gtk_widget_set_sensitive (self->button_details_add_shortcut,
					  !has_shortcut);
	}

	if (remove_shortcut_func) {
		gtk_widget_set_visible (self->button_details_remove_shortcut,
					has_shortcut || !add_shortcut_func);
		gtk_widget_set_sensitive (self->button_details_remove_shortcut,
					  has_shortcut);
	}
}

static GFile *
get_removable_destination (GsDetailsPage *self)
{
	/* first one is the most-recently added mount on a removable disk */
	if (self->copy_dests != NULL && self->copy_dests->len > 0)
		return self->copy_dests->pdata[0];

	return NULL;
}

static void
gs_details_page_update_copy_button (GsDetailsPage *self)
{
	GFile *copy_dest;

	gtk_widget_set_sensitive (self->button_copy, FALSE);
	gtk_widget_set_visible (self->button_copy, FALSE);

	if (gs_plugin_loader_app_copying (self->plugin_loader, self->app))
		return;

	copy_dest = get_removable_destination (self);
	if (self->app != NULL && gs_app_is_installed (self->app) && self->app_copyable) {
		if (copy_dest != NULL) {
			gtk_button_set_label (GTK_BUTTON (self->button_copy), _("Copy to US_B"));
			gtk_widget_set_sensitive (self->button_copy, TRUE);
		} else {
			gtk_button_set_label (GTK_BUTTON (self->button_copy), _("Insert USB to Copy To"));
			gtk_widget_set_sensitive (self->button_copy, FALSE);
		}
		if (!gs_plugin_loader_copy_queue_empty (self->plugin_loader))  {
			gtk_button_set_label (GTK_BUTTON (self->button_copy), _("Pending Copy"));
			gtk_widget_set_sensitive (self->button_copy, FALSE);
		}
		gtk_widget_set_visible (self->button_copy, TRUE);
	}
}

static void
gs_details_page_app_get_copyable_cb (GObject *source_object,
				     GAsyncResult *res,
				     gpointer user_data)
{
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (source_object);
	GsDetailsPage *self = GS_DETAILS_PAGE (user_data);
	g_autoptr(GError) error = NULL;
	gboolean copyable;

	copyable = gs_plugin_loader_job_app_get_copyable_finish (plugin_loader,
								 res, &error);
	g_debug ("%s: copyable = %s", G_STRFUNC, copyable ? "yes" : "no");
	self->app_copyable = copyable;
	gs_details_page_update_copy_button (self);
}

static gboolean
app_has_pending_action (GsApp *app)
{
	/* sanitize the pending state change by verifying we're in one of the
	 * expected states */
	if (gs_app_get_state (app) != AS_APP_STATE_AVAILABLE &&
	    gs_app_get_state (app) != AS_APP_STATE_UPDATABLE_LIVE &&
	    gs_app_get_state (app) != AS_APP_STATE_UPDATABLE &&
	    gs_app_get_state (app) != AS_APP_STATE_QUEUED_FOR_INSTALL)
		return FALSE;

	return (gs_app_get_pending_action (app) != GS_PLUGIN_ACTION_UNKNOWN) ||
	       (gs_app_get_state (app) == AS_APP_STATE_QUEUED_FOR_INSTALL);
}

static gboolean
plugin_has_pending_action (GsDetailsPage *self)
{
	return (gs_plugin_loader_app_copying (self->plugin_loader, self->app)) ||
		app_has_pending_action (self->app);
}

static void
gs_details_page_switch_to (GsPage *page, gboolean scroll_up)
{
	GsDetailsPage *self = GS_DETAILS_PAGE (page);
	GtkWidget *widget;
	GtkAdjustment *adj;

	if (gs_shell_get_mode (self->shell) != GS_SHELL_MODE_DETAILS) {
		g_warning ("Called switch_to(details) when in mode %s",
			   gs_shell_get_mode_string (self->shell));
		return;
	}

	widget = GTK_WIDGET (gtk_builder_get_object (self->builder, "application_details_header"));
	gtk_label_set_label (GTK_LABEL (widget), "");
	gtk_widget_show (widget);

	/* not set, perhaps file-to-app */
	if (self->app == NULL)
		return;

	adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->scrolledwindow_details));
	gtk_adjustment_set_value (adj, gtk_adjustment_get_lower (adj));

	gs_grab_focus_when_mapped (self->scrolledwindow_details);
}

static gboolean
_pulse_cb (gpointer user_data)
{
	GsDetailsPage *self = GS_DETAILS_PAGE (user_data);

	gtk_progress_bar_pulse (GTK_PROGRESS_BAR (self->progressbar_top));

	return G_SOURCE_CONTINUE;
}

static void
stop_progress_pulsing (GsDetailsPage *self)
{
	if (self->progress_pulse_id != 0) {
		g_source_remove (self->progress_pulse_id);
		self->progress_pulse_id = 0;
	}
}

static void
gs_details_page_refresh_progress (GsDetailsPage *self)
{
	guint percentage;
	AsAppState state;

	/* cancel button */
	state = gs_app_get_state (self->app);
	switch (state) {
	case AS_APP_STATE_INSTALLING:
		gtk_widget_set_visible (self->button_cancel, TRUE);
		/* If the app is installing, the user can only cancel it if
		 * 1) They haven't already, and
		 * 2) the plugin hasn't said that they can't, for example if a
		 *    package manager has already gone 'too far'
		 */
		gtk_widget_set_sensitive (self->button_cancel,
					  !g_cancellable_is_cancelled (self->app_cancellable) &&
					   gs_app_get_allow_cancel (self->app));
		break;
	default:
		gtk_widget_set_visible (self->button_cancel, FALSE);
		break;
	}
	if (plugin_has_pending_action (self)) {
		gtk_widget_set_visible (self->button_cancel, TRUE);
		gtk_widget_set_sensitive (self->button_cancel,
					  !g_cancellable_is_cancelled (self->app_cancellable) &&
					  gs_app_get_allow_cancel (self->app));
	}

	/* progress status label */
	if (state == AS_APP_STATE_REMOVING) {
		gtk_widget_set_visible (self->label_progress_status, TRUE);
		gtk_label_set_label (GTK_LABEL (self->label_progress_status),
				     _("Removing…"));
	} else if (state == AS_APP_STATE_INSTALLING) {
		gtk_widget_set_visible (self->label_progress_status, TRUE);
		gtk_label_set_label (GTK_LABEL (self->label_progress_status),
				     _("Installing"));
	} else if (gs_plugin_loader_app_copying (self->plugin_loader, self->app)) {
		gtk_widget_set_visible (self->label_progress_status, TRUE);
		gtk_label_set_label (GTK_LABEL (self->label_progress_status),
				     _("Copying"));
	} else {
		gtk_widget_set_visible (self->label_progress_status, FALSE);
	}
	if (app_has_pending_action (self->app)) {
		GsPluginAction action = gs_app_get_pending_action (self->app);
		gtk_widget_set_visible (self->label_progress_status, TRUE);
		switch (action) {
		case GS_PLUGIN_ACTION_INSTALL:
			gtk_label_set_label (GTK_LABEL (self->label_progress_status),
					     /* TRANSLATORS: This is a label on top of the app's progress
					      * bar to inform the user that the app should be installed soon */
					     _("Pending installation…"));
			break;
		case GS_PLUGIN_ACTION_UPDATE:
		case GS_PLUGIN_ACTION_UPGRADE_DOWNLOAD:
			gtk_label_set_label (GTK_LABEL (self->label_progress_status),
					     /* TRANSLATORS: This is a label on top of the app's progress
					      * bar to inform the user that the app should be updated soon */
					     _("Pending update…"));
			break;
		default:
			gtk_widget_set_visible (self->label_progress_status, FALSE);
			break;
		}
	}

	/* percentage bar */
	switch (state) {
	case AS_APP_STATE_INSTALLING:
		percentage = gs_app_get_progress (self->app);
		if (percentage == GS_APP_PROGRESS_UNKNOWN) {
			/* Translators: This string is shown when preparing to download and install an app. */
			gtk_label_set_label (GTK_LABEL (self->label_progress_status), _("Preparing…"));
			gtk_widget_set_visible (self->label_progress_status, TRUE);
			gtk_widget_set_visible (self->label_progress_percentage, FALSE);

			if (self->progress_pulse_id == 0)
				self->progress_pulse_id = g_timeout_add (50, _pulse_cb, self);

			gtk_widget_set_visible (self->progressbar_top, TRUE);
			break;
		} else if (percentage <= 100) {
			g_autofree gchar *str = g_strdup_printf ("%u%%", percentage);
			gtk_label_set_label (GTK_LABEL (self->label_progress_percentage), str);
			gtk_widget_set_visible (self->label_progress_percentage, TRUE);
			stop_progress_pulsing (self);
			gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (self->progressbar_top),
						       (gdouble) percentage / 100.f);
			gtk_widget_set_visible (self->progressbar_top, TRUE);
			break;
		}
		/* FALLTHROUGH */
	default:
		gtk_widget_set_visible (self->label_progress_percentage, FALSE);
		gtk_widget_set_visible (self->progressbar_top, FALSE);
		stop_progress_pulsing (self);
		break;
	}
	if (app_has_pending_action (self->app)) {
		gtk_widget_set_visible (self->progressbar_top, TRUE);
		gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (self->progressbar_top), 0);
	}

	/* spinner */
	if (state == AS_APP_STATE_REMOVING ||
	    gs_plugin_loader_app_copying (self->plugin_loader, self->app)) {
		if (!gtk_widget_get_visible (self->spinner_remove)) {
			gtk_spinner_start (GTK_SPINNER (self->spinner_remove));
			gtk_widget_set_visible (self->spinner_remove, TRUE);
		}
		/* align text together with the spinner if we're showing it */
		gtk_widget_set_halign (self->box_progress2, GTK_ALIGN_START);
	} else {
		gtk_widget_set_visible (self->spinner_remove, FALSE);
		gtk_spinner_stop (GTK_SPINNER (self->spinner_remove));
		gtk_widget_set_halign (self->box_progress2, GTK_ALIGN_CENTER);
	}

	/* progress box */
	switch (state) {
	case AS_APP_STATE_REMOVING:
	case AS_APP_STATE_INSTALLING:
		gtk_widget_set_visible (self->box_progress, TRUE);
		break;
	default:
		gtk_widget_set_visible (self->box_progress, FALSE);
		break;
	}
	if (plugin_has_pending_action (self))
		gtk_widget_set_visible (self->box_progress, TRUE);
}

static gboolean
gs_details_page_refresh_progress_idle (gpointer user_data)
{
	GsDetailsPage *self = GS_DETAILS_PAGE (user_data);
	gs_details_page_refresh_progress (self);
	g_object_unref (self);
	return G_SOURCE_REMOVE;
}

static void
gs_details_page_progress_changed_cb (GsApp *app,
                                     GParamSpec *pspec,
                                     GsDetailsPage *self)
{
	g_idle_add (gs_details_page_refresh_progress_idle, g_object_ref (self));
}

static gboolean
gs_details_page_allow_cancel_changed_idle (gpointer user_data)
{
	GsDetailsPage *self = GS_DETAILS_PAGE (user_data);
	gtk_widget_set_sensitive (self->button_cancel,
				  gs_app_get_allow_cancel (self->app));
	g_object_unref (self);
	return G_SOURCE_REMOVE;
}

static void
gs_details_page_allow_cancel_changed_cb (GsApp *app,
                                                    GParamSpec *pspec,
                                                    GsDetailsPage *self)
{
	g_idle_add (gs_details_page_allow_cancel_changed_idle,
		    g_object_ref (self));
}

static gboolean
gs_details_page_switch_to_idle (gpointer user_data)
{
	GsDetailsPage *self = GS_DETAILS_PAGE (user_data);

	if (gs_shell_get_mode (self->shell) == GS_SHELL_MODE_DETAILS)
		gs_page_switch_to (GS_PAGE (self), TRUE);

	/* update widgets */
	gs_details_page_refresh_all (self);

	g_object_unref (self);
	return G_SOURCE_REMOVE;
}

static void
gs_details_page_notify_state_changed_cb (GsApp *app,
                                         GParamSpec *pspec,
                                         GsDetailsPage *self)
{
	g_idle_add (gs_details_page_switch_to_idle, g_object_ref (self));
}

static void
gs_details_page_load_main_screenshot (GsDetailsPage *self,
				      AsScreenshot *screenshot)
{
	GsScreenshotImage *ssmain;
	g_autoptr(GList) children = NULL;

	children = gtk_container_get_children (GTK_CONTAINER (self->box_details_screenshot_main));
	ssmain = GS_SCREENSHOT_IMAGE (children->data);

	gs_screenshot_image_set_screenshot (ssmain, screenshot);
	gs_screenshot_image_load_async (ssmain, NULL);
}

static void
gs_details_page_screenshot_selected_cb (GtkListBox *list,
                                        GtkListBoxRow *row,
                                        GsDetailsPage *self)
{
	GsScreenshotImage *ssthumb;
	AsScreenshot *ss;
	g_autoptr(GList) children = NULL;

	if (row == NULL)
		return;

	ssthumb = GS_SCREENSHOT_IMAGE (gtk_bin_get_child (GTK_BIN (row)));
	ss = gs_screenshot_image_get_screenshot (ssthumb);

	gs_details_page_load_main_screenshot (self, ss);
}

static void
gs_details_page_refresh_screenshots (GsDetailsPage *self)
{
	GPtrArray *screenshots;
	AsScreenshot *ss;
	GtkWidget *label;
	GtkWidget *list;
	GtkWidget *ssimg;
	GtkWidget *main_screenshot = NULL;
	guint i;
	gboolean is_offline = !gs_plugin_loader_get_network_available (self->plugin_loader);
	guint num_screenshots_loaded = 0;

	/* reset the visibility of screenshots */
	gtk_widget_show (self->box_details_screenshot);

	/* treat screenshots differently */
	if (gs_app_get_kind (self->app) == AS_APP_KIND_FONT) {
		gs_container_remove_all (GTK_CONTAINER (self->box_details_screenshot_thumbnails));
		gs_container_remove_all (GTK_CONTAINER (self->box_details_screenshot_main));
		screenshots = gs_app_get_screenshots (self->app);
		for (i = 0; i < screenshots->len; i++) {
			ss = g_ptr_array_index (screenshots, i);

			/* set caption */
			label = gtk_label_new (as_screenshot_get_caption (ss, NULL));
			g_object_set (label,
				      "xalign", 0.0,
				      "max-width-chars", 10,
				      "wrap", TRUE,
				      NULL);
			gtk_container_add (GTK_CONTAINER (self->box_details_screenshot_main), label);
			gtk_widget_set_visible (label, TRUE);

			/* set images */
			ssimg = gs_screenshot_image_new (self->session);
			gs_screenshot_image_set_screenshot (GS_SCREENSHOT_IMAGE (ssimg), ss);
			gs_screenshot_image_set_size (GS_SCREENSHOT_IMAGE (ssimg),
						      640,
						      48);
			gs_screenshot_image_load_async (GS_SCREENSHOT_IMAGE (ssimg), NULL);
			gtk_container_add (GTK_CONTAINER (self->box_details_screenshot_main), ssimg);
			gtk_widget_set_visible (ssimg, TRUE);
		}
		gtk_widget_set_visible (self->box_details_screenshot,
		                        screenshots->len > 0);
		gtk_widget_set_visible (self->box_details_screenshot_fallback,
		                        screenshots->len == 0 && !is_offline);
		return;
	}

	/* fallback warning */
	screenshots = gs_app_get_screenshots (self->app);
	switch (gs_app_get_kind (self->app)) {
	case AS_APP_KIND_GENERIC:
	case AS_APP_KIND_CODEC:
	case AS_APP_KIND_ADDON:
	case AS_APP_KIND_SOURCE:
	case AS_APP_KIND_FIRMWARE:
	case AS_APP_KIND_DRIVER:
	case AS_APP_KIND_INPUT_METHOD:
	case AS_APP_KIND_LOCALIZATION:
	case AS_APP_KIND_RUNTIME:
		gtk_widget_set_visible (self->box_details_screenshot_fallback, FALSE);
		break;
	default:
		gtk_widget_set_visible (self->box_details_screenshot_fallback,
					screenshots->len == 0 && !is_offline);
		break;
	}

	/* reset screenshots */
	gs_container_remove_all (GTK_CONTAINER (self->box_details_screenshot_main));
	gs_container_remove_all (GTK_CONTAINER (self->box_details_screenshot_thumbnails));

	list = gtk_list_box_new ();
	gtk_style_context_add_class (gtk_widget_get_style_context (list), "image-list");
	gtk_widget_show (list);
	gtk_widget_show (self->box_details_screenshot_scrolledwindow);
	gtk_container_add (GTK_CONTAINER (self->box_details_screenshot_thumbnails), list);

	for (i = 0; i < screenshots->len; i++) {
		ss = g_ptr_array_index (screenshots, i);

		/* we need to load the main screenshot only once if we're online
		 * but all times if we're offline (to check which are cached and
		 * hide those who aren't) */
		if (is_offline || main_screenshot == NULL) {
			GtkWidget *ssmain = gs_screenshot_image_new (self->session);
			gtk_widget_set_can_focus (gtk_bin_get_child (GTK_BIN (ssmain)), FALSE);
			gs_screenshot_image_set_screenshot (GS_SCREENSHOT_IMAGE (ssmain), ss);
			gs_screenshot_image_set_size (GS_SCREENSHOT_IMAGE (ssmain),
						      AS_IMAGE_NORMAL_WIDTH,
						      AS_IMAGE_NORMAL_HEIGHT);
			gs_screenshot_image_load_async (GS_SCREENSHOT_IMAGE (ssmain), NULL);

			/* when we're offline, the load will be immediate, so we
			 * can check if it succeeded, and just skip it and its
			 * thumbnails otherwise */
			if (is_offline &&
			    !gs_screenshot_image_is_showing (GS_SCREENSHOT_IMAGE (ssmain)))
				continue;

			/* only set the main_screenshot once */
			if (main_screenshot == NULL) {
				main_screenshot = ssmain;
				gtk_box_pack_start (GTK_BOX (self->box_details_screenshot_main),
						    main_screenshot, FALSE, FALSE, 0);
				gtk_widget_show (main_screenshot);
			}
		}

		ssimg = gs_screenshot_image_new (self->session);
		gs_screenshot_image_set_screenshot (GS_SCREENSHOT_IMAGE (ssimg), ss);
		gs_screenshot_image_set_size (GS_SCREENSHOT_IMAGE (ssimg),
					      AS_IMAGE_THUMBNAIL_WIDTH,
					      AS_IMAGE_THUMBNAIL_HEIGHT);
		gtk_style_context_add_class (gtk_widget_get_style_context (ssimg),
					     "screenshot-image-thumb");
		gs_screenshot_image_load_async (GS_SCREENSHOT_IMAGE (ssimg), NULL);
		gtk_list_box_insert (GTK_LIST_BOX (list), ssimg, -1);
		gtk_widget_set_visible (ssimg, TRUE);
		++num_screenshots_loaded;
	}

	if (main_screenshot == NULL) {
		gtk_widget_hide (self->box_details_screenshot);
		return;
	}

	/* reload the main screenshot with a larger size if it's the only screenshot
	 * available */
	if (num_screenshots_loaded == 1) {
		gs_screenshot_image_set_size (GS_SCREENSHOT_IMAGE (main_screenshot),
					      AS_IMAGE_LARGE_WIDTH,
					      AS_IMAGE_LARGE_HEIGHT);
		gs_screenshot_image_load_async (GS_SCREENSHOT_IMAGE (main_screenshot), NULL);
	}

	if (num_screenshots_loaded <= 1) {
		gtk_widget_hide (self->box_details_screenshot_thumbnails);
		return;
	}

	gtk_widget_show (self->box_details_screenshot_thumbnails);
	gtk_list_box_set_selection_mode (GTK_LIST_BOX (list), GTK_SELECTION_BROWSE);
	g_signal_connect (list, "row-selected",
			  G_CALLBACK (gs_details_page_screenshot_selected_cb),
			  self);
	gtk_list_box_select_row (GTK_LIST_BOX (list),
				 gtk_list_box_get_row_at_index (GTK_LIST_BOX (list), 0));
}

static void
gs_details_page_website_cb (GtkWidget *widget, GsDetailsPage *self)
{
	gs_shell_show_uri (self->shell,
	                   gs_app_get_url (self->app, AS_URL_KIND_HOMEPAGE));
}

static void
gs_details_page_donate_cb (GtkWidget *widget, GsDetailsPage *self)
{
	gs_shell_show_uri (self->shell, gs_app_get_url (self->app, AS_URL_KIND_DONATION));
}

static void
gs_details_page_set_description (GsDetailsPage *self, const gchar *tmp)
{
	GtkStyleContext *style_context;
	GtkWidget *para;
	guint i;
	g_auto(GStrv) split = NULL;

	/* does the description exist? */
	gtk_widget_set_visible (self->box_details_description, tmp != NULL);
	if (tmp == NULL)
		return;

	/* add each paragraph as a new GtkLabel which lets us get the 24px
	 * paragraph spacing */
	gs_container_remove_all (GTK_CONTAINER (self->box_details_description));
	split = g_strsplit (tmp, "\n\n", -1);
	for (i = 0; split[i] != NULL; i++) {
		para = gtk_label_new (split[i]);
		gtk_label_set_line_wrap (GTK_LABEL (para), TRUE);
		gtk_label_set_max_width_chars (GTK_LABEL (para), 40);
		gtk_label_set_selectable (GTK_LABEL (para), TRUE);
		gtk_widget_set_visible (para, TRUE);
		gtk_widget_set_can_focus (para, FALSE);
		g_object_set (para,
			      "xalign", 0.0,
			      NULL);

		/* add style class for theming */
		style_context = gtk_widget_get_style_context (para);
		gtk_style_context_add_class (style_context,
					     "application-details-description");

		gtk_container_add (GTK_CONTAINER (self->box_details_description), para);
	}

	/* show the webapp warning */
	if (gs_app_get_kind (self->app) == AS_APP_KIND_WEB_APP) {
		GtkWidget *label;
		/* TRANSLATORS: this is the warning box */
		label = gtk_label_new (_("This application can only be used when there is an active internet connection."));
		gtk_widget_set_visible (label, TRUE);
		gtk_label_set_xalign (GTK_LABEL (label), 0.f);
		gtk_style_context_add_class (gtk_widget_get_style_context (label),
					     "application-details-webapp-warning");
		gtk_container_add (GTK_CONTAINER (self->box_details_description), label);
	}
}

static void
gs_details_page_set_sensitive (GtkWidget *widget, gboolean is_active)
{
	GtkStyleContext *style_context;
	style_context = gtk_widget_get_style_context (widget);
	if (!is_active) {
		gtk_style_context_add_class (style_context, "dim-label");
	} else {
		gtk_style_context_remove_class (style_context, "dim-label");
	}
}

static gboolean
gs_details_page_history_cb (GtkLabel *label,
                            gchar *uri,
                            GsDetailsPage *self)
{
	GtkWidget *dialog;

	dialog = gs_history_dialog_new ();
	gs_history_dialog_set_app (GS_HISTORY_DIALOG (dialog), self->app);
	gs_shell_modal_dialog_present (self->shell, GTK_DIALOG (dialog));

	/* just destroy */
	g_signal_connect_swapped (dialog, "response",
				  G_CALLBACK (gtk_widget_destroy), dialog);

	return TRUE;
}

static void
gs_details_page_refresh_size (GsDetailsPage *self)
{
	/* set the installed size */
	if (gs_app_get_size_installed (self->app) != GS_APP_SIZE_UNKNOWABLE &&
	    gs_app_get_size_installed (self->app) != 0) {
		g_autofree gchar *size = NULL;
		size = g_format_size (gs_app_get_size_installed (self->app));
		gtk_label_set_label (GTK_LABEL (self->label_details_size_installed_value), size);
		gtk_widget_show (self->label_details_size_installed_title);
		gtk_widget_show (self->label_details_size_installed_value);
	} else {
		gtk_widget_hide (self->label_details_size_installed_title);
		gtk_widget_hide (self->label_details_size_installed_value);
	}

	/* set the download size */
	if (!gs_app_is_installed (self->app) &&
	    gs_app_get_size_download (self->app) != GS_APP_SIZE_UNKNOWABLE) {
		g_autofree gchar *size = NULL;
		size = g_format_size (gs_app_get_size_download (self->app));
		gtk_label_set_label (GTK_LABEL (self->label_details_size_download_value), size);
		gtk_widget_show (self->label_details_size_download_title);
		gtk_widget_show (self->label_details_size_download_value);
	} else {
		gtk_widget_hide (self->label_details_size_download_title);
		gtk_widget_hide (self->label_details_size_download_value);
	}
}

static void
gs_details_page_get_alternates_cb (GObject *source_object,
                                   GAsyncResult *res,
                                   gpointer user_data)
{
	GsDetailsPage *self = GS_DETAILS_PAGE (user_data);
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (source_object);
	g_autoptr(GError) error = NULL;
	g_autoptr(GsAppList) list = NULL;
	GtkWidget *origin_box;
	GtkWidget *origin_button_label;
	GtkWidget *origin_popover_list_box;
	g_autofree gchar *origin_ui = NULL;

	origin_box = GTK_WIDGET (gtk_builder_get_object (self->builder, "origin_box"));
	origin_button_label = GTK_WIDGET (gtk_builder_get_object (self->builder, "origin_button_label"));
	origin_popover_list_box = GTK_WIDGET (gtk_builder_get_object (self->builder, "origin_popover_list_box"));

	gs_container_remove_all (GTK_CONTAINER (origin_popover_list_box));

	list = gs_plugin_loader_job_process_finish (plugin_loader,
						    res,
						    &error);
	if (list == NULL) {
		if (!g_error_matches (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_CANCELLED))
			g_warning ("failed to get alternates: %s", error->message);
		gtk_widget_hide (origin_box);
		return;
	}

	/* add the local file to the list so that we can carry it over when
	 * switching between alternates */
	if (self->app_local_file != NULL)
		gs_app_list_add (list, self->app_local_file);

	/* no alternates to show */
	if (gs_app_list_length (list) < 2) {
		gtk_widget_hide (origin_box);
		return;
	}

	for (guint i = 0; i < gs_app_list_length (list); i++) {
		GsApp *app = gs_app_list_index (list, i);
		GtkWidget *row = gs_origin_popover_row_new (app);
		gtk_widget_show (row);
		if (app == self->app)
			gs_origin_popover_row_set_selected (GS_ORIGIN_POPOVER_ROW (row), TRUE);
		gs_origin_popover_row_set_size_group (GS_ORIGIN_POPOVER_ROW (row),
		                                      self->size_group_origin_popover);
		gtk_container_add (GTK_CONTAINER (origin_popover_list_box), row);
	}

	origin_ui = gs_app_get_origin_ui (self->app);
	if (origin_ui != NULL)
		gtk_label_set_text (GTK_LABEL (origin_button_label), origin_ui);
	else
		gtk_label_set_text (GTK_LABEL (origin_button_label), "");

	gtk_widget_show (origin_box);
}

static void
gs_details_page_refresh_buttons (GsDetailsPage *self)
{
	AsAppState state;
	g_autofree gchar *text = NULL;

	state = gs_app_get_state (self->app);

	/* install button */
	switch (state) {
	case AS_APP_STATE_AVAILABLE:
	case AS_APP_STATE_AVAILABLE_LOCAL:
		gtk_widget_set_visible (self->button_install, TRUE);

		/* we need to show more directly to users that apps needs to be downloaded,
		 * so we're changing the installation button's label to "Download" but
		 * only if the app is not coming from a removable drive (as in which
		 * case they don't need to be downloaded, only installed) */
		if (gs_app_has_category (self->app, "usb")) {
			/* TRANSLATORS: button text in the header when an application
			 * can be installed */
			gtk_button_set_label (GTK_BUTTON (self->button_install), _("_Install"));
		} else {
			/* TRANSLATORS: button text in the header when an application
			 * can be installed but needs to be downloaded first */
			gtk_button_set_label (GTK_BUTTON (self->button_install), _("_Download"));
		}
		break;
	case AS_APP_STATE_INSTALLING:
		gtk_widget_set_visible (self->button_install, FALSE);
		break;
	case AS_APP_STATE_UNKNOWN:
	case AS_APP_STATE_INSTALLED:
	case AS_APP_STATE_REMOVING:
	case AS_APP_STATE_UPDATABLE:
	case AS_APP_STATE_QUEUED_FOR_INSTALL:
		gtk_widget_set_visible (self->button_install, FALSE);
		break;
	case AS_APP_STATE_UPDATABLE_LIVE:
		if (gs_app_get_kind (self->app) == AS_APP_KIND_FIRMWARE) {
			gtk_widget_set_visible (self->button_install, TRUE);
			/* TRANSLATORS: button text in the header when firmware
			 * can be live-installed */
			gtk_button_set_label (GTK_BUTTON (self->button_install), _("_Download"));
		} else {
			gtk_widget_set_visible (self->button_install, FALSE);
		}
		break;
	case AS_APP_STATE_UNAVAILABLE:
		if (gs_app_get_url (self->app, AS_URL_KIND_MISSING) != NULL) {
			gtk_widget_set_visible (self->button_install, FALSE);
		} else {
			gtk_widget_set_visible (self->button_install, TRUE);
			/* TRANSLATORS: this is a button that allows the apps to
			 * be installed.
			 * The ellipsis indicates that further steps are required,
			 * e.g. enabling software repositories or the like */
			gtk_button_set_label (GTK_BUTTON (self->button_install), _("_Download…"));
		}
		break;
	default:
		g_warning ("App unexpectedly in state %s",
			   as_app_state_to_string (state));
		g_assert_not_reached ();
	}

	/* update button */
	switch (state) {
	case AS_APP_STATE_UPDATABLE_LIVE:
		if (gs_app_get_kind (self->app) == AS_APP_KIND_FIRMWARE) {
			gtk_widget_set_visible (self->button_update, FALSE);
		} else {
			gtk_widget_set_visible (self->button_update, TRUE);
		}
		break;
	default:
		gtk_widget_set_visible (self->button_update, FALSE);
		break;
	}

	/* copy button */
	gs_details_page_update_copy_button (self);

	/* launch button */
	switch (gs_app_get_state (self->app)) {
	case AS_APP_STATE_INSTALLED:
	case AS_APP_STATE_UPDATABLE:
	case AS_APP_STATE_UPDATABLE_LIVE:
		if (!gs_app_has_quirk (self->app, GS_APP_QUIRK_NOT_LAUNCHABLE) &&
		    !gs_app_has_quirk (self->app, GS_APP_QUIRK_PARENTAL_NOT_LAUNCHABLE)) {
			gtk_widget_set_visible (self->button_details_launch, TRUE);
		} else {
			gtk_widget_set_visible (self->button_details_launch, FALSE);
		}
		break;
	default:
		gtk_widget_set_visible (self->button_details_launch, FALSE);
		break;
	}

	gtk_button_set_label (GTK_BUTTON (self->button_details_launch),
			      /* TRANSLATORS: A label for a button to execute the selected application. */
			      _("_Launch"));

	/* don't show the launch and shortcut buttons if the app doesn't have a desktop ID */
	if (gs_app_get_id (self->app) == NULL) {
		gtk_widget_set_visible (self->button_details_launch, FALSE);
	}

	/* remove button */
	if (gs_app_has_quirk (self->app, GS_APP_QUIRK_COMPULSORY) ||
	    gs_app_get_kind (self->app) == AS_APP_KIND_FIRMWARE) {
		gtk_widget_set_visible (self->button_remove, FALSE);
	} else {
		switch (state) {
		case AS_APP_STATE_INSTALLED:
		case AS_APP_STATE_UPDATABLE:
		case AS_APP_STATE_UPDATABLE_LIVE:
			gtk_widget_set_visible (self->button_remove, TRUE);
			gtk_widget_set_sensitive (self->button_remove, TRUE);
			/* Mark the button as destructive only if Launch is not visible */
			if (gtk_widget_get_visible (self->button_details_launch))
				gtk_style_context_remove_class (gtk_widget_get_style_context (self->button_remove), "destructive-action");
			else
				gtk_style_context_add_class (gtk_widget_get_style_context (self->button_remove), "destructive-action");
			/* TRANSLATORS: button text in the header when an application can be erased */
			gtk_button_set_label (GTK_BUTTON (self->button_remove), _("_Uninstall"));
			break;
		case AS_APP_STATE_AVAILABLE_LOCAL:
		case AS_APP_STATE_AVAILABLE:
		case AS_APP_STATE_INSTALLING:
		case AS_APP_STATE_REMOVING:
		case AS_APP_STATE_UNAVAILABLE:
		case AS_APP_STATE_UNKNOWN:
		case AS_APP_STATE_QUEUED_FOR_INSTALL:
			gtk_widget_set_visible (self->button_remove, FALSE);
			break;
		default:
			g_warning ("App unexpectedly in state %s",
				   as_app_state_to_string (state));
			g_assert_not_reached ();
		}
	}

	if (app_has_pending_action (self->app)) {
		gtk_widget_set_visible (self->button_install, FALSE);
		gtk_widget_set_visible (self->button_update, FALSE);
		gtk_widget_set_visible (self->button_details_launch, FALSE);
		gtk_widget_set_visible (self->button_remove, FALSE);
		gtk_widget_set_visible (self->button_copy, FALSE);
	}
}

static struct {
	GsAppPermissions permission;
	const char *title;
	const char *subtitle;
} permission_display_data[] = {
  { GS_APP_PERMISSIONS_NETWORK, N_("Network"), N_("Can communicate over the network") },
  { GS_APP_PERMISSIONS_SYSTEM_BUS, N_("System Services"), N_("Can access D-Bus services on the system bus") },
  { GS_APP_PERMISSIONS_SESSION_BUS, N_("Session Services"), N_("Can access D-Bus services on the session bus") },
  { GS_APP_PERMISSIONS_DEVICES, N_("Devices"), N_("Can access system device files") },
  { GS_APP_PERMISSIONS_HOME_FULL, N_("Home folder"), N_("Can view, edit and create files") },
  { GS_APP_PERMISSIONS_HOME_READ, N_("Home folder"), N_("Can view files") },
  { GS_APP_PERMISSIONS_FILESYSTEM_FULL, N_("File system"), N_("Can view, edit and create files") },
  { GS_APP_PERMISSIONS_FILESYSTEM_READ, N_("File system"), N_("Can view files") },
  { GS_APP_PERMISSIONS_DOWNLOADS_FULL, N_("Downloads folder"), N_("Can view, edit and create files") },
  { GS_APP_PERMISSIONS_DOWNLOADS_READ, N_("Downloads folder"), N_("Can view files") },
  { GS_APP_PERMISSIONS_SETTINGS, N_("Settings"), N_("Can view and change any settings") },
  { GS_APP_PERMISSIONS_X11, N_("Legacy display system"), N_("Uses an old, insecure display system") },
  { GS_APP_PERMISSIONS_ESCAPE_SANDBOX, N_("Sandbox escape"), N_("Can escape the sandbox and circumvent any other restrictions") },
};

static void
populate_permission_details (GsDetailsPage *self, GsAppPermissions permissions)
{
	GList *children;

	children = gtk_container_get_children (GTK_CONTAINER (self->box_permissions_details));
	for (GList *l = children; l != NULL; l = l->next)
		gtk_widget_destroy (GTK_WIDGET (l->data));
	g_list_free (children);

	if (permissions == GS_APP_PERMISSIONS_NONE) {
		GtkWidget *label;
		label = gtk_label_new (_("This application is fully sandboxed."));
		gtk_label_set_xalign (GTK_LABEL (label), 0);
		gtk_label_set_max_width_chars (GTK_LABEL (label), 40);
		gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
		gtk_widget_show (label);
		gtk_container_add (GTK_CONTAINER (self->box_permissions_details), label);
	} else if (permissions == GS_APP_PERMISSIONS_UNKNOWN) {
		GtkWidget *label;
		label = gtk_label_new (_("Unable to determine which parts of the system "
		                         "this application accesses. This is typical for "
		                         "older applications."));
		gtk_label_set_xalign (GTK_LABEL (label), 0);
		gtk_label_set_max_width_chars (GTK_LABEL (label), 40);
		gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
		gtk_widget_show (label);
		gtk_container_add (GTK_CONTAINER (self->box_permissions_details), label);
	} else {
		for (gsize i = 0; i < G_N_ELEMENTS (permission_display_data); i++) {
			GtkWidget *row, *image, *box, *label;

			if ((permissions & permission_display_data[i].permission) == 0)
				continue;

			row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
			gtk_widget_show (row);

			image = gtk_image_new_from_icon_name ("dialog-warning-symbolic", GTK_ICON_SIZE_MENU);
			if ((permission_display_data[i].permission & ~MEDIUM_PERMISSIONS) == 0)
				gtk_widget_set_opacity (image, 0);

			gtk_widget_show (image);
			gtk_container_add (GTK_CONTAINER (row), image);

			box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
			gtk_widget_show (box);
			gtk_container_add (GTK_CONTAINER (row), box);

			label = gtk_label_new (_(permission_display_data[i].title));
			gtk_label_set_xalign (GTK_LABEL (label), 0);
			gtk_widget_show (label);
			gtk_container_add (GTK_CONTAINER (box), label);

			label = gtk_label_new (_(permission_display_data[i].subtitle));
			gtk_label_set_xalign (GTK_LABEL (label), 0);
			gtk_style_context_add_class (gtk_widget_get_style_context (label), "dim-label");
			gtk_widget_show (label);
			gtk_container_add (GTK_CONTAINER (box), label);

			gtk_container_add (GTK_CONTAINER (self->box_permissions_details), row);
		}
	}
}

static void
gs_details_page_refresh_all (GsDetailsPage *self)
{
	GsAppList *history;
	GdkPixbuf *pixbuf = NULL;
	GList *addons;
	GtkWidget *widget;
	const gchar *tmp;
	gboolean ret;
	gchar **menu_path;
	guint64 kudos;
	guint64 updated;
	guint64 user_integration_bf;
	gboolean show_support_box = FALSE;
	g_autofree gchar *origin = NULL;

	/* change widgets */
	tmp = gs_app_get_name (self->app);
	widget = GTK_WIDGET (gtk_builder_get_object (self->builder, "application_details_header"));
	if (tmp != NULL && tmp[0] != '\0') {
		gtk_label_set_label (GTK_LABEL (self->application_details_title), tmp);
		gtk_label_set_label (GTK_LABEL (widget), tmp);
		gtk_widget_set_visible (self->application_details_title, TRUE);
	} else {
		gtk_widget_set_visible (self->application_details_title, FALSE);
		gtk_label_set_label (GTK_LABEL (widget), "");
	}
	tmp = gs_app_get_summary (self->app);
	if (tmp != NULL && tmp[0] != '\0') {
		gtk_label_set_label (GTK_LABEL (self->application_details_summary), tmp);
		gtk_widget_set_visible (self->application_details_summary, TRUE);
	} else {
		gtk_widget_set_visible (self->application_details_summary, FALSE);
	}

	/* refresh buttons */
	gs_details_page_refresh_buttons (self);

	/* set the description */
	tmp = gs_app_get_description (self->app);
	gs_details_page_set_description (self, tmp);

	/* set the icon */
	pixbuf = gs_app_get_pixbuf (self->app);
	if (pixbuf != NULL) {
		gs_image_set_from_pixbuf (GTK_IMAGE (self->application_details_icon), pixbuf);
	} else {
		gtk_image_set_from_icon_name (GTK_IMAGE (self->application_details_icon),
		                              "application-x-executable",
		                              GTK_ICON_SIZE_DIALOG);
	}

	tmp = gs_app_get_url (self->app, AS_URL_KIND_HOMEPAGE);
	if (tmp != NULL && tmp[0] != '\0') {
		gtk_widget_set_visible (self->button_details_website, TRUE);
		show_support_box = TRUE;
	} else {
		gtk_widget_set_visible (self->button_details_website, FALSE);
	}
	tmp = gs_app_get_url (self->app, AS_URL_KIND_DONATION);
	if (tmp != NULL && tmp[0] != '\0') {
		gtk_widget_set_visible (self->button_donate, TRUE);
		show_support_box = TRUE;
	} else {
		gtk_widget_set_visible (self->button_donate, FALSE);
	}
	gtk_widget_set_visible (self->box_details_support, show_support_box);

	/* set the developer name, falling back to the project group */
	tmp = gs_app_get_developer_name (self->app);
	if (tmp == NULL)
		tmp = gs_app_get_project_group (self->app);
	if (tmp == NULL) {
		gtk_widget_set_visible (self->label_details_developer_title, FALSE);
		gtk_widget_set_visible (self->box_details_developer, FALSE);
	} else {
		gtk_widget_set_visible (self->label_details_developer_title, TRUE);
		gtk_label_set_label (GTK_LABEL (self->label_details_developer_value), tmp);
		gtk_widget_set_visible (self->box_details_developer, TRUE);
	}
	gtk_widget_set_visible (self->image_details_developer_verified, gs_app_has_quirk (self->app, GS_APP_QUIRK_DEVELOPER_VERIFIED));

	/* set the license buttons */
	tmp = gs_app_get_license (self->app);
	if (tmp == NULL) {
		gtk_widget_set_visible (self->button_details_license_free, FALSE);
		gtk_widget_set_visible (self->button_details_license_nonfree, FALSE);
		gtk_widget_set_visible (self->button_details_license_unknown, TRUE);
	} else if (gs_app_get_license_is_free (self->app)) {
		gtk_widget_set_visible (self->button_details_license_free, TRUE);
		gtk_widget_set_visible (self->button_details_license_nonfree, FALSE);
		gtk_widget_set_visible (self->button_details_license_unknown, FALSE);
	} else {
		gtk_widget_set_visible (self->button_details_license_free, FALSE);
		gtk_widget_set_visible (self->button_details_license_nonfree, TRUE);
		gtk_widget_set_visible (self->button_details_license_unknown, FALSE);
	}

	/* set channel for snaps */
	if (gs_app_get_bundle_kind (self->app) == AS_BUNDLE_KIND_SNAP) {
		gtk_label_set_label (GTK_LABEL (self->label_details_channel_value), gs_app_get_branch (self->app));
		gtk_widget_set_visible (self->label_details_channel_title, TRUE);
		gtk_widget_set_visible (self->label_details_channel_value, TRUE);
	} else {
		gtk_widget_set_visible (self->label_details_channel_title, FALSE);
		gtk_widget_set_visible (self->label_details_channel_value, FALSE);
	}

	/* set version */
	tmp = gs_app_get_version (self->app);
	if (tmp != NULL){
		gtk_label_set_label (GTK_LABEL (self->label_details_version_value), tmp);
	} else {
		/* TRANSLATORS: this is where the version is not known */
		gtk_label_set_label (GTK_LABEL (self->label_details_version_value), C_("version", "Unknown"));
	}

	/* refresh size information */
	gs_details_page_refresh_size (self);

	/* set the updated date */
	updated = gs_app_get_install_date (self->app);
	if (updated == GS_APP_INSTALL_DATE_UNSET) {
		gtk_widget_set_visible (self->label_details_updated_title, FALSE);
		gtk_widget_set_visible (self->label_details_updated_value, FALSE);
	} else if (updated == GS_APP_INSTALL_DATE_UNKNOWN) {
		/* TRANSLATORS: this is where the updated date is not known */
		gtk_label_set_label (GTK_LABEL (self->label_details_updated_value), C_("updated", "Never"));
		gtk_widget_set_visible (self->label_details_updated_title, TRUE);
		gtk_widget_set_visible (self->label_details_updated_value, TRUE);
	} else {
		g_autoptr(GDateTime) dt = NULL;
		g_autofree gchar *updated_str = NULL;
		dt = g_date_time_new_from_unix_utc ((gint64) updated);
		updated_str = g_date_time_format (dt, "%x");

		history = gs_app_get_history (self->app);

		if (gs_app_list_length (history) == 0) {
			gtk_label_set_label (GTK_LABEL (self->label_details_updated_value), updated_str);
		} else {
			GString *url;

			url = g_string_new (NULL);
			g_string_printf (url, "<a href=\"show-history\">%s</a>", updated_str);
			gtk_label_set_markup (GTK_LABEL (self->label_details_updated_value), url->str);
			g_string_free (url, TRUE);
		}
		gtk_widget_set_visible (self->label_details_updated_title, TRUE);
		gtk_widget_set_visible (self->label_details_updated_value, TRUE);
	}

	/* set the category */
	menu_path = gs_app_get_menu_path (self->app);
	if (menu_path == NULL || menu_path[0] == NULL || menu_path[0][0] == '\0') {
		gtk_widget_set_visible (self->label_details_category_title, FALSE);
		gtk_widget_set_visible (self->label_details_category_value, FALSE);
	} else {
		g_autofree gchar *path = NULL;
		if (gtk_widget_get_direction (self->label_details_category_value) == GTK_TEXT_DIR_RTL)
			path = g_strjoinv (" ← ", menu_path);
		else
			path = g_strjoinv (" → ", menu_path);
		gtk_label_set_label (GTK_LABEL (self->label_details_category_value), path);
		gtk_widget_set_visible (self->label_details_category_title, TRUE);
		gtk_widget_set_visible (self->label_details_category_value, TRUE);
	}

	/* set the origin */
	origin = g_strdup (gs_app_get_origin_hostname (self->app));
	if (origin == NULL)
		origin = g_strdup (gs_app_get_origin (self->app));
	if (origin == NULL) {
		GFile *local_file = gs_app_get_local_file (self->app);
		if (local_file != NULL)
			origin = g_file_get_basename (local_file);
	}
	if (origin == NULL || origin[0] == '\0') {
		/* TRANSLATORS: this is where we don't know the origin of the
		 * application */
		gtk_label_set_label (GTK_LABEL (self->label_details_origin_value), C_("origin", "Unknown"));
	} else {
		gtk_label_set_label (GTK_LABEL (self->label_details_origin_value), origin);
	}

	/* set MyLanguage kudo */
	kudos = gs_app_get_kudos (self->app);
	ret = (kudos & GS_APP_KUDO_MY_LANGUAGE) > 0;
	gtk_widget_set_sensitive (self->image_details_kudo_translated, ret);
	gs_details_page_set_sensitive (self->label_details_kudo_translated, ret);

	/* set RecentRelease kudo */
	ret = (kudos & GS_APP_KUDO_RECENT_RELEASE) > 0;
	gtk_widget_set_sensitive (self->image_details_kudo_updated, ret);
	gs_details_page_set_sensitive (self->label_details_kudo_updated, ret);

	/* set UserDocs kudo */
	ret = (kudos & GS_APP_KUDO_INSTALLS_USER_DOCS) > 0;
	gtk_widget_set_sensitive (self->image_details_kudo_docs, ret);
	gs_details_page_set_sensitive (self->label_details_kudo_docs, ret);

	/* set sandboxed kudo */
	ret = (kudos & GS_APP_KUDO_SANDBOXED) > 0;
	gtk_widget_set_sensitive (self->image_details_kudo_sandboxed, ret);
	gs_details_page_set_sensitive (self->label_details_kudo_sandboxed, ret);

	/* any of the various integration kudos */
	user_integration_bf = GS_APP_KUDO_SEARCH_PROVIDER |
			      GS_APP_KUDO_USES_NOTIFICATIONS |
			      GS_APP_KUDO_HIGH_CONTRAST;
	ret = (kudos & user_integration_bf) > 0;
	gtk_widget_set_sensitive (self->image_details_kudo_integration, ret);
	gs_details_page_set_sensitive (self->label_details_kudo_integration, ret);

	/* hide the kudo details for non-desktop software */
	switch (gs_app_get_kind (self->app)) {
	case AS_APP_KIND_DESKTOP:
		gtk_widget_set_visible (self->grid_details_kudo, TRUE);
		break;
	default:
		gtk_widget_set_visible (self->grid_details_kudo, FALSE);
		break;
	}

	/* only show permissions for flatpak apps */
	if (gs_app_get_bundle_kind (self->app) == AS_BUNDLE_KIND_FLATPAK &&
	    gs_app_get_kind (self->app) == AS_APP_KIND_DESKTOP) {
		GsAppPermissions permissions = gs_app_get_permissions (self->app);

		populate_permission_details (self, permissions);

		if (gs_app_get_permissions (self->app) != GS_APP_PERMISSIONS_UNKNOWN) {
			if ((permissions & ~LIMITED_PERMISSIONS) == 0)
				gtk_button_set_label (GTK_BUTTON (self->button_details_permissions_value), _("Low"));
			else if ((permissions & ~MEDIUM_PERMISSIONS) == 0)
				gtk_button_set_label (GTK_BUTTON (self->button_details_permissions_value), _("Medium"));
			else
				gtk_button_set_label (GTK_BUTTON (self->button_details_permissions_value), _("High"));
		} else {
			gtk_button_set_label (GTK_BUTTON (self->button_details_permissions_value), _("Unknown"));
		}

		gtk_widget_set_visible (self->label_details_permissions_title, TRUE);
		gtk_widget_set_visible (self->button_details_permissions_value, TRUE);
	} else {
		gtk_widget_set_visible (self->label_details_permissions_title, FALSE);
		gtk_widget_set_visible (self->button_details_permissions_value, FALSE);
	}

	/* are we trying to replace something in the baseos */
	gtk_widget_set_visible (self->infobar_details_package_baseos,
				gs_app_has_quirk (self->app, GS_APP_QUIRK_COMPULSORY) &&
				gs_app_get_state (self->app) == AS_APP_STATE_AVAILABLE_LOCAL);

	switch (gs_app_get_kind (self->app)) {
	case AS_APP_KIND_DESKTOP:
		/* installing an app with a repo file */
		gtk_widget_set_visible (self->infobar_details_app_repo,
					gs_app_has_quirk (self->app,
							  GS_APP_QUIRK_HAS_SOURCE) &&
					gs_app_get_state (self->app) == AS_APP_STATE_AVAILABLE_LOCAL);
		gtk_widget_set_visible (self->infobar_details_repo, FALSE);
		break;
	case AS_APP_KIND_GENERIC:
		/* installing a repo-release package */
		gtk_widget_set_visible (self->infobar_details_app_repo, FALSE);
		gtk_widget_set_visible (self->infobar_details_repo,
					gs_app_has_quirk (self->app,
							  GS_APP_QUIRK_HAS_SOURCE) &&
					gs_app_get_state (self->app) == AS_APP_STATE_AVAILABLE_LOCAL);
		break;
	default:
		gtk_widget_set_visible (self->infobar_details_app_repo, FALSE);
		gtk_widget_set_visible (self->infobar_details_repo, FALSE);
		break;
	}

	/* installing a app without a repo file */
	switch (gs_app_get_kind (self->app)) {
	case AS_APP_KIND_DESKTOP:
		if (gs_app_get_kind (self->app) == AS_APP_KIND_FIRMWARE) {
			gtk_widget_set_visible (self->infobar_details_app_norepo, FALSE);
		} else {
			gtk_widget_set_visible (self->infobar_details_app_norepo,
						!gs_app_has_quirk (self->app,
							  GS_APP_QUIRK_HAS_SOURCE) &&
						gs_app_get_state (self->app) == AS_APP_STATE_AVAILABLE_LOCAL);
		}
		break;
	default:
		gtk_widget_set_visible (self->infobar_details_app_norepo, FALSE);
		break;
	}

	/* only show the "select addons" string if the app isn't yet installed */
	switch (gs_app_get_state (self->app)) {
	case AS_APP_STATE_INSTALLED:
	case AS_APP_STATE_UPDATABLE:
	case AS_APP_STATE_UPDATABLE_LIVE:
		gtk_widget_set_visible (self->label_addons_uninstalled_app, FALSE);
		break;
	default:
		gtk_widget_set_visible (self->label_addons_uninstalled_app, TRUE);
		break;
	}

	/* hide fields that don't make sense for sources */
	switch (gs_app_get_kind (self->app)) {
	case AS_APP_KIND_SOURCE:
		gtk_widget_set_visible (self->label_details_license_title, FALSE);
		gtk_widget_set_visible (self->box_details_license_value, FALSE);
		gtk_widget_set_visible (self->label_details_version_title, FALSE);
		gtk_widget_set_visible (self->label_details_version_value, FALSE);
		break;
	default:
		gtk_widget_set_visible (self->label_details_license_title, TRUE);
		gtk_widget_set_visible (self->box_details_license_value, TRUE);
		gtk_widget_set_visible (self->label_details_version_title, TRUE);
		gtk_widget_set_visible (self->label_details_version_value, TRUE);
		break;
	}

	gs_details_page_update_shortcut_button (self);

	gs_details_page_update_copy_button (self);

	/* update progress */
	gs_details_page_refresh_progress (self);

	addons = gtk_container_get_children (GTK_CONTAINER (self->list_box_addons));
	gtk_widget_set_visible (self->box_addons, addons != NULL);
	g_list_free (addons);
}

static void
list_header_func (GtkListBoxRow *row,
		  GtkListBoxRow *before,
		  gpointer user_data)
{
	GtkWidget *header = NULL;
	if (before != NULL)
		header = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
	gtk_list_box_row_set_header (row, header);
}

static gint
list_sort_func (GtkListBoxRow *a,
		GtkListBoxRow *b,
		gpointer user_data)
{
	GsApp *a1 = gs_app_addon_row_get_addon (GS_APP_ADDON_ROW (a));
	GsApp *a2 = gs_app_addon_row_get_addon (GS_APP_ADDON_ROW (b));

	return gs_utils_sort_strcmp (gs_app_get_name (a1),
				     gs_app_get_name (a2));
}

static void gs_details_page_addon_selected_cb (GsAppAddonRow *row, GParamSpec *pspec, GsDetailsPage *self);

static void
gs_details_page_refresh_addons (GsDetailsPage *self)
{
	GsAppList *addons;
	guint i;

	gs_container_remove_all (GTK_CONTAINER (self->list_box_addons));

	addons = gs_app_get_addons (self->app);
	for (i = 0; i < gs_app_list_length (addons); i++) {
		GsApp *addon;
		GtkWidget *row;

		addon = gs_app_list_index (addons, i);
		if (gs_app_get_state (addon) == AS_APP_STATE_UNKNOWN ||
		    gs_app_get_state (addon) == AS_APP_STATE_UNAVAILABLE)
			continue;

		row = gs_app_addon_row_new (addon);

		gtk_container_add (GTK_CONTAINER (self->list_box_addons), row);
		gtk_widget_show (row);

		g_signal_connect (row, "notify::selected",
				  G_CALLBACK (gs_details_page_addon_selected_cb),
				  self);
	}
}

static void gs_details_page_refresh_reviews (GsDetailsPage *self);

typedef struct {
	GsDetailsPage		*self;
	AsReview		*review;
	GsApp			*app;
	GsPluginAction		 action;
} GsDetailsPageReviewHelper;

static void
gs_details_page_review_helper_free (GsDetailsPageReviewHelper *helper)
{
	g_object_unref (helper->self);
	g_object_unref (helper->review);
	g_object_unref (helper->app);
	g_free (helper);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GsDetailsPageReviewHelper, gs_details_page_review_helper_free);

static void
gs_details_page_app_set_review_cb (GObject *source,
                                   GAsyncResult *res,
                                   gpointer user_data)
{
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (source);
	g_autoptr(GsDetailsPageReviewHelper) helper = (GsDetailsPageReviewHelper *) user_data;
	g_autoptr(GError) error = NULL;

	if (!gs_plugin_loader_job_action_finish (plugin_loader, res, &error)) {
		g_warning ("failed to set review on %s: %s",
			   gs_app_get_id (helper->app), error->message);
		return;
	}
	gs_details_page_refresh_reviews (helper->self);
}

static void
gs_details_page_review_button_clicked_cb (GsReviewRow *row,
                                          GsPluginAction action,
                                          GsDetailsPage *self)
{
	GsDetailsPageReviewHelper *helper = g_new0 (GsDetailsPageReviewHelper, 1);
	g_autoptr(GsPluginJob) plugin_job = NULL;

	helper->self = g_object_ref (self);
	helper->app = g_object_ref (self->app);
	helper->review = g_object_ref (gs_review_row_get_review (row));
	helper->action = action;
	plugin_job = gs_plugin_job_newv (helper->action,
					 "interactive", TRUE,
					 "app", helper->app,
					 "review", helper->review,
					 NULL);
	gs_plugin_loader_job_process_async (self->plugin_loader, plugin_job,
					    self->cancellable,
					    gs_details_page_app_set_review_cb,
					    helper);
}

static void
gs_details_page_refresh_reviews (GsDetailsPage *self)
{
	GArray *review_ratings = NULL;
	GPtrArray *reviews;
	gboolean show_review_button = TRUE;
	gboolean show_reviews = FALSE;
	guint n_reviews = 0;
	guint64 possible_actions = 0;
	guint i;
	struct {
		GsPluginAction action;
		const gchar *plugin_func;
	} plugin_vfuncs[] = {
		{ GS_PLUGIN_ACTION_REVIEW_UPVOTE,	"gs_plugin_review_upvote" },
		{ GS_PLUGIN_ACTION_REVIEW_DOWNVOTE,	"gs_plugin_review_downvote" },
		{ GS_PLUGIN_ACTION_REVIEW_REPORT,	"gs_plugin_review_report" },
		{ GS_PLUGIN_ACTION_REVIEW_SUBMIT,	"gs_plugin_review_submit" },
		{ GS_PLUGIN_ACTION_REVIEW_REMOVE,	"gs_plugin_review_remove" },
		{ GS_PLUGIN_ACTION_LAST,	NULL }
	};

	/* nothing to show */
	if (self->app == NULL)
		return;

	/* show or hide the entire reviews section */
	switch (gs_app_get_kind (self->app)) {
	case AS_APP_KIND_DESKTOP:
	case AS_APP_KIND_FONT:
	case AS_APP_KIND_INPUT_METHOD:
	case AS_APP_KIND_WEB_APP:
	case AS_APP_KIND_SHELL_EXTENSION:
		/* don't show a missing rating on a local file */
		if (gs_app_get_state (self->app) != AS_APP_STATE_AVAILABLE_LOCAL &&
		    self->enable_reviews)
			show_reviews = TRUE;
		break;
	default:
		break;
	}

	/* some apps are unreviewable */
	if (gs_app_has_quirk (self->app, GS_APP_QUIRK_NOT_REVIEWABLE))
		show_reviews = FALSE;

	/* set the star rating */
	if (show_reviews) {
		gtk_widget_set_sensitive (self->star, gs_app_get_rating (self->app) >= 0);
		gs_star_widget_set_rating (GS_STAR_WIDGET (self->star),
					   gs_app_get_rating (self->app));

		review_ratings = gs_app_get_review_ratings (self->app);
		if (review_ratings != NULL) {
			gs_review_histogram_set_ratings (GS_REVIEW_HISTOGRAM (self->histogram),
						         review_ratings);
		}
		if (review_ratings != NULL) {
			for (i = 0; i < review_ratings->len; i++)
				n_reviews += (guint) g_array_index (review_ratings, guint32, i);
		} else if (gs_app_get_reviews (self->app) != NULL) {
			n_reviews = gs_app_get_reviews (self->app)->len;
		}
	}

	/* enable appropriate widgets */
	gtk_widget_set_visible (self->star, show_reviews);
	gtk_widget_set_visible (self->box_reviews, show_reviews);
	gtk_widget_set_visible (self->histogram, review_ratings != NULL && review_ratings->len > 0);
	gtk_widget_set_visible (self->label_review_count, n_reviews > 0);

	/* update the review label next to the star widget */
	if (n_reviews > 0) {
		g_autofree gchar *text = NULL;
		gtk_widget_set_visible (self->label_review_count, TRUE);
		text = g_strdup_printf ("(%u)", n_reviews);
		gtk_label_set_text (GTK_LABEL (self->label_review_count), text);
	}

	/* no point continuing */
	if (!show_reviews)
		return;

	/* find what the plugins support */
	for (i = 0; plugin_vfuncs[i].action != GS_PLUGIN_ACTION_LAST; i++) {
		if (gs_plugin_loader_get_plugin_supported (self->plugin_loader,
							   plugin_vfuncs[i].plugin_func)) {
			possible_actions |= 1u << plugin_vfuncs[i].action;
		}
	}

	/* add all the reviews */
	gs_container_remove_all (GTK_CONTAINER (self->list_box_reviews));
	reviews = gs_app_get_reviews (self->app);
	for (i = 0; i < reviews->len; i++) {
		AsReview *review = g_ptr_array_index (reviews, i);
		GtkWidget *row = gs_review_row_new (review);
		guint64 actions;

		g_signal_connect (row, "button-clicked",
				  G_CALLBACK (gs_details_page_review_button_clicked_cb), self);
		if (as_review_get_flags (review) & AS_REVIEW_FLAG_SELF) {
			actions = possible_actions & 1 << GS_PLUGIN_ACTION_REVIEW_REMOVE;
			show_review_button = FALSE;
		} else {
			actions = possible_actions & ~(1u << GS_PLUGIN_ACTION_REVIEW_REMOVE);
		}
		gs_review_row_set_actions (GS_REVIEW_ROW (row), actions);
		gtk_container_add (GTK_CONTAINER (self->list_box_reviews), row);
		gtk_widget_set_visible (row, self->show_all_reviews ||
					     i < SHOW_NR_REVIEWS_INITIAL);
		gs_review_row_set_network_available (GS_REVIEW_ROW (row),
						     gs_plugin_loader_get_network_available (self->plugin_loader));
	}

	/* only show the button if there are more to show */
	gtk_widget_set_visible (self->button_more_reviews,
				!self->show_all_reviews &&
				reviews->len > SHOW_NR_REVIEWS_INITIAL);

	/* show the button only if the user never reviewed */
	gtk_widget_set_visible (self->button_review, show_review_button);
	if (gs_plugin_loader_get_network_available (self->plugin_loader)) {
		gtk_widget_set_sensitive (self->button_review, TRUE);
		gtk_widget_set_sensitive (self->star_eventbox, TRUE);
		gtk_widget_set_tooltip_text (self->button_review, NULL);
	} else {
		gtk_widget_set_sensitive (self->button_review, FALSE);
		gtk_widget_set_sensitive (self->star_eventbox, FALSE);
		gtk_widget_set_tooltip_text (self->button_review,
					     /* TRANSLATORS: we need a remote server to process */
					     _("You need internet access to write a review"));
	}
}

static void
gs_details_page_app_refine_cb (GObject *source,
				GAsyncResult *res,
				gpointer user_data)
{
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (source);
	GsDetailsPage *self = GS_DETAILS_PAGE (user_data);
	g_autoptr(GError) error = NULL;
	if (!gs_plugin_loader_job_action_finish (plugin_loader, res, &error)) {
		g_warning ("failed to refine %s: %s",
			   gs_app_get_id (self->app),
			   error->message);
		return;
	}
	gs_details_page_refresh_size (self);
	gs_details_page_refresh_reviews (self);
}

static void
gs_details_page_content_rating_set_css (GsDetailsPage *page, guint age)
{
	g_autoptr(GString) css = g_string_new (NULL);
	const gchar *color_bg = NULL;
	const gchar *color_fg = "#ffffff";
	if (age >= 18) {
		color_bg = "#ee2222";
	} else if (age >= 15) {
		color_bg = "#f1c000";
	} else if (age >= 12) {
		color_bg = "#2a97c9";
	} else if (age >= 5) {
		color_bg = "#3f756c";
	} else {
		color_bg = "#009d66";
	}
	g_string_append_printf (css, "color: %s;\n", color_fg);
	g_string_append_printf (css, "background-color: %s;\n", color_bg);

	gs_utils_widget_set_css (page->button_details_rating_value,
				 (GtkCssProvider **) &page->button_details_rating_style_provider,
				 "content-rating-custom", css->str);
}

static void
gs_details_page_refresh_content_rating (GsDetailsPage *self)
{
	AsContentRating *content_rating;
	GsContentRatingSystem system;
	guint age = 0;
	g_autofree gchar *display = NULL;
	const gchar *locale;

	/* get the content rating system from the locale */
	locale = setlocale (LC_MESSAGES, NULL);
	system = gs_utils_content_rating_system_from_locale (locale);
	g_debug ("content rating system is guessed as %s from %s",
		 gs_content_rating_system_to_str (system),
		 locale);

	/* only show the button if a game and has a content rating */
	content_rating = gs_app_get_content_rating (self->app);
	if (content_rating != NULL) {
		age = as_content_rating_get_minimum_age (content_rating);
		display = gs_utils_content_rating_age_to_str (system, age);
	}
	if (display != NULL) {
		gtk_button_set_label (GTK_BUTTON (self->button_details_rating_value), display);
		gtk_widget_set_visible (self->button_details_rating_value, TRUE);
		gtk_widget_set_visible (self->label_details_rating_title, TRUE);
		gs_details_page_content_rating_set_css (self, age);
	} else {
		gtk_widget_set_visible (self->button_details_rating_value, FALSE);
		gtk_widget_set_visible (self->label_details_rating_title, FALSE);
	}
}

static void
_set_app (GsDetailsPage *self, GsApp *app)
{
	g_autoptr(GsPluginJob) plugin_job = NULL;
	GFile *copy_dest;

	/* do not show all the reviews by default */
	self->show_all_reviews = FALSE;

	/* disconnect the old handlers */
	if (self->app != NULL) {
		g_signal_handlers_disconnect_by_func (self->app, gs_details_page_notify_state_changed_cb, self);
		g_signal_handlers_disconnect_by_func (self->app, gs_details_page_progress_changed_cb, self);
		g_signal_handlers_disconnect_by_func (self->app, gs_details_page_allow_cancel_changed_cb,
						      self);
	}

	/* save app */
	g_set_object (&self->app, app);
	if (self->app == NULL) {
		/* switch away from the details view that failed to load */
		gs_shell_set_mode (self->shell, GS_SHELL_MODE_OVERVIEW);
		return;
	}
	g_set_object (&self->app_cancellable, gs_app_get_cancellable (app));
	g_signal_connect_object (self->app, "notify::state",
				 G_CALLBACK (gs_details_page_notify_state_changed_cb),
				 self, 0);
	g_signal_connect_object (self->app, "notify::size",
				 G_CALLBACK (gs_details_page_notify_state_changed_cb),
				 self, 0);
	g_signal_connect_object (self->app, "notify::license",
				 G_CALLBACK (gs_details_page_notify_state_changed_cb),
				 self, 0);
	g_signal_connect_object (self->app, "notify::quirk",
				 G_CALLBACK (gs_details_page_notify_state_changed_cb),
				 self, 0);
	g_signal_connect_object (self->app, "notify::progress",
				 G_CALLBACK (gs_details_page_progress_changed_cb),
				 self, 0);
	g_signal_connect_object (self->app, "notify::allow-cancel",
				 G_CALLBACK (gs_details_page_allow_cancel_changed_cb),
				 self, 0);
	g_signal_connect_object (self->app, "notify::pending-action",
				 G_CALLBACK (gs_details_page_notify_state_changed_cb),
				 self, 0);

	/* also check (asynchronously) whether the app will
	 * certainly fail a copy so we can update the UI
	 * accordingly. Cases where the plugin would know a copy
	 * would fail include a Flatpak which lacks a collection
	 * ID or broader issues like the user lacking write
	 * access to the destination. */
	copy_dest = get_removable_destination (self);
	plugin_job = gs_plugin_job_newv (
		GS_PLUGIN_ACTION_GET_COPYABLE,
		"app", self->app,
		"copy-dest", copy_dest,
		 NULL);
	gs_plugin_loader_job_app_get_copyable_async (
		self->plugin_loader,
		plugin_job,
		self->cancellable,
		gs_details_page_app_get_copyable_cb,
		self);
}

/* show the UI and do operations that should not block page load */
static void
gs_details_page_load_stage2 (GsDetailsPage *self)
{
	g_autofree gchar *tmp = NULL;
	g_autoptr(GsPluginJob) plugin_job1 = NULL;
	g_autoptr(GsPluginJob) plugin_job2 = NULL;

	/* print what we've got */
	tmp = gs_app_to_string (self->app);
	g_debug ("%s", tmp);

	/* update UI */
	gs_details_page_set_state (self, GS_DETAILS_PAGE_STATE_READY);
	gs_details_page_refresh_screenshots (self);
	gs_details_page_refresh_addons (self);
	gs_details_page_refresh_reviews (self);
	gs_details_page_refresh_all (self);
	gs_details_page_refresh_content_rating (self);

	/* if these tasks fail (e.g. because we have no networking) then it's
	 * of no huge importance if we don't get the required data */
	plugin_job1 = gs_plugin_job_newv (GS_PLUGIN_ACTION_REFINE,
					  "app", self->app,
					  "refine-flags", GS_PLUGIN_REFINE_FLAGS_REQUIRE_RATING |
							  GS_PLUGIN_REFINE_FLAGS_REQUIRE_REVIEW_RATINGS |
							  GS_PLUGIN_REFINE_FLAGS_REQUIRE_REVIEWS |
							  GS_PLUGIN_REFINE_FLAGS_REQUIRE_SIZE,
					  NULL);
	plugin_job2 = gs_plugin_job_newv (GS_PLUGIN_ACTION_GET_ALTERNATES,
					  "interactive", TRUE,
					  "app", self->app,
					  "refine-flags", GS_PLUGIN_REFINE_FLAGS_REQUIRE_ORIGIN_HOSTNAME |
							  GS_PLUGIN_REFINE_FLAGS_REQUIRE_PROVENANCE,
					  "dedupe-flags", GS_APP_LIST_FILTER_FLAG_NONE,
					  NULL);
	gs_plugin_loader_job_process_async (self->plugin_loader, plugin_job1,
					    self->cancellable,
					    gs_details_page_app_refine_cb,
					    self);
	gs_plugin_loader_job_process_async (self->plugin_loader, plugin_job2,
					    self->cancellable,
					    gs_details_page_get_alternates_cb,
					    self);
}

static void
gs_details_page_load_stage1_cb (GObject *source,
				GAsyncResult *res,
				gpointer user_data)
{
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (source);
	GsDetailsPage *self = GS_DETAILS_PAGE (user_data);
	g_autoptr(GError) error = NULL;

	if (!gs_plugin_loader_job_action_finish (plugin_loader, res, &error)) {
		g_warning ("failed to refine %s: %s",
			   gs_app_get_id (self->app),
			   error->message);
	}
	if (gs_app_get_kind (self->app) == AS_APP_KIND_UNKNOWN ||
	    gs_app_get_state (self->app) == AS_APP_STATE_UNKNOWN) {
		g_autofree gchar *str = NULL;
		const gchar *id = gs_app_get_id (self->app);
		str = g_strdup_printf (_("Unable to find “%s”"), id == NULL ? gs_app_get_source_default (self->app) : id);
		gtk_label_set_text (GTK_LABEL (self->label_failed), str);
		gs_details_page_set_state (self, GS_DETAILS_PAGE_STATE_FAILED);
		return;
	}

	/* Hide the app if it’s not suitable for the user, but only if it’s not
	 * already installed — a parent could have decided that a particular
	 * app *is* actually suitable for their child, despite its age rating.
	 *
	 * Make it look like the app doesn’t exist, to not tantalise the
	 * child. */
	if (!gs_app_is_installed (self->app) &&
	    gs_app_has_quirk (self->app, GS_APP_QUIRK_PARENTAL_FILTER)) {
		g_autofree gchar *str = NULL;
		const gchar *id = gs_app_get_id (self->app);
		str = g_strdup_printf (_("Unable to find “%s”"), id == NULL ? gs_app_get_source_default (self->app) : id);
		gtk_label_set_text (GTK_LABEL (self->label_failed), str);
		gs_details_page_set_state (self, GS_DETAILS_PAGE_STATE_FAILED);
		return;
	}

	/* do 2nd stage refine */
	gs_details_page_load_stage2 (self);
}

static void
gs_details_page_file_to_app_cb (GObject *source,
                                GAsyncResult *res,
                                gpointer user_data)
{
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (source);
	GsDetailsPage *self = GS_DETAILS_PAGE (user_data);
	g_autoptr(GsAppList) list = NULL;
	g_autoptr(GError) error = NULL;

	list = gs_plugin_loader_job_process_finish (plugin_loader,
						    res,
						    &error);
	if (list == NULL) {
		g_warning ("failed to convert file to GsApp: %s", error->message);
		/* go back to the overview */
		gs_shell_set_mode (self->shell, GS_SHELL_MODE_OVERVIEW);
	} else {
		GsApp *app = gs_app_list_index (list, 0);
		g_set_object (&self->app_local_file, app);
		_set_app (self, app);
		gs_details_page_load_stage2 (self);
	}
}

static void
gs_details_page_url_to_app_cb (GObject *source,
                               GAsyncResult *res,
                               gpointer user_data)
{
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (source);
	GsDetailsPage *self = GS_DETAILS_PAGE (user_data);
	g_autoptr(GsAppList) list = NULL;
	g_autoptr(GError) error = NULL;

	list = gs_plugin_loader_job_process_finish (plugin_loader,
						    res,
						    &error);
	if (list == NULL) {
		g_warning ("failed to convert URL to GsApp: %s", error->message);
		/* go back to the overview */
		gs_shell_set_mode (self->shell, GS_SHELL_MODE_OVERVIEW);
	} else {
		GsApp *app = gs_app_list_index (list, 0);
		_set_app (self, app);
		gs_details_page_load_stage2 (self);
	}
}

void
gs_details_page_set_local_file (GsDetailsPage *self, GFile *file)
{
	g_autoptr(GsPluginJob) plugin_job = NULL;
	gs_details_page_set_state (self, GS_DETAILS_PAGE_STATE_LOADING);
	g_clear_object (&self->app_local_file);
	plugin_job = gs_plugin_job_newv (GS_PLUGIN_ACTION_FILE_TO_APP,
					 "file", file,
					 "refine-flags", GS_PLUGIN_REFINE_FLAGS_REQUIRE_ICON |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_LICENSE |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_SIZE |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_VERSION |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_HISTORY |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_ORIGIN_HOSTNAME |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_MENU_PATH |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_URL |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_SETUP_ACTION |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_PROVENANCE |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_RELATED |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_RUNTIME |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_PERMISSIONS |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_PROJECT_GROUP |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_DEVELOPER_NAME |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_KUDOS |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_CONTENT_RATING |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_SCREENSHOTS |
							 GS_PLUGIN_REFINE_FLAGS_INTERACTIVE,
					 NULL);
	gs_plugin_loader_job_process_async (self->plugin_loader, plugin_job,
					    self->cancellable,
					    gs_details_page_file_to_app_cb,
					    self);
}

void
gs_details_page_set_url (GsDetailsPage *self, const gchar *url)
{
	g_autoptr(GsPluginJob) plugin_job = NULL;
	gs_details_page_set_state (self, GS_DETAILS_PAGE_STATE_LOADING);
	g_clear_object (&self->app_local_file);
	plugin_job = gs_plugin_job_newv (GS_PLUGIN_ACTION_URL_TO_APP,
					 "search", url,
					 "refine-flags", GS_PLUGIN_REFINE_FLAGS_REQUIRE_ICON |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_LICENSE |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_SIZE |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_VERSION |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_HISTORY |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_ORIGIN_HOSTNAME |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_MENU_PATH |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_URL |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_SETUP_ACTION |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_PROVENANCE |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_RELATED |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_RUNTIME |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_PERMISSIONS |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_PROJECT_GROUP |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_DEVELOPER_NAME |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_KUDOS |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_SCREENSHOTS |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_CONTENT_RATING |
							 GS_PLUGIN_REFINE_FLAGS_ALLOW_PACKAGES |
							 GS_PLUGIN_REFINE_FLAGS_INTERACTIVE,
					 NULL);
	gs_plugin_loader_job_process_async (self->plugin_loader, plugin_job,
					    self->cancellable,
					    gs_details_page_url_to_app_cb,
					    self);
}

/* refines a GsApp */
static void
gs_details_page_load_stage1 (GsDetailsPage *self)
{
	g_autoptr(GsPluginJob) plugin_job = NULL;

	/* update UI */
	gs_page_switch_to (GS_PAGE (self), TRUE);
	gs_details_page_set_state (self, GS_DETAILS_PAGE_STATE_LOADING);

	/* get extra details about the app */
	plugin_job = gs_plugin_job_newv (GS_PLUGIN_ACTION_REFINE,
					 "app", self->app,
					 "refine-flags", GS_PLUGIN_REFINE_FLAGS_REQUIRE_ICON |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_PERMISSIONS |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_LICENSE |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_VERSION |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_HISTORY |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_SETUP_ACTION |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_ORIGIN_HOSTNAME |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_MENU_PATH |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_URL |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_DESCRIPTION |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_SETUP_ACTION |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_PROVENANCE |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_RUNTIME |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_ADDONS |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_PROJECT_GROUP |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_DEVELOPER_NAME |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_KUDOS |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_CONTENT_RATING |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_SCREENSHOTS |
							 GS_PLUGIN_REFINE_FLAGS_INTERACTIVE,
					 NULL);
	gs_plugin_loader_job_process_async (self->plugin_loader, plugin_job,
					    self->cancellable,
					    gs_details_page_load_stage1_cb,
					    self);

	/* update UI with loading page */
	gs_details_page_refresh_all (self);
}

static void
gs_details_page_reload (GsPage *page)
{
	GsDetailsPage *self = GS_DETAILS_PAGE (page);
	if (self->app != NULL)
		gs_details_page_load_stage1 (self);
}

static gint
origin_popover_list_sort_func (GtkListBoxRow *a,
                               GtkListBoxRow *b,
                               gpointer user_data)
{
	GsApp *a1 = gs_origin_popover_row_get_app (GS_ORIGIN_POPOVER_ROW (a));
	GsApp *a2 = gs_origin_popover_row_get_app (GS_ORIGIN_POPOVER_ROW (b));
	g_autofree gchar *a1_origin = gs_app_get_origin_ui (a1);
	g_autofree gchar *a2_origin = gs_app_get_origin_ui (a2);

	return gs_utils_sort_strcmp (a1_origin, a2_origin);
}

static void
origin_popover_row_activated_cb (GtkListBox *list_box,
                                 GtkListBoxRow *row,
                                 gpointer user_data)
{
	GsDetailsPage *self = GS_DETAILS_PAGE (user_data);
	GsApp *app;
	GtkWidget *popover;

	popover = GTK_WIDGET (gtk_builder_get_object (self->builder, "origin_popover"));
	gtk_popover_popdown (GTK_POPOVER (popover));

	app = gs_origin_popover_row_get_app (GS_ORIGIN_POPOVER_ROW (row));
	if (app != self->app) {
		_set_app (self, app);
		gs_details_page_load_stage1 (self);
	}
}

static void
settings_changed_cb (GsDetailsPage *self, const gchar *key, gpointer data)
{
	if (self->app == NULL)
		return;
	if (g_strcmp0 (key, "show-nonfree-ui") == 0) {
		gs_details_page_refresh_all (self);
	}
}

/* this is being called from GsShell */
void
gs_details_page_set_app (GsDetailsPage *self, GsApp *app)
{
	g_return_if_fail (GS_IS_DETAILS_PAGE (self));
	g_return_if_fail (GS_IS_APP (app));

	/* clear old state */
	g_clear_object (&self->app_local_file);

	/* save GsApp */
	_set_app (self, app);
	gs_details_page_load_stage1 (self);
}

GsApp *
gs_details_page_get_app (GsDetailsPage *self)
{
	return self->app;
}

static void
gs_details_page_remove_app (GsDetailsPage *self)
{
	g_set_object (&self->app_cancellable, gs_app_get_cancellable (self->app));
	gs_page_remove_app (GS_PAGE (self), self->app, self->app_cancellable);
}

static void
gs_details_page_app_remove_button_cb (GtkWidget *widget, GsDetailsPage *self)
{
	gs_details_page_remove_app (self);
}

static void
gs_details_page_app_cancel_button_cb (GtkWidget *widget, GsDetailsPage *self)
{
	g_cancellable_cancel (self->app_cancellable);
	gtk_widget_set_sensitive (widget, FALSE);

	/* reset the pending-action from the app if needed */
	gs_app_set_pending_action (self->app, GS_PLUGIN_ACTION_UNKNOWN);

	/* FIXME: We should be able to revert the QUEUED_FOR_INSTALL without
	 * having to pretend to remove the app */
	if (gs_app_get_state (self->app) == AS_APP_STATE_QUEUED_FOR_INSTALL)
		gs_details_page_remove_app (self);
}

static void
gs_details_page_app_copy_button_cb (GtkWidget *widget, GsDetailsPage *self)
{
	GFile *copy_dest = get_removable_destination (self);

	g_set_object (&self->app_cancellable,
		      gs_app_get_cancellable (self->app));
	gs_page_copy_app (GS_PAGE (self), self->app, copy_dest,
			  GS_SHELL_INTERACTION_FULL, self->app_cancellable);
}

static void
gs_details_page_app_install_button_cb (GtkWidget *widget, GsDetailsPage *self)
{
	g_autoptr(GList) addons = NULL;

	/* Mark ticked addons to be installed together with the app */
	addons = gtk_container_get_children (GTK_CONTAINER (self->list_box_addons));
	for (GList *l = addons; l; l = l->next) {
		if (gs_app_addon_row_get_selected (l->data)) {
			GsApp *addon = gs_app_addon_row_get_addon (l->data);

			if (gs_app_get_state (addon) == AS_APP_STATE_AVAILABLE)
				gs_app_set_to_be_installed (addon, TRUE);
		}
	}

	g_set_object (&self->app_cancellable, gs_app_get_cancellable (self->app));

	if (gs_app_get_state (self->app) == AS_APP_STATE_UPDATABLE_LIVE) {
		gs_page_update_app (GS_PAGE (self), self->app, self->app_cancellable);
		return;
	}

	gs_page_install_app (GS_PAGE (self), self->app, GS_SHELL_INTERACTION_FULL,
			     self->app_cancellable);
}

static void
gs_details_page_app_update_button_cb (GtkWidget *widget, GsDetailsPage *self)
{
	g_set_object (&self->app_cancellable, gs_app_get_cancellable (self->app));
	gs_page_update_app (GS_PAGE (self), self->app, self->app_cancellable);
}

static void
gs_details_page_addon_selected_cb (GsAppAddonRow *row,
                                   GParamSpec *pspec,
                                   GsDetailsPage *self)
{
	GsApp *addon;

	addon = gs_app_addon_row_get_addon (row);

	/* If the main app is already installed, ticking the addon checkbox
	 * triggers an immediate install. Otherwise we'll install the addon
	 * together with the main app. */
	switch (gs_app_get_state (self->app)) {
	case AS_APP_STATE_INSTALLED:
	case AS_APP_STATE_UPDATABLE:
	case AS_APP_STATE_UPDATABLE_LIVE:
		if (gs_app_addon_row_get_selected (row)) {
			g_set_object (&self->app_cancellable, gs_app_get_cancellable (addon));
			gs_page_install_app (GS_PAGE (self), addon, GS_SHELL_INTERACTION_FULL,
					     self->app_cancellable);
		} else {
			g_set_object (&self->app_cancellable, gs_app_get_cancellable (addon));
			gs_page_remove_app (GS_PAGE (self), addon, self->app_cancellable);
			/* make sure the addon checkboxes are synced if the
			 * user clicks cancel in the remove confirmation dialog */
			gs_details_page_refresh_addons (self);
			gs_details_page_refresh_all (self);
		}
		break;
	default:
		break;
	}
}

static void
gs_details_page_app_launch_button_cb (GtkWidget *widget, GsDetailsPage *self)
{
	g_autoptr(GCancellable) cancellable = g_cancellable_new ();

	/* hide the notification */
	g_application_withdraw_notification (g_application_get_default (),
					     "installed");

	g_set_object (&self->cancellable, cancellable);
	gs_page_launch_app (GS_PAGE (self), self->app, self->cancellable);
}

static void
gs_details_page_app_add_shortcut_button_cb (GtkWidget *widget,
                                            GsDetailsPage *self)
{
	g_autoptr(GCancellable) cancellable = g_cancellable_new ();
	g_set_object (&self->cancellable, cancellable);
	gs_page_shortcut_add (GS_PAGE (self), self->app, self->cancellable);
}

static void
gs_details_page_app_remove_shortcut_button_cb (GtkWidget *widget,
                                               GsDetailsPage *self)
{
	g_autoptr(GCancellable) cancellable = g_cancellable_new ();
	g_set_object (&self->cancellable, cancellable);
	gs_page_shortcut_remove (GS_PAGE (self), self->app, self->cancellable);
}

static void
gs_details_page_review_response_cb (GtkDialog *dialog,
                                    gint response,
                                    GsDetailsPage *self)
{
	g_autofree gchar *text = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(AsReview) review = NULL;
	g_autoptr(GsPluginJob) plugin_job = NULL;
	GsDetailsPageReviewHelper *helper;
	GsReviewDialog *rdialog = GS_REVIEW_DIALOG (dialog);

	/* not agreed */
	if (response != GTK_RESPONSE_OK) {
		gtk_widget_destroy (GTK_WIDGET (dialog));
		return;
	}

	review = as_review_new ();
	as_review_set_summary (review, gs_review_dialog_get_summary (rdialog));
	text = gs_review_dialog_get_text (rdialog);
	as_review_set_description (review, text);
	as_review_set_rating (review, gs_review_dialog_get_rating (rdialog));
	as_review_set_version (review, gs_app_get_version (self->app));
	now = g_date_time_new_now_local ();
	as_review_set_date (review, now);

	/* call into the plugins to set the new value */
	helper = g_new0 (GsDetailsPageReviewHelper, 1);
	helper->self = g_object_ref (self);
	helper->app = g_object_ref (self->app);
	helper->review = g_object_ref (review);
	helper->action = GS_PLUGIN_ACTION_REVIEW_SUBMIT;
	plugin_job = gs_plugin_job_newv (GS_PLUGIN_ACTION_REVIEW_SUBMIT,
					 "interactive", TRUE,
					 "app", helper->app,
					 "review", helper->review,
					 NULL);
	gs_plugin_loader_job_process_async (self->plugin_loader, plugin_job,
					    self->cancellable,
					    gs_details_page_app_set_review_cb,
					    helper);

	/* unmap the dialog */
	gtk_widget_destroy (GTK_WIDGET (dialog));
}

static void
gs_details_page_write_review_cb (GtkButton *button,
                                 GsDetailsPage *self)
{
	GtkWidget *dialog;
	dialog = gs_review_dialog_new ();
	g_signal_connect (dialog, "response",
			  G_CALLBACK (gs_details_page_review_response_cb), self);
	gs_shell_modal_dialog_present (self->shell, GTK_DIALOG (dialog));
}

static void
gs_details_page_copy_dests_notify_cb (GsPluginLoader *plugin_loader,
				      GParamSpec *pspec,
				      GsDetailsPage *self)
{
	g_clear_pointer (&self->copy_dests, g_ptr_array_unref);
	self->copy_dests = gs_plugin_loader_dup_copy_dests (plugin_loader);
	gs_details_page_update_copy_button (self);
}

static void
gs_details_page_app_installed (GsPage *page, GsApp *app)
{
	gs_details_page_reload (page);
}

static void
gs_details_page_app_removed (GsPage *page, GsApp *app)
{
	gs_details_page_reload (page);
}

static void
show_all_cb (GtkWidget *widget, gpointer user_data)
{
	gtk_widget_show (widget);
}

static void
gs_details_page_app_copied (GsPage *page, GsApp *app, const GError *error)
{
	GsDetailsPage *self = GS_DETAILS_PAGE (page);

	if(self->app != app)
		return;

	if (error == NULL) {
		gtk_button_set_label (GTK_BUTTON (self->button_copy), _("Copied to USB"));
		gtk_widget_set_sensitive (self->button_copy, FALSE);
	} else {
		/* This should reset to the ‘Copy to USB’ original text. The
		 * error is displayed by the shell separately as a popup
		 * notification. */
		gs_details_page_update_copy_button (self);
	}
}

static void
gs_details_page_more_reviews_button_cb (GtkWidget *widget, GsDetailsPage *self)
{
	self->show_all_reviews = TRUE;
	gtk_container_foreach (GTK_CONTAINER (self->list_box_reviews),
	                       show_all_cb, NULL);
	gtk_widget_set_visible (self->button_more_reviews, FALSE);
}

static void
gs_details_page_content_rating_button_cb (GtkWidget *widget, GsDetailsPage *self)
{
	AsContentRating *cr;
	AsContentRatingValue value_bad = AS_CONTENT_RATING_VALUE_NONE;
	const gchar *tmp;
	g_autofree const gchar **ids = NULL;
	g_autoptr(GString) str = g_string_new (NULL);

	/* Ordered from worst to best */
	const gchar *violence_group[] = {
		"violence-bloodshed",
		"violence-realistic",
		"violence-fantasy",
		"violence-cartoon",
		NULL
	};
	const gchar *social_group[] = {
		"social-audio",
		"social-chat",
		"social-contacts",
		"social-info",
		NULL
	};

	cr = gs_app_get_content_rating (self->app);
	if (cr == NULL)
		return;

	ids = gs_content_rating_get_all_rating_ids ();

	/* get the worst thing */
	for (gsize i = 0; ids[i] != NULL; i++) {
		AsContentRatingValue value;
		value = as_content_rating_get_value (cr, ids[i]);
		if (value > value_bad)
			value_bad = value;
	}

	/* get the content rating description for the worst things about the app;
	 * handle the groups separately*/
	for (gsize i = 0; ids[i] != NULL; i++) {
		if (!g_strv_contains (violence_group, ids[i]) &&
		    !g_strv_contains (social_group, ids[i])) {
			AsContentRatingValue value;
			value = as_content_rating_get_value (cr, ids[i]);
			if (value < value_bad)
				continue;
			tmp = gs_content_rating_key_value_to_str (ids[i], value);
			g_string_append_printf (str, "• %s\n", tmp);
		}
	}

	for (gsize i = 0; violence_group[i] != NULL; i++) {
		AsContentRatingValue value;
		value = as_content_rating_get_value (cr, violence_group[i]);
		if (value < value_bad)
			continue;
		tmp = gs_content_rating_key_value_to_str (violence_group[i], value);
		g_string_append_printf (str, "• %s\n", tmp);
		break;
	}

	for (gsize i = 0; social_group[i] != NULL; i++) {
		AsContentRatingValue value;
		value = as_content_rating_get_value (cr, social_group[i]);
		if (value < value_bad)
			continue;
		tmp = gs_content_rating_key_value_to_str (social_group[i], value);
		g_string_append_printf (str, "• %s\n", tmp);
		break;
	}

	if (str->len > 0)
		g_string_truncate (str, str->len - 1);

	/* enable the details if there are any */
	gtk_label_set_label (GTK_LABEL (self->label_content_rating_message), str->str);
	gtk_widget_set_visible (self->label_content_rating_title, str->len > 0);
	gtk_widget_set_visible (self->label_content_rating_message, str->len > 0);
	gtk_widget_set_visible (self->label_content_rating_none, str->len == 0);

	/* show popover */
	gtk_popover_set_relative_to (GTK_POPOVER (self->popover_content_rating), widget);
	gtk_widget_show (self->popover_content_rating);
}

static void
gs_details_page_permissions_button_cb (GtkWidget *widget, GsDetailsPage *self)
{
	gtk_widget_show (self->popover_permissions);
}

static gboolean
gs_details_page_activate_link_cb (GtkLabel *label,
                                  const gchar *uri,
                                  GsDetailsPage *self)
{
	gs_shell_show_uri (self->shell, uri);
	return TRUE;
}

static GtkWidget *
gs_details_page_label_widget (GsDetailsPage *self,
                              const gchar *title,
                              const gchar *url)
{
	GtkWidget *w;
	g_autofree gchar *markup = NULL;

	markup = g_strdup_printf ("<a href=\"%s\">%s</a>", url, title);
	w = gtk_label_new (markup);
	g_signal_connect (w, "activate-link",
			  G_CALLBACK (gs_details_page_activate_link_cb),
			  self);
	gtk_label_set_use_markup (GTK_LABEL (w), TRUE);
	gtk_label_set_xalign (GTK_LABEL (w), 0.f);
	gtk_widget_set_visible (w, TRUE);
	return w;
}

static GtkWidget *
gs_details_page_license_widget_for_token (GsDetailsPage *self, const gchar *token)
{
	/* public domain */
	if (g_strcmp0 (token, "@LicenseRef-public-domain") == 0) {
		/* TRANSLATORS: see the wikipedia page */
		return gs_details_page_label_widget (self, _("Public domain"),
			/* TRANSLATORS: Replace the link with a version in your language,
			 * e.g. https://de.wikipedia.org/wiki/Gemeinfreiheit */
			_("https://en.wikipedia.org/wiki/Public_domain"));
	}

	/* free software, license unspecified */
	if (g_str_has_prefix (token, "@LicenseRef-free")) {
		/* TRANSLATORS: Replace the link with a version in your language,
		 * e.g. https://www.gnu.org/philosophy/free-sw.de */
		const gchar *url = _("https://www.gnu.org/philosophy/free-sw");
		gchar *tmp;

		/* we support putting a custom URL in the
		 * token string, e.g. @LicenseRef-free=http://ubuntu.com */
		tmp = g_strstr_len (token, -1, "=");
		if (tmp != NULL)
			url = tmp + 1;

		/* TRANSLATORS: see GNU page */
		return gs_details_page_label_widget (self, _("Free Software"), url);
	}

	/* SPDX value */
	if (g_str_has_prefix (token, "@")) {
		g_autofree gchar *uri = NULL;
		uri = g_strdup_printf ("http://spdx.org/licenses/%s",
				       token + 1);
		return gs_details_page_label_widget (self, token + 1, uri);
	}

	/* new SPDX value the extractor didn't know about */
	if (as_utils_is_spdx_license_id (token)) {
		g_autofree gchar *uri = NULL;
		uri = g_strdup_printf ("http://spdx.org/licenses/%s",
				       token);
		return gs_details_page_label_widget (self, token, uri);
	}

	/* nothing to show */
	return NULL;
}

static void
gs_details_page_license_free_cb (GtkWidget *widget, GsDetailsPage *self)
{
	guint cnt = 0;
	guint i;
	g_auto(GStrv) tokens = NULL;

	/* URLify any SPDX IDs */
	gs_container_remove_all (GTK_CONTAINER (self->box_details_license_list));
	tokens = as_utils_spdx_license_tokenize (gs_app_get_license (self->app));
	for (i = 0; tokens[i] != NULL; i++) {
		GtkWidget *w = NULL;

		/* translated join */
		if (g_strcmp0 (tokens[i], "&") == 0)
			continue;
		if (g_strcmp0 (tokens[i], "|") == 0)
			continue;
		if (g_strcmp0 (tokens[i], "+") == 0)
			continue;

		/* add widget */
		w = gs_details_page_license_widget_for_token (self, tokens[i]);
		if (w == NULL)
			continue;
		gtk_container_add (GTK_CONTAINER (self->box_details_license_list), w);

		/* one more license */
		cnt++;
	}

	/* use the correct plural */
	gtk_label_set_label (GTK_LABEL (self->label_licenses_intro),
			     /* TRANSLATORS: for the free software popover */
			     ngettext ("Users are bound by the following license:",
				       "Users are bound by the following licenses:",
				       cnt));
	gtk_widget_set_visible (self->label_licenses_intro, cnt > 0);

	gtk_widget_show (self->popover_license_free);
}

static void
gs_details_page_license_nonfree_cb (GtkWidget *widget, GsDetailsPage *self)
{
	g_autofree gchar *str = NULL;
	g_autofree gchar *uri = NULL;
	g_auto(GStrv) tokens = NULL;

	/* license specified as a link */
	tokens = as_utils_spdx_license_tokenize (gs_app_get_license (self->app));
	for (guint i = 0; tokens[i] != NULL; i++) {
		if (g_str_has_prefix (tokens[i], "@LicenseRef-proprietary=")) {
			uri = g_strdup (tokens[i] + 24);
			break;
		}
	}
	if (uri == NULL)
		uri = g_settings_get_string (self->settings, "nonfree-software-uri");
	str = g_strdup_printf ("<a href=\"%s\">%s</a>",
			       uri,
			       _("More information"));
	gtk_label_set_label (GTK_LABEL (self->label_license_nonfree_details), str);
	gtk_widget_show (self->popover_license_nonfree);
}

static void
gs_details_page_license_unknown_cb (GtkWidget *widget, GsDetailsPage *self)
{
	gtk_widget_show (self->popover_license_unknown);
}

static void
gs_details_page_network_available_notify_cb (GsPluginLoader *plugin_loader,
                                             GParamSpec *pspec,
                                             GsDetailsPage *self)
{
	gs_details_page_refresh_reviews (self);
}

static void
gs_details_page_star_pressed_cb(GtkWidget *widget, GdkEventButton *event, GsDetailsPage *self)
{
	gs_details_page_write_review_cb(GTK_BUTTON (self->button_review), self);
}

static void
gs_details_page_plugin_status_changed_cb (GsPluginLoader *plugin_loader,
                                          GsApp *app,
                                          GsPluginStatus status,
                                          GsDetailsPage *self)
{
	if (app == NULL || self->app == NULL)
		return;

	/* Various bits of UI state depend on the plugin status, so refresh
	 * everything. */
	gs_details_page_refresh_all (self);

	if (status == GS_PLUGIN_STATUS_COPYING) {
		/* prevent the app from being deleted, etc. while it's being copied */
		gs_app_add_quirk (app, GS_APP_QUIRK_COMPULSORY);
	} else {
		/* allow removal once copying has completed */
		gs_app_remove_quirk (app, GS_APP_QUIRK_COMPULSORY);
	}
}

static gboolean
gs_details_page_setup (GsPage *page,
                       GsShell *shell,
                       GsPluginLoader *plugin_loader,
                       GtkBuilder *builder,
                       GCancellable *cancellable,
                       GError **error)
{
	GsDetailsPage *self = GS_DETAILS_PAGE (page);
	GtkAdjustment *adj;
	GtkWidget *origin_popover_list_box;

	g_return_val_if_fail (GS_IS_DETAILS_PAGE (self), TRUE);

	self->shell = shell;

	self->plugin_loader = g_object_ref (plugin_loader);
	self->builder = g_object_ref (builder);
	self->cancellable = g_object_ref (cancellable);
	self->copy_dests = NULL;
	g_signal_connect (plugin_loader, "notify::copy-dests",
			  G_CALLBACK (gs_details_page_copy_dests_notify_cb),
			  self);
	gs_details_page_copy_dests_notify_cb (plugin_loader, NULL, self);

	/* show review widgets if we have plugins that provide them */
	self->enable_reviews =
		gs_plugin_loader_get_plugin_supported (plugin_loader,
						       "gs_plugin_review_submit");
	g_signal_connect (self->button_review, "clicked",
			  G_CALLBACK (gs_details_page_write_review_cb),
			  self);
	g_signal_connect (self->star_eventbox, "button-press-event",
			  G_CALLBACK (gs_details_page_star_pressed_cb),
			  self);

	/* hide some UI when offline */
	g_signal_connect_object (self->plugin_loader, "notify::network-available",
				 G_CALLBACK (gs_details_page_network_available_notify_cb),
				 self, 0);

	/* update UI when copying updates to removable media */
	g_signal_connect_object (self->plugin_loader, "status-changed",
				 G_CALLBACK (gs_details_page_plugin_status_changed_cb),
				 self, 0);

	/* setup details */
	g_signal_connect (self->button_install, "clicked",
			  G_CALLBACK (gs_details_page_app_install_button_cb),
			  self);
	g_signal_connect (self->button_update, "clicked",
			  G_CALLBACK (gs_details_page_app_update_button_cb),
			  self);
	g_signal_connect (self->button_remove, "clicked",
			  G_CALLBACK (gs_details_page_app_remove_button_cb),
			  self);
	g_signal_connect (self->button_cancel, "clicked",
			  G_CALLBACK (gs_details_page_app_cancel_button_cb),
			  self);
	g_signal_connect (self->button_copy, "clicked",
			  G_CALLBACK (gs_details_page_app_copy_button_cb),
			  self);
	g_signal_connect (self->button_more_reviews, "clicked",
			  G_CALLBACK (gs_details_page_more_reviews_button_cb),
			  self);
	g_signal_connect (self->button_details_rating_value, "clicked",
			  G_CALLBACK (gs_details_page_content_rating_button_cb),
			  self);
	g_signal_connect (self->button_details_permissions_value, "clicked",
			  G_CALLBACK (gs_details_page_permissions_button_cb),
			  self);
	g_signal_connect (self->label_details_updated_value, "activate-link",
			  G_CALLBACK (gs_details_page_history_cb),
			  self);
	g_signal_connect (self->button_details_launch, "clicked",
			  G_CALLBACK (gs_details_page_app_launch_button_cb),
			  self);
	g_signal_connect (self->button_details_add_shortcut, "clicked",
			  G_CALLBACK (gs_details_page_app_add_shortcut_button_cb),
			  self);
	g_signal_connect (self->button_details_remove_shortcut, "clicked",
			  G_CALLBACK (gs_details_page_app_remove_shortcut_button_cb),
			  self);
	g_signal_connect (self->button_details_website, "clicked",
			  G_CALLBACK (gs_details_page_website_cb),
			  self);
	g_signal_connect (self->button_donate, "clicked",
			  G_CALLBACK (gs_details_page_donate_cb),
			  self);
	g_signal_connect (self->button_details_license_free, "clicked",
			  G_CALLBACK (gs_details_page_license_free_cb),
			  self);
	g_signal_connect (self->button_details_license_nonfree, "clicked",
			  G_CALLBACK (gs_details_page_license_nonfree_cb),
			  self);
	g_signal_connect (self->button_details_license_unknown, "clicked",
			  G_CALLBACK (gs_details_page_license_unknown_cb),
			  self);
	g_signal_connect (self->label_license_nonfree_details, "activate-link",
			  G_CALLBACK (gs_details_page_activate_link_cb),
			  self);
	origin_popover_list_box = GTK_WIDGET (gtk_builder_get_object (self->builder, "origin_popover_list_box"));
	gtk_list_box_set_sort_func (GTK_LIST_BOX (origin_popover_list_box),
	                            origin_popover_list_sort_func,
	                            NULL, NULL);
	gtk_list_box_set_header_func (GTK_LIST_BOX (origin_popover_list_box),
	                              list_header_func,
	                              NULL, NULL);
	g_signal_connect (origin_popover_list_box, "row-activated",
	                  G_CALLBACK (origin_popover_row_activated_cb),
	                  self);

	adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->scrolledwindow_details));
	gtk_container_set_focus_vadjustment (GTK_CONTAINER (self->box_details), adj);
	return TRUE;
}

static void
gs_details_page_dispose (GObject *object)
{
	GsDetailsPage *self = GS_DETAILS_PAGE (object);

	stop_progress_pulsing (self);

	if (self->app != NULL) {
		g_signal_handlers_disconnect_by_func (self->app, gs_details_page_notify_state_changed_cb, self);
		g_signal_handlers_disconnect_by_func (self->app, gs_details_page_progress_changed_cb, self);
		g_clear_object (&self->app);
	}
	g_clear_object (&self->app_local_file);

	g_signal_handlers_disconnect_by_func (self->plugin_loader, gs_details_page_copy_dests_notify_cb, self);
	g_clear_pointer (&self->copy_dests, g_ptr_array_unref);

	g_clear_object (&self->builder);
	g_clear_object (&self->plugin_loader);
	g_clear_object (&self->cancellable);
	g_clear_object (&self->app_cancellable);
	g_clear_object (&self->session);
	g_clear_object (&self->size_group_origin_popover);
	g_clear_object (&self->button_details_rating_style_provider);

	G_OBJECT_CLASS (gs_details_page_parent_class)->dispose (object);
}

static void
gs_details_page_class_init (GsDetailsPageClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GsPageClass *page_class = GS_PAGE_CLASS (klass);
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

	object_class->dispose = gs_details_page_dispose;
	page_class->app_installed = gs_details_page_app_installed;
	page_class->app_removed = gs_details_page_app_removed;
	page_class->app_copied = gs_details_page_app_copied;
	page_class->switch_to = gs_details_page_switch_to;
	page_class->reload = gs_details_page_reload;
	page_class->setup = gs_details_page_setup;

	gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/Software/gs-details-page.ui");

	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, application_details_icon);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, application_details_summary);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, application_details_title);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, box_addons);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, box_details);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, box_details_description);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, box_details_support);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, box_progress);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, box_progress2);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, star);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, label_review_count);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, box_details_screenshot);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, box_details_screenshot_main);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, box_details_screenshot_scrolledwindow);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, box_details_screenshot_thumbnails);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, box_details_license_list);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, button_details_launch);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, button_details_add_shortcut);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, button_details_remove_shortcut);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, button_details_website);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, button_donate);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, button_install);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, button_update);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, button_remove);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, button_cancel);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, button_more_reviews);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, button_copy);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, infobar_details_app_norepo);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, infobar_details_app_repo);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, infobar_details_package_baseos);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, infobar_details_repo);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, label_addons_uninstalled_app);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, label_progress_percentage);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, label_progress_status);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, label_details_category_title);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, label_details_category_value);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, label_details_developer_title);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, label_details_developer_value);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, box_details_developer);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, image_details_developer_verified);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, button_details_license_free);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, button_details_license_nonfree);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, button_details_license_unknown);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, label_details_license_title);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, box_details_license_value);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, label_details_channel_title);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, label_details_channel_value);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, label_details_origin_title);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, label_details_origin_value);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, label_details_size_download_title);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, label_details_size_download_value);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, label_details_size_installed_title);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, label_details_size_installed_value);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, label_details_updated_title);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, label_details_updated_value);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, label_details_version_title);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, label_details_version_value);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, label_details_permissions_title);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, button_details_permissions_value);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, label_failed);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, list_box_addons);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, box_reviews);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, box_details_screenshot_fallback);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, histogram);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, button_review);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, list_box_reviews);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, scrolledwindow_details);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, spinner_details);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, spinner_remove);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, stack_details);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, grid_details_kudo);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, image_details_kudo_docs);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, image_details_kudo_sandboxed);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, image_details_kudo_integration);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, image_details_kudo_translated);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, image_details_kudo_updated);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, label_details_kudo_docs);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, label_details_kudo_sandboxed);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, label_details_kudo_integration);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, label_details_kudo_translated);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, label_details_kudo_updated);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, progressbar_top);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, popover_license_free);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, popover_license_nonfree);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, popover_license_unknown);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, label_license_nonfree_details);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, label_licenses_intro);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, popover_content_rating);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, label_content_rating_title);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, label_content_rating_message);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, label_content_rating_none);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, button_details_rating_value);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, label_details_rating_title);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, popover_permissions);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, box_permissions_details);
	gtk_widget_class_bind_template_child (widget_class, GsDetailsPage, star_eventbox);
}

static void
gs_details_page_init (GsDetailsPage *self)
{
	gtk_widget_init_template (GTK_WIDGET (self));

	/* setup networking */
	self->session = soup_session_new_with_options (SOUP_SESSION_USER_AGENT, gs_user_agent (),
	                                               NULL);
	self->settings = g_settings_new ("org.gnome.software");
	g_signal_connect_swapped (self->settings, "changed",
				  G_CALLBACK (settings_changed_cb),
				  self);
	self->size_group_origin_popover = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);

	gtk_list_box_set_header_func (GTK_LIST_BOX (self->list_box_addons),
				      list_header_func,
				      self, NULL);
	gtk_list_box_set_sort_func (GTK_LIST_BOX (self->list_box_addons),
				    list_sort_func,
				    self, NULL);
}

GsDetailsPage *
gs_details_page_new (void)
{
	GsDetailsPage *self;
	self = g_object_new (GS_TYPE_DETAILS_PAGE, NULL);
	return GS_DETAILS_PAGE (self);
}
