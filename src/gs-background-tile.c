/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2013 Matthias Clasen <mclasen@redhat.com>
 * Copyright (C) 2017-2018 Endless, Inc.
 *
 * Authors:
 *     Joaquim Rocha <jrocha@endlessm.com>
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
#include <gtk/gtk.h>

#include "gs-background-tile.h"
#include "gs-star-widget.h"
#include "gs-common.h"

struct _GsBackgroundTile
{
	GsAppTile	 parent_instance;

	GsApp		*app;
	GtkWidget	*name_label;
	GtkWidget	*summary_label;
	GtkWidget	*image;
	GtkWidget	*image_box;
	GtkWidget	*installed_icon;
	GtkWidget	*scheduled_update_icon;
	GtkWidget	*price;
	GtkWidget	*stack;
	GtkWidget	*stack_tile_status;
};

G_DEFINE_TYPE (GsBackgroundTile, gs_background_tile, GS_TYPE_APP_TILE)

static GsApp *
gs_background_tile_get_app (GsAppTile *tile)
{
	return GS_BACKGROUND_TILE (tile)->app;
}

static void
update_tile_colors_bg (GsBackgroundTile *tile)
{
	GsApp *app = tile->app;
	const guint num_colors = 4;
	const GPtrArray *colors = gs_app_get_key_colors (app);
	/* initialise with 1 extra member because we need the last element to be NULL*/
	g_auto(GStrv) gradients = g_new0 (gchar *, num_colors + 1);
	g_autofree gchar *css = NULL;
	g_autofree gchar *gradients_str = NULL;
	gint degrees = 45;

	if (colors->len == 0)
		return;

	/* apply one color gradient for each color from the corners in the
	 * following fashion:
	 * -------  -------  -------  -------
	 * -     -  - /   -  - / \ -  - / \ -
	 * - \   -  - \   -  - \   -  - \ / -
	 * -------  -------  -------  -------
	 */
	for (guint i = 0; i < num_colors; ++i) {
		GdkRGBA *rgba = g_ptr_array_index (colors, i % colors->len);
		g_autofree gchar *rgba_str = NULL;
		g_autofree gchar *rgba_str2 = NULL;

		/* the gradient will go from an solid color to the same color with more
		 * transparency */
		rgba->alpha = 1.0;
		rgba_str = gdk_rgba_to_string (rgba);

		rgba->alpha = 0.2;
		rgba_str2 = gdk_rgba_to_string (rgba);

		gradients[i] = g_strdup_printf ("linear-gradient(%udeg, %s, %s 40%%)",
						degrees + i * 90, rgba_str, rgba_str2);
	}

	gradients_str = g_strjoinv (",", gradients);
	css = g_strconcat ("background: ", gradients_str, ";", NULL);

	gs_utils_widget_set_css (GTK_WIDGET (tile->image_box), css);
}

static void
update_tile_background (GsBackgroundTile *tile)
{
	const gchar *css = gs_app_get_metadata_item (tile->app, "GnomeSoftware::BackgroundTile-css");

	if (css != NULL)
		gs_utils_widget_set_css (GTK_WIDGET (tile->image_box), css);
	else
		update_tile_colors_bg (tile);
}

static void
update_tile_status (GsBackgroundTile *tile)
{
	GsPrice *price = NULL;
	GtkStack *status_stack = GTK_STACK (tile->stack_tile_status);


	if (gs_app_is_installed (tile->app)) {
		gtk_stack_set_visible_child (status_stack,
					     tile->installed_icon);
		return;
	}

	price = gs_app_get_price (tile->app);
	if (price == NULL || (gs_price_get_amount (price) == (gdouble) 0)) {
		gtk_stack_set_visible_child (status_stack,
					     tile->requires_download_icon);
	} else {
		g_autofree gchar *price_str = gs_price_to_string (price);
		gtk_label_set_label (GTK_LABEL (tile->price), price_str);
		gtk_stack_set_visible_child (status_stack, tile->price);
	}
}

static void
update_tile_info (GsBackgroundTile *tile)
{
	g_autofree gchar *name = NULL;
	AtkObject *accessible = gtk_widget_get_accessible (GTK_WIDGET (tile));
	const gchar *summary = NULL;

	if (gs_app_is_installed (tile->app)) {
		/* TRANSLATORS: This is the name and state of an app for the ATK object */
		name = g_strdup_printf (_("%s (Installed)"), gs_app_get_name (tile->app));
	} else if (gs_app_get_state (tile->app) == AS_APP_STATE_AVAILABLE) {
		name = g_strdup (gs_app_get_name (tile->app));
	}

	summary = gs_app_get_summary (tile->app);

	if (GTK_IS_ACCESSIBLE (accessible)) {
		atk_object_set_name (accessible, name);
		atk_object_set_description (accessible, summary);
	}

	gtk_label_set_label (GTK_LABEL (tile->name_label), gs_app_get_name (tile->app));
	gtk_label_set_label (GTK_LABEL (tile->summary_label), summary);

	update_tile_status (tile);
}

