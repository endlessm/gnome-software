/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2016 Endless Mobile, Inc
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <config.h>

#include <gio/gdesktopappinfo.h>
#include <gnome-software.h>
#include <glib/gi18n.h>
#include <gs-plugin.h>

/*
 * SECTION:
 * Plugin to improve GNOME Software integration in the EOS desktop.
 */

struct GsPluginData
{
	GDBusConnection *session_bus;
	GHashTable *desktop_apps;
	int applications_changed_id;
};

static GHashTable *
get_applications_with_shortcuts (GsPlugin	*self,
				 GCancellable	*cancellable,
				 GError		**error);

static void
on_desktop_apps_changed (GDBusConnection *connection,
			 const gchar	 *sender_name,
			 const gchar	 *object_path,
			 const gchar	 *interface_name,
			 const gchar	 *signal_name,
			 GVariant	 *parameters,
			 GsPlugin	 *plugin)
{
	GHashTable *apps;
	GHashTableIter iter;
	gpointer key, value;
	GsPluginData *priv = gs_plugin_get_data (plugin);
	apps = get_applications_with_shortcuts (plugin, NULL, NULL);

	g_hash_table_iter_init (&iter, priv->desktop_apps);
	while (g_hash_table_iter_next (&iter, &key, &value)) {
		if (!g_hash_table_lookup (apps, key)) {
			GsApp *app = gs_plugin_cache_lookup (plugin, key);
			if (app) {
				gs_app_remove_quirk (app,
						     AS_APP_QUIRK_HAS_SHORTCUT);
			}

			g_hash_table_remove (priv->desktop_apps, key);
		} else {
			g_hash_table_add (priv->desktop_apps,
					  g_strdup (key));
		}
	}

	g_hash_table_destroy (apps);
}

/**
 * gs_plugin_initialize:
 */
void
gs_plugin_initialize (GsPlugin *plugin)
{
	GsPluginData *priv = gs_plugin_alloc_data (plugin,
						   sizeof(GsPluginData));
	priv->session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
	priv->desktop_apps = g_hash_table_new_full (g_str_hash, g_str_equal,
						    g_free, NULL);
	priv->applications_changed_id =
		g_dbus_connection_signal_subscribe (priv->session_bus,
						    "org.gnome.Shell",
						    "org.gnome.Shell.AppStore",
						    "ApplicationsChanged",
						    "/org/gnome/Shell",
						    NULL,
						    G_DBUS_SIGNAL_FLAGS_NONE,
						    (GDBusSignalCallback) on_desktop_apps_changed,
						    plugin, NULL);
}

/**
 * gs_plugin_destroy:
 */
void
gs_plugin_destroy (GsPlugin *plugin)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);

	if (priv->applications_changed_id != 0) {
		g_dbus_connection_signal_unsubscribe (priv->session_bus,
						      priv->applications_changed_id);
		priv->applications_changed_id = 0;
	}

	g_clear_object (&priv->session_bus);
	g_hash_table_destroy (priv->desktop_apps);
}

static GHashTable *
get_applications_with_shortcuts (GsPlugin	*plugin,
				 GCancellable	*cancellable,
				 GError		**error_out) {
	g_autoptr (GVariantIter) iter;
	g_autoptr (GVariant) apps;
	GError *error = NULL;
	gchar *application;
	GHashTable *apps_table;
	GsPluginData *priv = gs_plugin_get_data (plugin);

	apps_table = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
					    NULL);

	apps = g_dbus_connection_call_sync (priv->session_bus,
					    "org.gnome.Shell",
					    "/org/gnome/Shell",
					    "org.gnome.Shell.AppStore",
					    "ListApplications",
					    NULL, NULL,
					    G_DBUS_CALL_FLAGS_NONE,
					    -1,
					    cancellable,
					    &error);
	if (error != NULL) {
		g_critical ("Unable to list available applications: %s",
			    error->message);
		g_propagate_error (error_out, error);
		return NULL;
	}

	g_variant_get (apps, "(as)", &iter);

	while (g_variant_iter_loop (iter, "s", &application))
		g_hash_table_add (apps_table, g_strdup (application));

	return apps_table;
}

static gboolean
gs_plugin_eos_blacklist_if_needed (GsApp *app)
{
	gboolean blacklist_app = FALSE;

	blacklist_app = gs_app_get_kind (app) != AS_APP_KIND_DESKTOP &&
			gs_app_has_quirk (app, AS_APP_QUIRK_COMPULSORY);

	if (!blacklist_app &&
	    g_str_has_prefix (gs_app_get_id_no_prefix (app), "eos-link-")) {
		blacklist_app = TRUE;
	}

	if (blacklist_app)
		gs_app_add_category (app, "Blacklisted");

	return blacklist_app;
}

