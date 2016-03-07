/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2011-2013 Richard Hughes <richard@hughsie.com>
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

#include <config.h>

#include <gs-plugin.h>

#include "eos-app-manager-service.h"

/*
 * SECTION:
 * Plugin to deal with EOS's legacy app bundles.
 *
 * Methods:     | Search, AddUpdates, AddInstalled, AddPopular
 * Refines:     | [id]->[name], [id]->[summary]
 */

/**
 * gs_plugin_get_name:
 */
const gchar *
gs_plugin_get_name (void)
{
  return "eos-legacy-bundles";
}

/**
 * gs_plugin_initialize:
 */
void
gs_plugin_initialize (GsPlugin *plugin)
{
}

/**
 * gs_plugin_add_updates:
 */
gboolean
gs_plugin_add_updates (GsPlugin *plugin,
                       GList **list,
                       GCancellable *cancellable,
                       GError **error)
{
  return TRUE;
}

/**
 * gs_plugin_add_installed:
 */
gboolean
gs_plugin_add_installed (GsPlugin *plugin,
                         GList **list,
                         GCancellable *cancellable,
                         GError **error)
{
  return TRUE;
}

/**
 * gs_plugin_add_popular:
 */
gboolean
gs_plugin_add_popular (GsPlugin *plugin,
                       GList **list,
                       GCancellable *cancellable,
                       GError **error)
{
  return TRUE;
}

/**
 * gs_plugin_refine:
 */
gboolean
gs_plugin_refine (GsPlugin *plugin,
                  GList **list,
                  GsPluginRefineFlags flags,
                  GCancellable *cancellable,
                  GError **error)
{
  return TRUE;
}

/**
 * gs_plugin_add_category_apps:
 */
gboolean
gs_plugin_add_category_apps (GsPlugin *plugin,
                             GsCategory *category,
                             GList **list,
                             GCancellable *cancellable,
                             GError **error)
{
  return TRUE;
}

/**
 * gs_plugin_add_distro_upgrades:
 */
gboolean
gs_plugin_add_distro_upgrades (GsPlugin *plugin,
                               GList **list,
                               GCancellable *cancellable,
                               GError **error)
{
  return TRUE;
}

/**
 * gs_plugin_review_submit:
 */
gboolean
gs_plugin_review_submit (GsPlugin *plugin,
                         GsApp *app,
                         GsReview *review,
                         GCancellable *cancellable,
                         GError **error)
{
  return TRUE;
}

/**
 * gs_plugin_review_report:
 */
gboolean
gs_plugin_review_report (GsPlugin *plugin,
                         GsApp *app,
                         GsReview *review,
                         GCancellable *cancellable,
                         GError **error)
{
  return TRUE;
}

/**
 * gs_plugin_review_upvote:
 */
gboolean
gs_plugin_review_upvote (GsPlugin *plugin,
                         GsApp *app,
                         GsReview *review,
                         GCancellable *cancellable,
                         GError **error)
{
  return TRUE;
}

/**
 * gs_plugin_review_downvote:
 */
gboolean
gs_plugin_review_downvote (GsPlugin *plugin,
                           GsApp *app,
                           GsReview *review,
                           GCancellable *cancellable,
                           GError **error)
{
  return TRUE;
}

/**
 * gs_plugin_review_remove:
 */
gboolean
gs_plugin_review_remove (GsPlugin *plugin,
                         GsApp *app,
                         GsReview *review,
                         GCancellable *cancellable,
                         GError **error)
{
  return TRUE;
}
