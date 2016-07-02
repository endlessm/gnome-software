/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2013 Matthias Clasen <mclasen@redhat.com>
 * Copyright (C) 2016 Endless Mobile, Inc.
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

#include "gs-image-tile.h"
#include "gs-star-widget.h"
#include "gs-common.h"

struct _GsImageTile
{
	GsAppTile	 parent_instance;

	GsApp		*app;
	GtkWidget	*app_name;
	GtkWidget	*app_summary;
	GtkWidget	*hover_app_name;
	GtkWidget	*icon;
	GtkWidget	*fallback_icon;
	GtkWidget	*image_box;
	GtkWidget	*eventbox;
	GtkWidget	*stack;
	GtkWidget	*stars;
	GtkWidget	*details_revealer;
};

G_DEFINE_TYPE (GsImageTile, gs_image_tile, GS_TYPE_APP_TILE)

static GsApp *
gs_image_tile_get_app (GsAppTile *tile)
{
	return GS_IMAGE_TILE (tile)->app;
}

static gboolean
app_state_changed_idle (gpointer user_data)
{
	GsImageTile *tile = GS_IMAGE_TILE (user_data);
	AtkObject *accessible;
	GtkWidget *label;
	gboolean installed;
	g_autofree gchar *name = NULL;

	accessible = gtk_widget_get_accessible (GTK_WIDGET (tile));

	label = gtk_bin_get_child (GTK_BIN (tile->eventbox));
	switch (gs_app_get_state (tile->app)) {
	case AS_APP_STATE_INSTALLED:
	case AS_APP_STATE_INSTALLING:
	case AS_APP_STATE_REMOVING:
	case AS_APP_STATE_UPDATABLE:
	case AS_APP_STATE_UPDATABLE_LIVE:
		installed = TRUE;
		name = g_strdup_printf ("%s (%s)",
					gs_app_get_name (tile->app),
					_("Installed"));
		/* TRANSLATORS: this is the small blue label on the tile
		 * that tells the user the application is installed */
		gtk_label_set_label (GTK_LABEL (label), _("Installed"));
		break;
	case AS_APP_STATE_AVAILABLE:
	default:
		installed = FALSE;
		name = g_strdup (gs_app_get_name (tile->app));
		break;
	}

	gtk_widget_set_visible (tile->eventbox, installed);

	if (GTK_IS_ACCESSIBLE (accessible)) {
		atk_object_set_name (accessible, name);
		atk_object_set_description (accessible,
					    gs_app_get_summary (tile->app));
	}

	g_object_unref (tile);
	return G_SOURCE_REMOVE;
}

static void
app_state_changed (GsApp *app, GParamSpec *pspec, GsImageTile *tile)
{
	g_idle_add (app_state_changed_idle, g_object_ref (tile));
}

static void
app_image_tile_css_added (GsApp *app, const char *metadata, GsImageTile *tile)
{
	if (g_strcmp0 (metadata, "GnomeSoftware::ImageTile-css") == 0) {
		gs_utils_widget_set_css_app (app, tile->image_box,
					     "GnomeSoftware::ImageTile-css");
	} else {
		g_assert_not_reached ();
	}
}

static void
gs_image_tile_set_app (GsAppTile *app_tile, GsApp *app)
{
	GsImageTile *tile = GS_IMAGE_TILE (app_tile);

	g_return_if_fail (GS_IS_APP (app) || app == NULL);

	if (tile->app) {
		g_signal_handlers_disconnect_by_func (tile->app,
						      app_state_changed, tile);
		g_signal_handlers_disconnect_by_func (tile->app,
		                                      app_image_tile_css_added, tile);
	}

	g_set_object (&tile->app, app);
	if (!app)
		return;

	if (gs_app_get_rating (tile->app) >= 0) {
		gtk_widget_set_visible (tile->stars, TRUE);
		gs_star_widget_set_rating (GS_STAR_WIDGET (tile->stars),
					   gs_app_get_rating (tile->app));
	} else {
		gtk_widget_set_visible (tile->stars, FALSE);
	}
	gtk_stack_set_visible_child_name (GTK_STACK (tile->stack), "content");

	g_signal_connect (tile->app, "notify::state",
		 	  G_CALLBACK (app_state_changed), tile);
	g_signal_connect (tile->app, "metadata-changed::GnomeSoftware::ImageTile-css",
	                  G_CALLBACK (app_image_tile_css_added), tile);
	app_state_changed (tile->app, NULL, tile);

	/* perhaps set custom css */
	gs_utils_widget_set_css_app (app, GTK_WIDGET (tile->image_box),
				     "GnomeSoftware::ImageTile-css");

	gs_image_set_from_pixbuf (GTK_IMAGE (tile->icon),
				  gs_app_get_pixbuf (tile->app));

	/* The fallback icon should be covered by the main image but
	 * is here for the cases where that image doesn't exist, as
	 * without a graphical reference it is very difficult to spot
	 * the applications */
	gs_image_set_from_pixbuf (GTK_IMAGE (tile->fallback_icon),
				  gs_app_get_pixbuf (tile->app));

	gtk_label_set_label (GTK_LABEL (tile->app_name), gs_app_get_name (app));
	gtk_label_set_label (GTK_LABEL (tile->hover_app_name),
			     gs_app_get_name (app));
	gtk_label_set_label (GTK_LABEL (tile->app_summary),
			     gs_app_get_summary (app));
}

