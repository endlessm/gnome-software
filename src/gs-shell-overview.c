/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2013-2016 Richard Hughes <richard@hughsie.com>
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#include <glib/gi18n.h>

#include "gs-shell.h"
#include "gs-shell-overview.h"
#include "gs-app.h"
#include "gs-app-list-private.h"
#include "gs-category.h"
#include "gs-popular-tile.h"
#include "gs-feature-tile.h"
#include "gs-category-tile.h"
#include "gs-common.h"

#define N_TILES 9

typedef struct
{
	GsPluginLoader		*plugin_loader;
	GtkBuilder		*builder;
	GCancellable		*cancellable;
	gboolean		 cache_valid;
	GsShell			*shell;
	gint			 refresh_count;
	gboolean		 loading_featured;
	gboolean		 loading_popular;
	gboolean		 loading_popular_rotating;
	gboolean		 loading_categories;
	gboolean		 empty;
	gchar			*category_of_day;
	GtkWidget		*search_button;

	GtkWidget		*bin_featured;
	GtkWidget		*box_overview;
	GtkWidget		*box_popular;
	GtkWidget		*box_popular_rotating;
	GtkWidget		*category_heading;
	GtkWidget		*flowbox_categories;
	GtkWidget		*flowbox_categories2;
	GtkWidget		*popular_heading;
	GtkWidget		*popular_rotating_heading;
	GtkWidget		*scrolledwindow_overview;
	GtkWidget		*stack_overview;
	GtkWidget		*categories_expander_button;
	GtkWidget		*categories_expander;
	GtkWidget		*categories_more;
} GsShellOverviewPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (GsShellOverview, gs_shell_overview, GS_TYPE_PAGE)

enum {
	SIGNAL_REFRESHED,
	SIGNAL_LAST
};

static guint signals [SIGNAL_LAST] = { 0 };

typedef struct {
        GsCategory	*category;
        GsShellOverview	*self;
} LoadData;

static void
load_data_free (LoadData *data)
{
        if (data->category != NULL)
                g_object_unref (data->category);
        if (data->self != NULL)
                g_object_unref (data->self);
        g_slice_free (LoadData, data);
}

/**
 * gs_shell_overview_invalidate:
 **/
void
gs_shell_overview_invalidate (GsShellOverview *self)
{
	GsShellOverviewPrivate *priv = gs_shell_overview_get_instance_private (self);

	priv->cache_valid = FALSE;
}

static void
popular_tile_clicked (GsPopularTile *tile, gpointer data)
{
	GsShellOverview *self = GS_SHELL_OVERVIEW (data);
	GsShellOverviewPrivate *priv = gs_shell_overview_get_instance_private (self);
	GsApp *app;

	app = gs_popular_tile_get_app (tile);
	gs_shell_show_app (priv->shell, app);
}

static gboolean
filter_category (GsApp *app, gpointer user_data)
{
	const gchar *category = (const gchar *) user_data;

	return !gs_app_has_category (app, category);
}

/**
 * gs_shell_overview_get_popular_cb:
 **/
static void
gs_shell_overview_get_popular_cb (GObject *source_object,
				  GAsyncResult *res,
				  gpointer user_data)
{
	GsShellOverview *self = GS_SHELL_OVERVIEW (user_data);
	GsShellOverviewPrivate *priv = gs_shell_overview_get_instance_private (self);
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (source_object);
	guint i;
	GsApp *app;
	GtkWidget *tile;
	g_autoptr(GError) error = NULL;
	g_autoptr(GsAppList) list = NULL;

	/* get popular apps */
	list = gs_plugin_loader_get_popular_finish (plugin_loader, res, &error);
	gtk_widget_set_visible (priv->box_popular, list != NULL);
	gtk_widget_set_visible (priv->popular_heading, list != NULL);
	if (list == NULL) {
		if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			g_warning ("failed to get popular apps: %s", error->message);
		goto out;
	}
	/* Don't show apps from the category that's currently featured as the category of the day */
	gs_app_list_filter (list, filter_category, priv->category_of_day);
	gs_app_list_randomize (list);

	gs_container_remove_all (GTK_CONTAINER (priv->box_popular));

	for (i = 0; i < gs_app_list_length (list) && i < N_TILES; i++) {
		app = gs_app_list_index (list, i);
		tile = gs_popular_tile_new (app);
		g_signal_connect (tile, "clicked",
			  G_CALLBACK (popular_tile_clicked), self);
		gtk_container_add (GTK_CONTAINER (priv->box_popular), tile);
	}

	priv->empty = FALSE;

out:
	priv->loading_popular = FALSE;
	priv->refresh_count--;
	if (priv->refresh_count == 0) {
		priv->cache_valid = TRUE;
		g_signal_emit (self, signals[SIGNAL_REFRESHED], 0);
	}
}