static void
gs_plugin_eos_update_app_shortcuts_info (GsPlugin *plugin,
					 GsApp *app,
					 GHashTable *apps_with_shortcuts)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	const char *app_id = gs_app_get_id_no_prefix (app);

	gs_plugin_cache_add (plugin, app_id, app);
	if (g_hash_table_lookup (apps_with_shortcuts, app_id)) {
		g_hash_table_add (priv->desktop_apps, g_strdup (app_id));
		gs_app_add_quirk (app, AS_APP_QUIRK_HAS_SHORTCUT);
	} else {
		g_hash_table_remove (priv->desktop_apps, app_id);
		gs_app_remove_quirk (app, AS_APP_QUIRK_HAS_SHORTCUT);
	}
}

static gboolean
gs_plugin_eos_app_is_flatpak (GsApp *app)
{
	const char *app_manager;
	app_manager = gs_app_get_management_plugin (app);

	return g_strcmp0 (app_manager, "flatpak") == 0;
}

static void
gs_plugin_eos_refine_core_app (GsApp *app)
{
	/* we only allow to remove flatpak apps */
	if (!gs_plugin_eos_app_is_flatpak (app)) {
		gs_app_add_quirk (app, AS_APP_QUIRK_COMPULSORY);
	}
}

/**
 * gs_plugin_refine:
 */
gboolean
gs_plugin_refine (GsPlugin		*plugin,
		  GsAppList		*list,
		  GsPluginRefineFlags	flags,
		  GCancellable		*cancellable,
		  GError		**error)
{
	guint i;
	GHashTable *apps;
	GsPluginData *priv = gs_plugin_get_data (plugin);

	g_hash_table_remove_all (priv->desktop_apps);
	apps = get_applications_with_shortcuts (plugin, cancellable, NULL);

	for (i = 0; i < gs_app_list_length (list); ++i) {
		GsApp *app = gs_app_list_index (list, i);

		gs_plugin_eos_refine_core_app (app);

		if (gs_plugin_eos_blacklist_if_needed (app))
			continue;

		if (gs_app_get_kind (app) != AS_APP_KIND_DESKTOP)
			continue;

		gs_plugin_eos_update_app_shortcuts_info (plugin, app, apps);
	}

	g_hash_table_destroy (apps);

	return TRUE;
}

static gboolean
remove_app_from_shell (GsPlugin		*plugin,
		       GsApp		*app,
		       GCancellable	*cancellable,
		       GError		**error_out)
{
	GError *error = NULL;
	GsPluginData *priv = gs_plugin_get_data (plugin);
	const char *app_id = gs_app_get_id_no_prefix (app);

	g_dbus_connection_call_sync (priv->session_bus,
				     "org.gnome.Shell",
				     "/org/gnome/Shell",
				     "org.gnome.Shell.AppStore",
				     "RemoveApplication",
				     g_variant_new ("(s)", app_id),
				     NULL,
				     G_DBUS_CALL_FLAGS_NONE,
				     -1,
				     cancellable,
				     &error);

	if (error != NULL) {
		g_propagate_error (error_out, error);
		return FALSE;
	}

	return TRUE;
}

static gboolean
add_app_to_shell (GsPlugin	*plugin,
		  GsApp		*app,
		  GCancellable	*cancellable,
		  GError	**error_out)
{
	GError *error = NULL;
	GsPluginData *priv = gs_plugin_get_data (plugin);
	const char *app_id = gs_app_get_id_no_prefix (app);

	g_dbus_connection_call_sync (priv->session_bus,
				     "org.gnome.Shell",
				     "/org/gnome/Shell",
				     "org.gnome.Shell.AppStore",
				     "AddApplication",
				     g_variant_new ("(s)", app_id),
				     NULL,
				     G_DBUS_CALL_FLAGS_NONE,
				     -1,
				     NULL,
				     &error);

	if (error != NULL) {
		g_debug ("%s", error->message);
		g_propagate_error (error_out, error);
		return FALSE;
	}

	return TRUE;
}

/**
 * gs_plugin_add_shortcut:
 */
gboolean
gs_plugin_add_shortcut (GsPlugin	*plugin,
			GsApp		*app,
			GCancellable	*cancellable,
			GError		**error)
{
	gs_app_add_quirk (app, AS_APP_QUIRK_HAS_SHORTCUT);
	return add_app_to_shell (plugin, app, cancellable, error);
}

/**
 * gs_plugin_remove_shortcut:
 */
gboolean
gs_plugin_remove_shortcut (GsPlugin	*plugin,
			   GsApp	*app,
			   GCancellable	*cancellable,
			   GError	**error)
{
	gs_app_remove_quirk (app, AS_APP_QUIRK_HAS_SHORTCUT);
	return remove_app_from_shell (plugin, app, cancellable, error);
}
