/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2013 Matthias Clasen <mclasen@redhat.com>
 * Copyright (C) 2016-2017 Endless Mobile, Inc.
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

#ifndef GS_SIDE_FILTER_ROW_H
#define GS_SIDE_FILTER_ROW_H

#include <gtk/gtk.h>

#include "gs-category.h"
#include "gs-shell.h"

G_BEGIN_DECLS

#define GS_TYPE_SIDE_FILTER_ROW (gs_side_filter_row_get_type ())

G_DECLARE_FINAL_TYPE (GsSideFilterRow, gs_side_filter_row, GS, SIDE_FILTER_ROW, GtkListBoxRow)

GtkWidget	*gs_side_filter_row_new			(GsCategory		*cat);
GsCategory	*gs_side_filter_row_get_category	(GsSideFilterRow	*row);
void		 gs_side_filter_row_set_category	(GsSideFilterRow	*row,
							 GsCategory		*cat);
void		 gs_side_filter_row_set_mode		(GsSideFilterRow	*row,
							 GsShellMode		mode);
GsShellMode	 gs_side_filter_row_get_mode		(GsSideFilterRow	*row);

G_END_DECLS

#endif /* GS_SIDE_FILTER_ROW_H */

/* vim: set noexpandtab: */
