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

	GtkWidget	*name_label;
	GtkWidget	*summary_label;
	GtkWidget	*image;
	GtkWidget	*image_box;
	GtkWidget	*installed_icon;
	GtkWidget	*scheduled_update_icon;
	GtkWidget	*requires_download_icon;
	GtkWidget	*available_in_usb_icon;
	GtkWidget	*stack;
	GtkWidget	*stack_tile_status;
};

G_DEFINE_TYPE (GsBackgroundTile, gs_background_tile, GS_TYPE_APP_TILE)

static void
update_tile_colors_bg (GsBackgroundTile *tile)
{
	GsApp *app = gs_app_tile_get_app (GS_APP_TILE (tile));
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
	GsApp *app = gs_app_tile_get_app (GS_APP_TILE (tile));
	const gchar *css = gs_app_get_metadata_item (app, "GnomeSoftware::BackgroundTile-css");

	if (css != NULL)
		gs_utils_widget_set_css (GTK_WIDGET (tile->image_box), css);
	else
		update_tile_colors_bg (tile);
}

static void
update_tile_status (GsBackgroundTile *tile)
{
	GsApp *app = gs_app_tile_get_app (GS_APP_TILE (tile));
	GtkStack *status_stack = GTK_STACK (tile->stack_tile_status);

	if (gs_app_get_pending_action (app) == GS_PLUGIN_ACTION_UPDATE) {
	        gtk_stack_set_visible_child (status_stack,
					     tile->scheduled_update_icon);
		return;
	}

	if (gs_app_is_installed (app)) {
		gtk_stack_set_visible_child (status_stack,
					     tile->installed_icon);
		return;
	}

	if (gs_app_has_category (app, "USB"))
		gtk_stack_set_visible_child (status_stack,
					     tile->available_in_usb_icon);
	else
		gtk_stack_set_visible_child (status_stack,
					     tile->requires_download_icon);
}

static void
update_tile_info (GsBackgroundTile *tile)
{
	GsApp *app = gs_app_tile_get_app (GS_APP_TILE (tile));
	g_autofree gchar *name = NULL;
	AtkObject *accessible = gtk_widget_get_accessible (GTK_WIDGET (tile));
	const gchar *summary = NULL;

	if (gs_app_is_installed (app)) {
		/* TRANSLATORS: This is the name and state of an app for the ATK object */
		name = g_strdup_printf (_("%s (Installed)"), gs_app_get_name (app));
	} else {
		name = g_strdup (gs_app_get_name (app));
	}

	summary = gs_app_get_summary (app);

	if (GTK_IS_ACCESSIBLE (accessible)) {
		atk_object_set_name (accessible, name);
		atk_object_set_description (accessible, summary);
	}

	gtk_label_set_label (GTK_LABEL (tile->name_label), gs_app_get_name (app));
	gtk_label_set_label (GTK_LABEL (tile->summary_label), summary);

	update_tile_status (tile);
}

static void
gs_background_tile_refresh (GsAppTile *app_tile)
{
	GsBackgroundTile *tile = GS_BACKGROUND_TILE (app_tile);
	GsApp *app = gs_app_tile_get_app (app_tile);
	GdkPixbuf *pixbuf = NULL;

	if (app == NULL)
		return;

	gtk_stack_set_visible_child_name (GTK_STACK (tile->stack), "content");

	update_tile_info (tile);
	update_tile_background (tile);

	pixbuf = gs_app_get_pixbuf (app);
	if (pixbuf != NULL)
		gs_image_set_from_pixbuf (GTK_IMAGE (tile->image), pixbuf);
	else
		gtk_image_set_from_icon_name (GTK_IMAGE (tile->image),
					      "application-x-executable",
					      GTK_ICON_SIZE_DIALOG);
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

	app_tile_class->refresh = gs_background_tile_refresh;

	gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/Software/gs-background-tile.ui");

	gtk_widget_class_bind_template_child (widget_class, GsBackgroundTile, name_label);
	gtk_widget_class_bind_template_child (widget_class, GsBackgroundTile, summary_label);
	gtk_widget_class_bind_template_child (widget_class, GsBackgroundTile, image);
	gtk_widget_class_bind_template_child (widget_class, GsBackgroundTile, image_box);
	gtk_widget_class_bind_template_child (widget_class, GsBackgroundTile, installed_icon);
	gtk_widget_class_bind_template_child (widget_class, GsBackgroundTile, scheduled_update_icon);
	gtk_widget_class_bind_template_child (widget_class, GsBackgroundTile, requires_download_icon);
	gtk_widget_class_bind_template_child (widget_class, GsBackgroundTile, available_in_usb_icon);
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