static void
gs_shell_overview_get_popular_rotating_cb (GObject *source_object,
					   GAsyncResult *res,
					   gpointer user_data)
{
	LoadData *load_data = (LoadData *) user_data;
	GsShellOverview *self = load_data->self;
	GsShellOverviewPrivate *priv = gs_shell_overview_get_instance_private (self);
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (source_object);
	guint i;
	GsApp *app;
	GtkWidget *tile;
	g_autoptr(GError) error = NULL;
	g_autoptr(GsAppList) list = NULL;

	/* get popular apps */
	list = gs_plugin_loader_get_category_apps_finish (plugin_loader, res, &error);
	if (list == NULL) {
		if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			g_warning ("failed to get recommended applications: %s", error->message);
		gtk_widget_hide (priv->popular_rotating_heading);
		gtk_widget_hide (priv->box_popular_rotating);
		goto out;
	} else if (gs_app_list_length (list) < N_TILES) {
		g_warning ("hiding recommended applications: "
			   "found only %d to show, need at least %d",
			   gs_app_list_length (list), N_TILES);
		gtk_widget_hide (priv->popular_rotating_heading);
		gtk_widget_hide (priv->box_popular_rotating);
		goto out;
	}
	gs_app_list_randomize (list);

	gtk_widget_show (priv->popular_rotating_heading);
	gtk_widget_show (priv->box_popular_rotating);

	gs_container_remove_all (GTK_CONTAINER (priv->box_popular_rotating));

	for (i = 0; i < gs_app_list_length (list) && i < N_TILES; i++) {
		app = gs_app_list_index (list, i);
		tile = gs_popular_tile_new (app);
		g_signal_connect (tile, "clicked",
			  G_CALLBACK (popular_tile_clicked), self);
		gtk_container_add (GTK_CONTAINER (priv->box_popular_rotating), tile);
	}

	priv->empty = FALSE;

out:
	load_data_free (load_data);
	priv->loading_popular_rotating = FALSE;
	priv->refresh_count--;
	if (priv->refresh_count == 0) {
		priv->cache_valid = TRUE;
		g_signal_emit (self, signals[SIGNAL_REFRESHED], 0);
	}
}

static void
feature_tile_clicked (GsFeatureTile *tile, gpointer data)
{
	GsShellOverview *self = GS_SHELL_OVERVIEW (data);
	GsShellOverviewPrivate *priv = gs_shell_overview_get_instance_private (self);
	GsApp *app;

	app = gs_feature_tile_get_app (tile);
	gs_shell_show_app (priv->shell, app);
}