static void
gs_image_tile_destroy (GtkWidget *widget)
{
	GsImageTile *tile = GS_IMAGE_TILE (widget);

	if (tile->app) {
		g_signal_handlers_disconnect_by_func (tile->app,
						      app_state_changed, tile);
		g_signal_handlers_disconnect_by_func (tile->app,
		                                      app_image_tile_css_added,
						      tile);
	}

	g_clear_object (&tile->app);

	GTK_WIDGET_CLASS (gs_image_tile_parent_class)->destroy (widget);
}

static void
gs_image_tile_init (GsImageTile *tile)
{
	gtk_widget_set_has_window (GTK_WIDGET (tile), FALSE);
	gtk_widget_init_template (GTK_WIDGET (tile));
	gs_star_widget_set_icon_size (GS_STAR_WIDGET (tile->stars), 12);
}

static gboolean
gs_image_tile_crossing_notify_event (GtkWidget *widget,
				     GdkEventCrossing *event)
{
	GtkWidgetClass *klass = GTK_WIDGET_CLASS (gs_image_tile_parent_class);
	GsImageTile *tile = GS_IMAGE_TILE (widget);
	gboolean type_enter = (event->type == GDK_ENTER_NOTIFY);

	gtk_revealer_set_reveal_child (GTK_REVEALER (tile->details_revealer),
				       type_enter);

	/* chain up */
	if (type_enter)
		return klass->enter_notify_event (widget, event);

	return klass->leave_notify_event (widget, event);
}

static void
gs_image_tile_class_init (GsImageTileClass *klass)
{
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
	GsAppTileClass *app_tile_class = GS_APP_TILE_CLASS (klass);

	widget_class->destroy = gs_image_tile_destroy;
	widget_class->enter_notify_event = gs_image_tile_crossing_notify_event;
	widget_class->leave_notify_event = gs_image_tile_crossing_notify_event;

	app_tile_class->set_app = gs_image_tile_set_app;
	app_tile_class->get_app = gs_image_tile_get_app;

	gtk_widget_class_set_template_from_resource (widget_class,
					"/org/gnome/Software/gs-image-tile.ui");

	gtk_widget_class_bind_template_child (widget_class, GsImageTile,
					      app_name);
	gtk_widget_class_bind_template_child (widget_class, GsImageTile,
					      app_summary);
	gtk_widget_class_bind_template_child (widget_class, GsImageTile, icon);
	gtk_widget_class_bind_template_child (widget_class, GsImageTile,
					      eventbox);
	gtk_widget_class_bind_template_child (widget_class, GsImageTile,
					      hover_app_name);
	gtk_widget_class_bind_template_child (widget_class, GsImageTile, stack);
	gtk_widget_class_bind_template_child (widget_class, GsImageTile, stars);
	gtk_widget_class_bind_template_child (widget_class, GsImageTile,
					      details_revealer);
	gtk_widget_class_bind_template_child (widget_class, GsImageTile,
					      fallback_icon);
	gtk_widget_class_bind_template_child (widget_class, GsImageTile,
					      image_box);
}

GtkWidget *
gs_image_tile_new (GsApp *app)
{
	GsImageTile *tile;

	tile = g_object_new (GS_TYPE_IMAGE_TILE, NULL);
	gs_app_tile_set_app (GS_APP_TILE (tile), app);

	return GTK_WIDGET (tile);
}

/* vim: set noexpandtab: */
