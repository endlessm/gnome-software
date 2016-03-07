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

static void
on_eam_proxy_name_owner_changed (GDBusProxy *proxy,
                                 GParamSpec *pspec,
                                 gpointer user_data)
{
  EosAppManager **proxy_ptr = user_data;
  char *name_owner = g_dbus_proxy_get_name_owner (proxy);

  /* Whenever eam goes away, we invalidate our static proxy,
   * otherwise calls that would read cached properties will all
   * return NULL.
   */
  if (name_owner == NULL)
    g_clear_object (proxy_ptr);

  g_free (name_owner);
}

static EosAppManager *
eos_get_eam_dbus_proxy (void)
{
  static EosAppManager *proxy = NULL;
  GError *error = NULL;

  g_debug ("Getting EAM dbus proxy");

  /* If we already have a proxy, return it */
  if (proxy != NULL)
    return proxy;

  /* Otherwise create it */
  g_debug ("No EAM dbus proxy object yet - creating it");

  proxy = eos_app_manager_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                                  G_DBUS_PROXY_FLAGS_NONE,
                                                  "com.endlessm.AppManager",
                                                  "/com/endlessm/AppManager",
                                                  NULL, /* GCancellable* */
                                                  &error);
  if (error != NULL)
    {
      g_warning ("Unable to create dbus proxy: %s", error->message);
      g_error_free (error);
      return NULL;
    }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (proxy), G_MAXINT);
  g_signal_connect (proxy, "notify::g-name-owner",
                    G_CALLBACK (on_eam_proxy_name_owner_changed), &proxy);

  return proxy;
}

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