static void
gs_shell_overview_get_featured_cb (GObject *source_object,
				   GAsyncResult *res,
				   gpointer user_data)
{
	GsShellOverview *self = GS_SHELL_OVERVIEW (user_data);
	GsShellOverviewPrivate *priv = gs_shell_overview_get_instance_private (self);
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (source_object);
	GtkWidget *tile;
	GsApp *app;
	g_autoptr(GError) error = NULL;
	g_autoptr(GsAppList) list = NULL;

	list = gs_plugin_loader_get_featured_finish (plugin_loader, res, &error);
	if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		goto out;

	if (g_getenv ("GNOME_SOFTWARE_FEATURED") == NULL) {
		/* Don't show apps from the category that's currently featured as the category of the day */
		gs_app_list_filter (list, filter_category, priv->category_of_day);
		gs_app_list_randomize (list);
	}

	gs_container_remove_all (GTK_CONTAINER (priv->bin_featured));
	if (list == NULL) {
		g_warning ("failed to get featured apps: %s",
			   error->message);
		goto out;
	}
	if (gs_app_list_length (list) == 0) {
		g_warning ("failed to get featured apps: "
			   "no apps to show");
		goto out;
	}

	/* at the moment, we only care about the first app */
	app = gs_app_list_index (list, 0);
	tile = gs_feature_tile_new (app);
	g_signal_connect (tile, "clicked",
			  G_CALLBACK (feature_tile_clicked), self);

	gtk_container_add (GTK_CONTAINER (priv->bin_featured), tile);

	priv->empty = FALSE;

out:
	priv->loading_featured = FALSE;
	priv->refresh_count--;
	if (priv->refresh_count == 0) {
		priv->cache_valid = TRUE;
		g_signal_emit (self, signals[SIGNAL_REFRESHED], 0);
	}
}

static void
category_tile_clicked (GsCategoryTile *tile, gpointer data)
{
	GsShellOverview *self = GS_SHELL_OVERVIEW (data);
	GsShellOverviewPrivate *priv = gs_shell_overview_get_instance_private (self);
	GsCategory *category;

	category = gs_category_tile_get_category (tile);
	gs_shell_show_category (priv->shell, category);
}

static void
add_category_tile_to_flowbox (GtkWidget *tile, GtkFlowBox *flowbox)
{
	gtk_flow_box_insert (flowbox, tile, -1);
	gtk_widget_set_can_focus (gtk_widget_get_parent (tile), FALSE);
}

/**
 * gs_shell_overview_get_categories_cb:
 **/
static void
gs_shell_overview_get_categories_cb (GObject *source_object,
				     GAsyncResult *res,
				     gpointer user_data)
{
	GsShellOverview *self = GS_SHELL_OVERVIEW (user_data);
	GsShellOverviewPrivate *priv = gs_shell_overview_get_instance_private (self);
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (source_object);
	guint i;
	GsCategory *cat;
	GtkWidget *tile;
	gboolean has_category = FALSE;
	gboolean use_expander = FALSE;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) list = NULL;
	g_autoptr(GPtrArray) primary_cats = NULL;
	g_autoptr(GPtrArray) secondary_cats = NULL;
	guint remaining_primary_cats = 1;
	GtkFlowBox *flowbox;

	list = gs_plugin_loader_get_categories_finish (plugin_loader, res, &error);
	if (list == NULL) {
		if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			g_warning ("failed to get categories: %s", error->message);
		goto out;
	}
	gs_container_remove_all (GTK_CONTAINER (priv->flowbox_categories));
	gs_container_remove_all (GTK_CONTAINER (priv->flowbox_categories2));

	secondary_cats = g_ptr_array_new ();

	flowbox = GTK_FLOW_BOX (priv->flowbox_categories);
	remaining_primary_cats = gtk_flow_box_get_max_children_per_line (flowbox);

	for (i = 0; i < list->len; i++) {
		cat = GS_CATEGORY (g_ptr_array_index (list, i));
		if (gs_category_get_size (cat) == 0)
			continue;
		tile = gs_category_tile_new (cat);
		g_signal_connect (tile, "clicked",
				  G_CALLBACK (category_tile_clicked), self);

		/* The the important categories directly to the UI and the
		 * secondary to an array for later */
		if (gs_category_get_important (cat)) {
			add_category_tile_to_flowbox (tile, flowbox);
			has_category = TRUE;

			if (remaining_primary_cats > 0)
				--remaining_primary_cats;
		} else {
			g_ptr_array_add (secondary_cats, tile);
		}
	}

	for (i = 0; i < secondary_cats->len; ++i) {
		tile = GTK_WIDGET (g_ptr_array_index (secondary_cats, i));

		/* if we have filled at least a row of categories, then use
		 * the secondary categories widget */
		if (!use_expander && remaining_primary_cats == 0) {
			flowbox = GTK_FLOW_BOX (priv->flowbox_categories2);
			use_expander = TRUE;
			has_category = TRUE;
		}

		add_category_tile_to_flowbox (tile, flowbox);

		if (remaining_primary_cats > 0)
			--remaining_primary_cats;
	}

	/* show the expander if we have too many children */
	gtk_widget_set_visible (priv->categories_expander, use_expander);
