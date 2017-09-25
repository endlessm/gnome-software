/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2013 Matthias Clasen <mclasen@redhat.com>
 * Copyright (C) 2017 Joaquim Rocha <jrocha@endlessm.com>
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

#ifndef GS_BACKGROUND_TILE_H
#define GS_BACKGROUND_TILE_H

#include "gs-app-tile.h"

G_BEGIN_DECLS

#define GS_TYPE_BACKGROUND_TILE (gs_background_tile_get_type ())

G_DECLARE_FINAL_TYPE (GsBackgroundTile, gs_background_tile, GS, BACKGROUND_TILE, GsAppTile)

GtkWidget	*gs_background_tile_new			(GsApp		*app);

G_END_DECLS

#endif /* GS_BACKGROUND_TILE_H */

/* vim: set noexpandtab: */
