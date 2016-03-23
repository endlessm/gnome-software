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

#include <gio/gdesktopappinfo.h>
#include <glib/gi18n.h>
#include <gs-plugin.h>

#include "eos-app-manager-service.h"

#define EOS_BUNDLE_KEY_FILE_GROUP "Bundle"
#define EOS_LEGACY_BUNDLES_PLUGIN_NAME "EosLegacyBundles"
#define EOS_LEGACY_BUNDLES_DESKTOP_INFO "desktop-info"

/*
 * SECTION:
 * Plugin to deal with EOS's legacy app bundles.
 *
 * Methods:     | Search, AddUpdates, AddInstalled, AddPopular
 * Refines:     | [id]->[name], [id]->[summary]
 */

struct GsPluginPrivate
{
  GDBusConnection *session_bus;
};

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
  plugin->priv = GS_PLUGIN_GET_PRIVATE (GsPluginPrivate);

  plugin->priv->session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
}

/**
 * gs_plugin_destroy:
 */
void
gs_plugin_destroy (GsPlugin *plugin)
{
  g_clear_object (&plugin->priv->session_bus);
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

static const char *
eos_get_bundles_dir (void)
{
  return eos_app_manager_get_applications_dir (eos_get_eam_dbus_proxy ());
}

static gboolean
is_app_id (const char *appid)
{
  guint i;
  int c;
  static const char alsoallowed[] = "_-+.";
  static const char *reserveddirs[] = { "bin", "games", "share", "lost+found", "xdg", };

  if (!appid || appid[0] == '\0')
    return FALSE;

  for (i = 0; i < G_N_ELEMENTS (reserveddirs); i++)
    {
      if (g_strcmp0 (appid, reserveddirs[i]) == 0)
        return FALSE;
    }

  if (!g_ascii_isalnum (appid[0]))
    return FALSE; /* must start with an alphanumeric character */

  while ((c = *appid++) != '\0')
    {
      if (!g_ascii_isalnum (c) && !strchr (alsoallowed, c))
        break;
    }

  if (!c)
    return TRUE;

  return FALSE;
}

static void
gs_plugin_set_categories_from_desktop_app_info (GsApp *app, GDesktopAppInfo *info)
{
  const gchar *categories;
  gchar **categories_list;
  gchar **categories_index;

  categories = g_desktop_app_info_get_categories (info);

  if (!categories)
    return;

  categories_list = g_strsplit (categories, ";", -1);
  for (categories_index = categories_list; *categories_index != NULL; ++categories_index)
    {
      if (strlen (*categories_index) > 0)
        gs_app_add_category(app, *categories_index);
    }

  g_strfreev (categories_list);
}

static void
gs_plugin_set_icon_from_app_info (GsApp *app, GAppInfo *app_info)
{
  GtkIconTheme *theme;
  GtkIconInfo *info = NULL;
  GdkPixbuf *pixbuf = NULL;
  GIcon *icon;
  GError *error = NULL;

  theme = gtk_icon_theme_get_default ();
  icon = g_app_info_get_icon (app_info);

  if (!icon)
    {
      g_warning ("Could not get icon from app info for app '%s'", gs_app_get_name (app));
      return;
    }

  info = gtk_icon_theme_lookup_by_gicon (theme, icon, 64, GTK_ICON_LOOKUP_GENERIC_FALLBACK);

  if (!info)
    {
      g_warning ("Could not get icon info from app info for app '%s'", gs_app_get_name (app));
      goto cleanup;
    }

  pixbuf = gtk_icon_info_load_icon (info, &error);

  if (error)
    {
      g_warning ("Could not load pixbuf for app '%s': %s",
                 gs_app_get_name (app),
                 error->message);
      g_error_free (error);
      goto cleanup;
    }

  gs_app_set_pixbuf (app, pixbuf);

  gs_plugin_set_categories_from_desktop_app_info (app, G_DESKTOP_APP_INFO (app_info));

cleanup:
  g_object_unref (info);
  g_object_unref (pixbuf);
}

static void
gs_plugin_set_app_info_from_bundle_info_file (GsApp *app, const gchar *info_file)
{
  GKeyFile *key_file;
  GError *error = NULL;
  gchar *value = NULL;

  key_file = g_key_file_new ();

  if (!g_key_file_load_from_file (key_file, info_file, G_KEY_FILE_NONE, &error))
    {
      g_warning ("Error loading info file '%s': %s", info_file, error->message);
      g_error_free (error);
      goto cleanup;
    }

  value = g_key_file_get_value (key_file, EOS_BUNDLE_KEY_FILE_GROUP, "version", NULL);
  if (value)
    {
      gs_app_set_version (app, value);
      g_free (value);
    }

  value = g_key_file_get_value (key_file, EOS_BUNDLE_KEY_FILE_GROUP, "homepage", NULL);
  if (value)
    {
      gs_app_set_url(app, AS_URL_KIND_HOMEPAGE, value);
      g_free (value);
    }

cleanup:
  g_key_file_free (key_file);
}

static void
gs_plugin_set_app_info_from_desktop_id (GsApp *app, const gchar *desktop_id)
{
  GAppInfo *app_info;

  app_info = G_APP_INFO (g_desktop_app_info_new (desktop_id));

  if (!app_info)
    {
      g_warning ("Could not load desktop file '%s'", desktop_id);
      return;
    }

  gs_app_set_name (app, GS_APP_QUALITY_NORMAL, g_app_info_get_display_name (app_info));
  gs_app_set_summary (app, GS_APP_QUALITY_NORMAL, g_app_info_get_description (app_info));
  gs_app_set_description (app, GS_APP_QUALITY_NORMAL, g_app_info_get_description (app_info));
  gs_app_set_metadata (app, EOS_LEGACY_BUNDLES_DESKTOP_INFO, desktop_id);

  gs_plugin_set_icon_from_app_info (app, app_info);
  gs_plugin_set_categories_from_desktop_app_info (app, G_DESKTOP_APP_INFO (app_info));
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
  gint64 start_time;
  const char *prefix = eos_get_bundles_dir ();
  GDir *dir = g_dir_open (prefix, 0, error);
  int n_bundles;
  const char *appid;

  if (!dir)
    {
      g_warning ("Unable to open '%s': %s", prefix, (*error)->message);
      g_error_free (*error);
      return FALSE;
    }

  /* update UI as this might take some time */
  gs_plugin_status_update (plugin, NULL, GS_PLUGIN_STATUS_WAITING);

  start_time = g_get_monotonic_time ();

  n_bundles = 0;
  while ((appid = g_dir_read_name (dir)) != NULL)
    {
      char *info_path;
      char *desktop_id;
      g_autoptr(GsApp) app = NULL;

      if (g_cancellable_is_cancelled (cancellable))
        break;

      if (!is_app_id (appid))
        {
          g_debug ("Skipping '%s/%s': not a valid app directory", prefix, appid);
          continue;
        }

      info_path = g_build_filename (prefix, appid, ".info", NULL);
      g_debug ("Loading bundle info for '%s' from '%s'...\n", appid, info_path);

      desktop_id = g_strconcat (appid, ".desktop", NULL);

      app = gs_app_new (appid);
      gs_app_set_management_plugin (app, EOS_LEGACY_BUNDLES_PLUGIN_NAME);
      gs_app_set_id (app, appid);
      gs_app_set_state (app, AS_APP_STATE_INSTALLED);
      gs_app_set_kind (app, AS_APP_KIND_DESKTOP);
      gs_plugin_set_app_info_from_desktop_id (app, desktop_id);
      gs_plugin_set_app_info_from_bundle_info_file (app, info_path);
      gs_plugin_add_app (list, app);

      g_free (desktop_id);
      g_free (info_path);
    }

  g_dir_close (dir);

  g_debug ("Bundle loading from '%s': %d bundles, %.3f msecs",
           prefix,
           n_bundles,
           (double) (g_get_monotonic_time () - start_time) / 1000);

  return TRUE;
}

/**
 * gs_plugin_launch:
 */
gboolean
gs_plugin_launch (GsPlugin *plugin,
                  GsApp *app,
                  GCancellable *cancellable,
                  GError **error)
{
  GError *err = NULL;
  gboolean retval = FALSE;
  const char *desktop_info;
  GVariant *res;

  /* only process this app if was created by this plugin */
  if (g_strcmp0 (gs_app_get_management_plugin (app), EOS_LEGACY_BUNDLES_PLUGIN_NAME) != 0)
    return TRUE;

  desktop_info = gs_app_get_metadata_item (app, EOS_LEGACY_BUNDLES_DESKTOP_INFO);

  g_assert (desktop_info != NULL);

  res = g_dbus_connection_call_sync (plugin->priv->session_bus,
                                     "org.gnome.Shell",
                                     "/org/gnome/Shell",
                                     "org.gnome.Shell.AppLauncher", "Launch",
                                     g_variant_new ("(su)",
                                                    desktop_info,
                                                    0),
                                     G_VARIANT_TYPE ("(b)"),
                                     G_DBUS_CALL_FLAGS_NONE,
                                     -1,
                                     NULL,
                                     &err);

  if (err != NULL)
    {
      g_critical ("Unable to launch application '%s': %s", desktop_info, err->message);
      g_propagate_error (error, err);

      retval = FALSE;
    }

  if (res != NULL)
    {
      g_variant_get (res, "(b)", &retval);
      g_variant_unref (res);
    }

  return retval;
}

static gboolean
remove_app_from_shell (GsPlugin *plugin,
                       GsApp *app,
                       GCancellable *cancellable,
                       GError **error_out)
{
  GError *error = NULL;
  const char *desktop_info;

  desktop_info = gs_app_get_metadata_item (app, EOS_LEGACY_BUNDLES_DESKTOP_INFO);

  g_dbus_connection_call_sync (plugin->priv->session_bus,
                               "org.gnome.Shell",
                               "/org/gnome/Shell",
                               "org.gnome.Shell.AppStore", "RemoveApplication",
                               g_variant_new ("(s)", desktop_info),
                               NULL,
                               G_DBUS_CALL_FLAGS_NONE,
                               -1,
                               cancellable,
                               &error);

  if (error != NULL)
    {
      g_propagate_error (error_out, error);
      return FALSE;
    }

  return TRUE;
}

static gboolean
remove_app_from_manager (GsApp *app,
                         GCancellable *cancellable,
                         GError **error_out)
{
  GError *error = NULL;
  gboolean retval = FALSE;
  EosAppManager *proxy;
  const char *app_id = gs_app_get_id (app);

  /* We do a double check here, to catch the case where the app manager
   * proxy was successfully created, but the app bundles directory was
   * removed afterwards
   */
  proxy = eos_get_eam_dbus_proxy ();

  if (proxy == NULL)
    {
      g_warning ("Cannot get EAM proxy to remove '%s'", app_id);
      goto out;
    }

  if (!g_file_test (eos_get_bundles_dir (), G_FILE_TEST_EXISTS))
    {
      g_warning ("Bundles dir does not exist");
      goto out;
    }

  g_info ("Trying to uninstall %s", app_id);
  eos_app_manager_call_uninstall_sync (proxy, app_id, &retval, NULL, &error);

  if (error != NULL && g_dbus_error_is_remote_error (error))
    {
      char *code = g_dbus_error_get_remote_error (error);

      if (g_strcmp0 (code, "com.endlessm.AppManager.Error.NotAuthorized") == 0)
        g_warning ("Only admin can remove applications");

      g_free (code);
    }

out:
  if (retval)
    {
      g_info ("Uninstalling '%s' succeeded", app_id);
    }
  else if (error)
    {
      g_info ("Uninstalling '%s' failed: %s", app_id, error->message);
      g_propagate_error (error_out, error);
    }

  return retval;
}

/**
 * gs_plugin_app_remove:
 */
gboolean
gs_plugin_app_remove (GsPlugin *plugin,
                      GsApp *app,
                      GCancellable *cancellable,
                      GError **error)
{
  /* only process this app if was created by this plugin */
  if (g_strcmp0 (gs_app_get_management_plugin (app), EOS_LEGACY_BUNDLES_PLUGIN_NAME) != 0)
    return TRUE;

  /* remove */
  gs_app_set_state (app, AS_APP_STATE_REMOVING);

  if (!remove_app_from_manager (app, cancellable, error))
    {
      return FALSE;
    }

  g_debug ("Removing '%s' from shell", gs_app_get_id (app));

  if (!remove_app_from_shell (plugin, app, cancellable, error))
    {
      if (error)
        g_debug ("Failed to remove '%s' from shell: %s", gs_app_get_id (app), (*error)->message);

      return FALSE;
    }

  return TRUE;
}