out:
	if (has_category) {
		priv->empty = FALSE;
	}
	gtk_widget_set_visible (priv->category_heading, has_category);

	priv->loading_categories = FALSE;
	priv->refresh_count--;
	if (priv->refresh_count == 0) {
		priv->cache_valid = TRUE;
		g_signal_emit (self, signals[SIGNAL_REFRESHED], 0);
	}
}

/**
 * gs_shell_overview_load:
 */
static void
gs_shell_overview_load (GsShellOverview *self)
{
	GsShellOverviewPrivate *priv = gs_shell_overview_get_instance_private (self);
	const gchar *category_of_day;
	g_autoptr(GDateTime) date = NULL;

	priv->empty = TRUE;

	date = g_date_time_new_now_utc ();
	switch (g_date_time_get_day_of_year (date) % 4) {
	case 0:
		category_of_day = "audio-video";
		/* TRANSLATORS: this is a heading for audio applications which have been featured ('recommended') by the distribution */
		gtk_label_set_label (GTK_LABEL (priv->popular_rotating_heading), _("Recommended Audio & Video Applications"));
		break;
	case 1:
		category_of_day = "games";
		/* TRANSLATORS: this is a heading for games which have been featured ('recommended') by the distribution */
		gtk_label_set_label (GTK_LABEL (priv->popular_rotating_heading), _("Recommended Games"));
		break;
	case 2:
		category_of_day = "graphics";
		/* TRANSLATORS: this is a heading for graphics applications which have been featured ('recommended') by the distribution */
		gtk_label_set_label (GTK_LABEL (priv->popular_rotating_heading), _("Recommended Graphics Applications"));
		break;
	case 3:
		category_of_day = "productivity";
		/* TRANSLATORS: this is a heading for office applications which have been featured ('recommended') by the distribution */
		gtk_label_set_label (GTK_LABEL (priv->popular_rotating_heading), _("Recommended Productivity Applications"));
		break;
	default:
		g_assert_not_reached ();
		break;
	}
	g_free (priv->category_of_day);
	priv->category_of_day = g_strdup (category_of_day);

	if (!priv->loading_featured) {
		priv->loading_featured = TRUE;
		gs_plugin_loader_get_featured_async (priv->plugin_loader,
						     GS_PLUGIN_REFINE_FLAGS_REQUIRE_ICON,
						     priv->cancellable,
						     gs_shell_overview_get_featured_cb,
						     self);
		priv->refresh_count++;
	}

	if (!priv->loading_popular) {
		priv->loading_popular = TRUE;
		gs_plugin_loader_get_popular_async (priv->plugin_loader,
						    GS_PLUGIN_REFINE_FLAGS_REQUIRE_ICON,
						    priv->cancellable,
						    gs_shell_overview_get_popular_cb,
						    self);
		priv->refresh_count++;
	}

	if (!priv->loading_popular_rotating) {
		LoadData *load_data;
		g_autoptr(GsCategory) category = NULL;
		g_autoptr(GsCategory) featured_category = NULL;

		category = gs_category_new (category_of_day);
		featured_category = gs_category_new ("featured");
		gs_category_add_child (category, featured_category);

		load_data = g_slice_new0 (LoadData);
		load_data->category = g_object_ref (category);
		load_data->self = g_object_ref (self);

		priv->loading_popular_rotating = TRUE;
		gs_plugin_loader_get_category_apps_async (priv->plugin_loader,
		                                          featured_category,
		                                          GS_PLUGIN_REFINE_FLAGS_REQUIRE_ICON,
		                                          priv->cancellable,
		                                          gs_shell_overview_get_popular_rotating_cb,
		                                          load_data);
		priv->refresh_count++;
	}

	if (!priv->loading_categories) {
		priv->loading_categories = TRUE;
		gs_plugin_loader_get_categories_async (priv->plugin_loader,
						       GS_PLUGIN_REFINE_FLAGS_DEFAULT,
						       priv->cancellable,
						       gs_shell_overview_get_categories_cb,
						       self);
		priv->refresh_count++;
	}
}

