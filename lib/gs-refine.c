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

#include "gs-refine.h"
#include "gnome-software-private.h"

static GsPluginRefineFlags
gs_refine_flag_from_string (const gchar *flag, GError **error)
{
	if (g_strcmp0 (flag, "all") == 0)
		return G_MAXINT32;
	if (g_strcmp0 (flag, "license") == 0)
		return GS_PLUGIN_REFINE_FLAGS_REQUIRE_LICENSE;
	if (g_strcmp0 (flag, "url") == 0)
		return GS_PLUGIN_REFINE_FLAGS_REQUIRE_URL;
	if (g_strcmp0 (flag, "description") == 0)
		return GS_PLUGIN_REFINE_FLAGS_REQUIRE_DESCRIPTION;
	if (g_strcmp0 (flag, "size") == 0)
		return GS_PLUGIN_REFINE_FLAGS_REQUIRE_SIZE;
	if (g_strcmp0 (flag, "rating") == 0)
		return GS_PLUGIN_REFINE_FLAGS_REQUIRE_RATING;
	if (g_strcmp0 (flag, "version") == 0)
		return GS_PLUGIN_REFINE_FLAGS_REQUIRE_VERSION;
	if (g_strcmp0 (flag, "history") == 0)
		return GS_PLUGIN_REFINE_FLAGS_REQUIRE_HISTORY;
	if (g_strcmp0 (flag, "setup-action") == 0)
		return GS_PLUGIN_REFINE_FLAGS_REQUIRE_SETUP_ACTION;
	if (g_strcmp0 (flag, "update-details") == 0)
		return GS_PLUGIN_REFINE_FLAGS_REQUIRE_UPDATE_DETAILS;
	if (g_strcmp0 (flag, "origin") == 0)
		return GS_PLUGIN_REFINE_FLAGS_REQUIRE_ORIGIN;
	if (g_strcmp0 (flag, "related") == 0)
		return GS_PLUGIN_REFINE_FLAGS_REQUIRE_RELATED;
	if (g_strcmp0 (flag, "menu-path") == 0)
		return GS_PLUGIN_REFINE_FLAGS_REQUIRE_MENU_PATH;
	if (g_strcmp0 (flag, "upgrade-removed") == 0)
		return GS_PLUGIN_REFINE_FLAGS_REQUIRE_UPGRADE_REMOVED;
	if (g_strcmp0 (flag, "provenance") == 0)
		return GS_PLUGIN_REFINE_FLAGS_REQUIRE_PROVENANCE;
	if (g_strcmp0 (flag, "reviews") == 0)
		return GS_PLUGIN_REFINE_FLAGS_REQUIRE_REVIEWS;
	if (g_strcmp0 (flag, "review-ratings") == 0)
		return GS_PLUGIN_REFINE_FLAGS_REQUIRE_REVIEW_RATINGS;
	if (g_strcmp0 (flag, "key-colors") == 0)
		return GS_PLUGIN_REFINE_FLAGS_REQUIRE_KEY_COLORS;
	if (g_strcmp0 (flag, "icon") == 0)
		return GS_PLUGIN_REFINE_FLAGS_REQUIRE_ICON;
	if (g_strcmp0 (flag, "permissions") == 0)
		return GS_PLUGIN_REFINE_FLAGS_REQUIRE_PERMISSIONS;
	if (g_strcmp0 (flag, "origin-hostname") == 0)
		return GS_PLUGIN_REFINE_FLAGS_REQUIRE_ORIGIN_HOSTNAME;
	if (g_strcmp0 (flag, "origin-ui") == 0)
		return GS_PLUGIN_REFINE_FLAGS_REQUIRE_ORIGIN_UI;
	if (g_strcmp0 (flag, "runtime") == 0)
		return GS_PLUGIN_REFINE_FLAGS_REQUIRE_RUNTIME;
	g_set_error (error,
		     GS_PLUGIN_ERROR,
		     GS_PLUGIN_ERROR_NOT_SUPPORTED,
		     "GsPluginRefineFlag '%s' not recognised", flag);
	return 0;
}

guint64
gs_parse_refine_flags (const gchar *extra, GError **error)
{
	GsPluginRefineFlags tmp;
	guint i;
	guint64 refine_flags = GS_PLUGIN_REFINE_FLAGS_DEFAULT;
	g_auto(GStrv) split = NULL;

	if (extra == NULL)
		return GS_PLUGIN_REFINE_FLAGS_DEFAULT;

	split = g_strsplit (extra, ",", -1);
	for (i = 0; split[i] != NULL; i++) {
		tmp = gs_refine_flag_from_string (split[i], error);
		if (tmp == 0)
			return G_MAXUINT64;
		refine_flags |= tmp;
	}
	return refine_flags;
}