static gboolean
app_state_changed_idle (gpointer user_data)
{
	GsBackgroundTile *tile = GS_BACKGROUND_TILE (user_data);

	update_tile_info (tile);
	g_object_unref (tile);

	return G_SOURCE_REMOVE;
}

static void
app_state_changed (GsApp *app,
		   GParamSpec *pspec,
		   GsBackgroundTile *tile)
{
	/* we call the function in idle because the state-change
	 * notification could be coming from a different thread */
	g_idle_add (app_state_changed_idle, g_object_ref (tile));
}

static void
gs_background_tile_disconnect_app_signals (GsBackgroundTile *tile)
{
	if (tile->app == NULL)
		return;

	g_signal_handlers_disconnect_by_func (tile->app, app_state_changed, tile);
	g_signal_handlers_disconnect_by_func (tile->app, update_tile_background, tile);
}

static void
gs_background_tile_set_app (GsAppTile *app_tile,
			    GsApp *app)
{
	GsBackgroundTile *tile = GS_BACKGROUND_TILE (app_tile);
	GdkPixbuf *pixbuf = NULL;

	g_return_if_fail (GS_IS_APP (app) || app == NULL);

	gs_background_tile_disconnect_app_signals (tile);

	g_set_object (&tile->app, app);

	if (app == NULL)
		return;

	gtk_stack_set_visible_child_name (GTK_STACK (tile->stack), "content");

	g_signal_connect (tile->app, "notify::state",
			  G_CALLBACK (app_state_changed), tile);
	g_signal_connect (tile->app, "notify::name",
			  G_CALLBACK (app_state_changed), tile);
	g_signal_connect (tile->app, "notify::summary",
			  G_CALLBACK (app_state_changed), tile);
	g_signal_connect_swapped (tile->app, "notify::key-colors",
				  G_CALLBACK (update_tile_background), tile);
	g_signal_connect_swapped (tile->app, "metadata-changed::GnomeSoftware::BackgroundTile-css",
				  G_CALLBACK (update_tile_background), tile);

	update_tile_info (tile);
	update_tile_background (tile);

	pixbuf = gs_app_get_pixbuf (tile->app);
	if (pixbuf != NULL)
		gs_image_set_from_pixbuf (GTK_IMAGE (tile->image), pixbuf);
	else
		gtk_image_set_from_icon_name (GTK_IMAGE (tile->image),
					      "application-x-executable",
					      GTK_ICON_SIZE_DIALOG);
}

static void
gs_background_tile_destroy (GtkWidget *widget)
{
	GsBackgroundTile *tile = GS_BACKGROUND_TILE (widget);

	gs_background_tile_disconnect_app_signals (tile);

	g_clear_object (&tile->app);

	GTK_WIDGET_CLASS (gs_background_tile_parent_class)->destroy (widget);
}

static void
gs_background_tile_init (GsBackgroundTile *tile)
{
	gtk_widget_set_has_window (GTK_WIDGET (tile), FALSE);
	gtk_widget_init_template (GTK_WIDGET (tile));
}

static void
gs_background_tile_class_init (GsBackgroundTileClass *klass)
{
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
	GsAppTileClass *app_tile_class = GS_APP_TILE_CLASS (klass);

	widget_class->destroy = gs_background_tile_destroy;

	app_tile_class->set_app = gs_background_tile_set_app;
	app_tile_class->get_app = gs_background_tile_get_app;

	gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/Software/gs-background-tile.ui");

	gtk_widget_class_bind_template_child (widget_class, GsBackgroundTile, name_label);
	gtk_widget_class_bind_template_child (widget_class, GsBackgroundTile, summary_label);
	gtk_widget_class_bind_template_child (widget_class, GsBackgroundTile, image);
	gtk_widget_class_bind_template_child (widget_class, GsBackgroundTile, image_box);
	gtk_widget_class_bind_template_child (widget_class, GsBackgroundTile, installed_icon);
	gtk_widget_class_bind_template_child (widget_class, GsBackgroundTile, scheduled_update_icon);
	gtk_widget_class_bind_template_child (widget_class, GsBackgroundTile, price);
	gtk_widget_class_bind_template_child (widget_class, GsBackgroundTile, stack);
	gtk_widget_class_bind_template_child (widget_class, GsBackgroundTile, stack_tile_status);
}

GtkWidget *
gs_background_tile_new (GsApp *app)
{
	GsBackgroundTile *tile = g_object_new (GS_TYPE_BACKGROUND_TILE, NULL);
	gs_app_tile_set_app (GS_APP_TILE (tile), app);
	return GTK_WIDGET (tile);
}

/* vim: set noexpandtab: */