/**
 * gs_shell_overview_reload:
 */
void
gs_shell_overview_reload (GsShellOverview *self)
{
	gs_shell_overview_invalidate (self);
	gs_shell_overview_load (self);
}

/**
 * gs_shell_overview_switch_to:
 **/
static void
gs_shell_overview_switch_to (GsPage *page, gboolean scroll_up)
{
	GsShellOverview *self = GS_SHELL_OVERVIEW (page);
	GsShellOverviewPrivate *priv = gs_shell_overview_get_instance_private (self);
	GtkWidget *widget;
	GtkAdjustment *adj;

	if (gs_shell_get_mode (priv->shell) != GS_SHELL_MODE_OVERVIEW) {
		g_warning ("Called switch_to(overview) when in mode %s",
			   gs_shell_get_mode_string (priv->shell));
		return;
	}

	/* we hid the search bar */
	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (priv->search_button), FALSE);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "buttonbox_main"));
	gtk_widget_show (widget);

	/* hide the expander */
	gtk_revealer_set_transition_duration (GTK_REVEALER (priv->categories_expander), 0);
	gtk_revealer_set_transition_duration (GTK_REVEALER (priv->categories_more), 0);
	gtk_revealer_set_reveal_child (GTK_REVEALER (priv->categories_expander), TRUE);
	gtk_revealer_set_reveal_child (GTK_REVEALER (priv->categories_more), FALSE);

	if (scroll_up) {
		adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (priv->scrolledwindow_overview));
		gtk_adjustment_set_value (adj, gtk_adjustment_get_lower (adj));
	}

	gs_grab_focus_when_mapped (priv->scrolledwindow_overview);

	if (priv->cache_valid || priv->refresh_count > 0)
		return;
	gs_shell_overview_load (self);
}

static void
gs_shell_overview_categories_expander_cb (GtkButton *button, GsShellOverview *self)
{
	GsShellOverviewPrivate *priv = gs_shell_overview_get_instance_private (self);
	gtk_revealer_set_transition_duration (GTK_REVEALER (priv->categories_expander), 250);
	gtk_revealer_set_transition_duration (GTK_REVEALER (priv->categories_more), 250);
	gtk_revealer_set_reveal_child (GTK_REVEALER (priv->categories_expander), FALSE);
	gtk_revealer_set_reveal_child (GTK_REVEALER (priv->categories_more), TRUE);
}

void
gs_shell_overview_setup (GsShellOverview *self,
			 GsShell *shell,
			 GsPluginLoader *plugin_loader,
			 GtkBuilder *builder,
			 GCancellable *cancellable)
{
	GsShellOverviewPrivate *priv = gs_shell_overview_get_instance_private (self);
	GtkSearchBar *search_bar;
	GtkAdjustment *adj;
	GtkWidget *tile;
	gint i;

	g_return_if_fail (GS_IS_SHELL_OVERVIEW (self));

	priv->plugin_loader = g_object_ref (plugin_loader);
	priv->builder = g_object_ref (builder);
	priv->cancellable = g_object_ref (cancellable);

	/* avoid a ref cycle */
	priv->shell = shell;

	adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (priv->scrolledwindow_overview));
	gtk_container_set_focus_vadjustment (GTK_CONTAINER (priv->box_overview), adj);

	tile = gs_feature_tile_new (NULL);
	gtk_container_add (GTK_CONTAINER (priv->bin_featured), tile);

	for (i = 0; i < N_TILES; i++) {
		tile = gs_popular_tile_new (NULL);
		gtk_container_add (GTK_CONTAINER (priv->box_popular), tile);

		tile = gs_popular_tile_new (NULL);
		gtk_container_add (GTK_CONTAINER (priv->box_popular_rotating), tile);
	}

	/* handle category expander */
	g_signal_connect (priv->categories_expander_button, "clicked",
			  G_CALLBACK (gs_shell_overview_categories_expander_cb), self);

	/* search button */
	search_bar = GTK_SEARCH_BAR (gtk_builder_get_object (priv->builder,
							     "search_bar"));
	priv->search_button = gs_search_button_new (search_bar);
	gs_page_set_header_end_widget (GS_PAGE (self), priv->search_button);

	/* chain up */
	gs_page_setup (GS_PAGE (self),
	               shell,
	               plugin_loader,
	               cancellable);
}

