/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2016 Joaquim Rocha <jrocha@endlessm.com>
 * Copyright (C) 2016 Mario Sanchez Prada <mario@endlessm.com>
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

/* Notes:
 *
 * All GsApp's created have management-plugin set to flatpak
 * Some GsApp's created have have flatpak::kind of app or runtime
 * The GsApp:origin is the remote name, e.g. test-repo
 */

#include <config.h>

#include <flatpak.h>
#include <gnome-software.h>

#include "gs-appstream.h"
#include "gs-flatpak.h"

struct GsPluginData {
	GsFlatpak		*flatpak;
	GSettings		*settings;
};

static gboolean
gs_plugin_flatpak_custom_is_valid_path (gchar *path)
{
	g_autoptr(GFile) file = NULL;

	if (path == NULL || !g_path_is_absolute (path))
		return FALSE;

	file = g_file_new_for_path (path);
	if (!g_file_query_exists (file, NULL))
		return FALSE;

	return TRUE;
}

void
gs_plugin_initialize (GsPlugin *plugin)
{
	g_autofree gchar *custom_location = NULL;
	GsPluginData *priv = gs_plugin_alloc_data (plugin, sizeof(GsPluginData));

	priv->settings = g_settings_new ("org.gnome.software");
	custom_location = g_settings_get_string (priv->settings,
						 "install-bundles-custom-location");

	printf("[%s:%d] %s / system location: %s\n", __FILE__, __LINE__, __FUNCTION__,
	       custom_location == NULL ? "NULL" : custom_location);

	if (!gs_plugin_flatpak_custom_is_valid_path (custom_location)) {
		gs_plugin_set_enabled (plugin, FALSE);
		printf("[%s:%d] %s / system location: %s\n", __FILE__, __LINE__, __FUNCTION__,
		       custom_location == NULL ? "NULL" : custom_location);
		return;
	}
	
	priv->flatpak = gs_flatpak_new (plugin, GS_FLATPAK_SCOPE_CUSTOM);

	/* set plugin flags */
	gs_plugin_add_flags (plugin, GS_PLUGIN_FLAGS_GLOBAL_CACHE);

	/* getting app properties from appstream is quicker */
	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_AFTER, "appstream");

	/* prioritize over packages */
	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_BETTER_THAN, "packagekit");
}

void
gs_plugin_destroy (GsPlugin *plugin)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	g_clear_object (&priv->flatpak);
	g_clear_object (&priv->settings);
}

void
gs_plugin_adopt_app (GsPlugin *plugin, GsApp *app)
{
	if (gs_app_get_bundle_kind (app) == AS_BUNDLE_KIND_FLATPAK &&
	    gs_app_get_scope (app) == AS_APP_SCOPE_SYSTEM) {
		gs_app_set_management_plugin (app, gs_plugin_get_name (plugin));
	}
}

gboolean
gs_plugin_setup (GsPlugin *plugin, GCancellable *cancellable, GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	return gs_flatpak_setup (priv->flatpak, cancellable, error);
}

gboolean
gs_plugin_add_installed (GsPlugin *plugin,
			 GsAppList *list,
			 GCancellable *cancellable,
			 GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	return gs_flatpak_add_installed (priv->flatpak, list, cancellable, error);
}

gboolean
gs_plugin_add_sources (GsPlugin *plugin,
		       GsAppList *list,
		       GCancellable *cancellable,
		       GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	return gs_flatpak_add_sources (priv->flatpak, list, cancellable, error);
}

gboolean
gs_plugin_add_updates (GsPlugin *plugin,
		       GsAppList *list,
		       GCancellable *cancellable,
		       GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	return gs_flatpak_add_updates (priv->flatpak, list, cancellable, error);
}

gboolean
gs_plugin_refresh (GsPlugin *plugin,
		   guint cache_age,
		   GsPluginRefreshFlags flags,
		   GCancellable *cancellable,
		   GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	return gs_flatpak_refresh (priv->flatpak, cache_age, flags,
				   cancellable, error);
}

gboolean
gs_plugin_refine_app (GsPlugin *plugin,
		      GsApp *app,
		      GsPluginRefineFlags flags,
		      GCancellable *cancellable,
		      GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	return gs_flatpak_refine_app (priv->flatpak, app, flags,
				      cancellable, error);
}

gboolean
gs_plugin_launch (GsPlugin *plugin,
		  GsApp *app,
		  GCancellable *cancellable,
		  GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	return gs_flatpak_launch (priv->flatpak, app, cancellable, error);
}

gboolean
gs_plugin_app_remove (GsPlugin *plugin,
		      GsApp *app,
		      GCancellable *cancellable,
		      GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	return gs_flatpak_app_remove (priv->flatpak, app, cancellable, error);
}

gboolean
gs_plugin_app_install (GsPlugin *plugin,
		       GsApp *app,
		       GCancellable *cancellable,
		       GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	return gs_flatpak_app_install (priv->flatpak, app, cancellable, error);
}

gboolean
gs_plugin_update_app (GsPlugin *plugin,
		      GsApp *app,
		      GCancellable *cancellable,
		      GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	return gs_flatpak_update_app (priv->flatpak, app, cancellable, error);
}

gboolean
gs_plugin_file_to_app (GsPlugin *plugin,
		       GsAppList *list,
		       GFile *file,
		       GCancellable *cancellable,
		       GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	return gs_flatpak_file_to_app (priv->flatpak, list, file,
				       cancellable, error);
}
