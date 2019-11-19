/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2013 Matthias Clasen <mclasen@redhat.com>
 * Copyright (C) 2016-2018 Endless Mobile, Inc.
 *
 * Authors:
 *    Joaquim Rocha <jrocha@endlessm.com>
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

#include "gs-side-filter-row.h"
#include "gs-common.h"
#include "gs-css.h"

struct _GsSideFilterRow
{
	GtkListBoxRow	parent_instance;

	GsCategory	*cat;
	GsShellMode	 mode;
	GtkWidget	*label;
	GtkWidget	*image;
	GtkWidget	*leftborder;
};

G_DEFINE_TYPE (GsSideFilterRow, gs_side_filter_row, GTK_TYPE_LIST_BOX_ROW)

GsCategory *
gs_side_filter_row_get_category (GsSideFilterRow *row)
{
	g_return_val_if_fail (GS_IS_SIDE_FILTER_ROW (row), NULL);

	return row->cat;
}

void
gs_side_filter_row_set_category (GsSideFilterRow *row, GsCategory *cat)
{
	GPtrArray *key_colors;

	g_return_if_fail (GS_IS_SIDE_FILTER_ROW (row));
	g_return_if_fail (GS_IS_CATEGORY (cat));

	g_set_object (&row->cat, cat);

	gtk_label_set_label (GTK_LABEL (row->label), gs_category_get_name (cat));
	gtk_image_set_from_icon_name (GTK_IMAGE (row->image),
				      gs_category_get_icon (cat),
				      GTK_ICON_SIZE_LARGE_TOOLBAR);

	/* set custom CSS for the colored border */
	key_colors = gs_category_get_key_colors (cat);
	if (key_colors->len > 0) {
		GdkRGBA *tmp = g_ptr_array_index (key_colors, 0);
		g_autofree gchar *css = NULL;
		g_autofree gchar *class_name = NULL;
		g_autofree gchar *color = gdk_rgba_to_string (tmp);
		css = g_strdup_printf ("background-color: %s", color);
		class_name = g_strdup_printf ("side-filter-row-custom-%p", row);
		gs_utils_widget_set_css (GTK_WIDGET (row->leftborder), class_name, css);
	}

	gs_side_filter_row_set_mode (row, GS_SHELL_MODE_CATEGORY);
}

static void
gs_side_filter_row_destroy (GtkWidget *widget)
{
	GsSideFilterRow *row = GS_SIDE_FILTER_ROW (widget);

	g_clear_object (&row->cat);

	GTK_WIDGET_CLASS (gs_side_filter_row_parent_class)->destroy (widget);
}

static void
gs_side_filter_row_init (GsSideFilterRow *row)
{
	gtk_widget_set_has_window (GTK_WIDGET (row), FALSE);
	gtk_widget_init_template (GTK_WIDGET (row));
	row->mode = GS_SHELL_MODE_UNKNOWN;
}

static void
gs_side_filter_row_class_init (GsSideFilterRowClass *klass)
{
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

	widget_class->destroy = gs_side_filter_row_destroy;

	gtk_widget_class_set_template_from_resource (widget_class,
						     "/org/gnome/Software/"
						     "gs-side-filter-row.ui");

	gtk_widget_class_bind_template_child (widget_class, GsSideFilterRow,
					      label);
	gtk_widget_class_bind_template_child (widget_class, GsSideFilterRow,
					      image);
	gtk_widget_class_bind_template_child (widget_class, GsSideFilterRow,
					      leftborder);
}

GtkWidget *
gs_side_filter_row_new (GsCategory *cat)
{
	GsSideFilterRow *row;

	row = g_object_new (GS_TYPE_SIDE_FILTER_ROW, NULL);
	gs_side_filter_row_set_category (row, cat);

	return GTK_WIDGET (row);
}

void
gs_side_filter_row_set_mode (GsSideFilterRow *row,
			     const GsShellMode mode)
{
	g_return_if_fail (GS_IS_SIDE_FILTER_ROW (row));
	row->mode = mode;
}

GsShellMode
gs_side_filter_row_get_mode (GsSideFilterRow *row)
{
	g_return_val_if_fail (GS_IS_SIDE_FILTER_ROW (row),
			      GS_SHELL_MODE_UNKNOWN);
	return row->mode;
}

/* vim: set noexpandtab: */