static void
gs_shell_overview_init (GsShellOverview *self)
{
	gtk_widget_init_template (GTK_WIDGET (self));
}

static void
gs_shell_overview_dispose (GObject *object)
{
	GsShellOverview *self = GS_SHELL_OVERVIEW (object);
	GsShellOverviewPrivate *priv = gs_shell_overview_get_instance_private (self);

	g_clear_object (&priv->builder);
	g_clear_object (&priv->plugin_loader);
	g_clear_object (&priv->cancellable);
	g_clear_pointer (&priv->category_of_day, g_free);

	G_OBJECT_CLASS (gs_shell_overview_parent_class)->dispose (object);
}

static void
gs_shell_overview_refreshed (GsShellOverview *self)
{
	GsShellOverviewPrivate *priv = gs_shell_overview_get_instance_private (self);

	if (priv->empty) {
		gtk_stack_set_visible_child_name (GTK_STACK (priv->stack_overview), "no-results");
	} else {
		gtk_stack_set_visible_child_name (GTK_STACK (priv->stack_overview), "overview");
	}
}

/**
 * gs_shell_overview_class_init:
 **/
static void
gs_shell_overview_class_init (GsShellOverviewClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GsPageClass *page_class = GS_PAGE_CLASS (klass);
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

	object_class->dispose = gs_shell_overview_dispose;
	page_class->switch_to = gs_shell_overview_switch_to;
	klass->refreshed = gs_shell_overview_refreshed;

	signals [SIGNAL_REFRESHED] =
		g_signal_new ("refreshed",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (GsShellOverviewClass, refreshed),
			      NULL, NULL, g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE, 0);

	gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/Software/gs-shell-overview.ui");

	gtk_widget_class_bind_template_child_private (widget_class, GsShellOverview, bin_featured);
	gtk_widget_class_bind_template_child_private (widget_class, GsShellOverview, box_overview);
	gtk_widget_class_bind_template_child_private (widget_class, GsShellOverview, box_popular);
	gtk_widget_class_bind_template_child_private (widget_class, GsShellOverview, box_popular_rotating);
	gtk_widget_class_bind_template_child_private (widget_class, GsShellOverview, category_heading);
	gtk_widget_class_bind_template_child_private (widget_class, GsShellOverview, flowbox_categories);
	gtk_widget_class_bind_template_child_private (widget_class, GsShellOverview, flowbox_categories2);
	gtk_widget_class_bind_template_child_private (widget_class, GsShellOverview, popular_heading);
	gtk_widget_class_bind_template_child_private (widget_class, GsShellOverview, popular_rotating_heading);
	gtk_widget_class_bind_template_child_private (widget_class, GsShellOverview, scrolledwindow_overview);
	gtk_widget_class_bind_template_child_private (widget_class, GsShellOverview, stack_overview);
	gtk_widget_class_bind_template_child_private (widget_class, GsShellOverview, categories_expander_button);
	gtk_widget_class_bind_template_child_private (widget_class, GsShellOverview, categories_expander);
	gtk_widget_class_bind_template_child_private (widget_class, GsShellOverview, categories_more);
}

/**
 * gs_shell_overview_new:
 **/
GsShellOverview *
gs_shell_overview_new (void)
{
	return GS_SHELL_OVERVIEW (g_object_new (GS_TYPE_SHELL_OVERVIEW, NULL));
}

/* vim: set noexpandtab: */
