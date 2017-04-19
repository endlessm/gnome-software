/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * gs-plugin-legacy-external-apps: This plugin handles the transition from
 * Endless' custom implementation of external apps to the Flatpak's one.
 * It should be removed once the transition path is complete for Endless OS
 * users.
 *
 * Copyright (C) 2016 Endless Mobile, Inc
 *
 * Author: Joaquim Rocha <jrocha@endlessm.com>
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
#include <flatpak.h>
#include <glib.h>
#include <gnome-software.h>
#include <string.h>

#include "gs-appstream.h"
#include "gs-flatpak.h"
#include "gs-legacy-external-apps.h"

#define LEGACY_RUNTIME_MTD_KEY "EndlessOS::legacy-ext-runtime"

static const char *LEGACY_EXTERNAL_APPS[] = {"com.dropbox.Client.desktop",
					     "com.google.Chrome.desktop",
					     "com.microsoft.Skype.desktop",
					     "com.spotify.Client.desktop",
					     NULL};

struct GsPluginData {
	FlatpakInstallation	*installation;
};

void
gs_plugin_initialize (GsPlugin *plugin)
{
	GsPluginData *priv = gs_plugin_alloc_data (plugin, sizeof(GsPluginData));

	/* Run plugin before the flatpak plugin because we need to setup the
	 * external runtime version in the apps before they are actually
	 * removed/updated */
	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_BEFORE, "flatpak");
}

gboolean
gs_plugin_setup (GsPlugin *plugin,
		 GCancellable *cancellable,
		 GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	priv->installation = flatpak_installation_new_system (cancellable,
							      error);

	if (!priv->installation)
		return FALSE;

	return TRUE;
}

void
gs_plugin_destroy (GsPlugin *plugin)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	g_object_unref (priv->installation);
}

static gboolean
app_is_legacy_external_app (GsApp *app)
{
	guint i;
	for (i = 0; LEGACY_EXTERNAL_APPS[i] != NULL; ++i) {
		const char *current_id = LEGACY_EXTERNAL_APPS[i];
		if (g_strcmp0 (gs_app_get_id (app), current_id) == 0)
			return TRUE;
	}

	return FALSE;
}

static AsApp *
get_installed_appstream_app (GsPlugin *plugin,
			     GsApp *app,
			     GCancellable *cancellable,
			     GError **error)
{
	AsApp *as_app;
	GsPluginData *priv = gs_plugin_get_data (plugin);
	const char *deploy_dir;
	g_autoptr(AsStore) store = NULL;
	g_autoptr(FlatpakInstalledRef) ref = NULL;
	g_autoptr(GFile) appstream_file = NULL;
	g_autofree char *appstream_xml = NULL;
	g_autofree char *appstream_path = NULL;

	ref = flatpak_installation_get_installed_ref (priv->installation,
						      FLATPAK_REF_KIND_APP,
						      gs_app_get_flatpak_name (app),
						      gs_app_get_flatpak_arch (app),
						      gs_app_get_flatpak_branch (app),
						      cancellable,
						      error);

	if (!ref)
		return NULL;

	deploy_dir = flatpak_installed_ref_get_deploy_dir (ref);

	appstream_xml = g_strdup_printf ("%s.appdata.xml",
					  gs_app_get_flatpak_name (app));
	appstream_path = g_build_filename (deploy_dir, "files", "share",
					   "app-info", "xmls", appstream_xml,
					   NULL);
	appstream_file = g_file_new_for_path (appstream_path);

	store = as_store_new ();
	as_store_set_add_flags (store,
				AS_STORE_ADD_FLAG_USE_UNIQUE_ID |
				AS_STORE_ADD_FLAG_USE_MERGE_HEURISTIC);
	if (!as_store_from_file (store, appstream_file, NULL, cancellable,
				 error))
		return NULL;

	as_app = as_store_get_app_by_id (store, gs_app_get_id (app));
	if (!as_app)
		g_set_error (error, AS_STORE_ERROR, AS_STORE_ERROR_FAILED,
			     "Failed to get app %s from its own installation "
			     "AppStream file.", gs_app_get_unique_id (app));

	return g_object_ref (as_app);
}

static void
setup_ext_runtime_version (GsPlugin *plugin,
			   GsApp *app,
			   GCancellable *cancellable)
{
	const char *runtime_version;
	g_autoptr(GError) local_error = NULL;
	g_autoptr(AsApp) as_app = NULL;

	if (!app_is_legacy_external_app (app))
		return;

	as_app = get_installed_appstream_app (plugin, app, cancellable,
					      &local_error);

	if (!as_app) {
		g_warning ("Failed to get AsApp from installed AppStream "
			   "data of app '%s': %s", gs_app_get_unique_id (app),
			   local_error->message);
		return;
	}

	/* get the runtime version that is set in the installed AppStream data */
	runtime_version = as_app_get_metadata_item (as_app,
						    LEGACY_RUNTIME_MTD_KEY);

	/* we setup the version of the external runtime used by this external
	 * app so it is later used by "external-apps-cleaner" plugin when
	 * removing those runtimes; we use a new key and not the one that is
	 * already set in the metadata so we verify that this key has been
	 * set by this plugin (and thus, by what was installed) and not by
	 * the general AppStream data */
	gs_app_set_metadata (app, LEGACY_RUNTIME_INSTALLED_MTD_KEY, NULL);
	gs_app_set_metadata (app, LEGACY_RUNTIME_INSTALLED_MTD_KEY,
			     runtime_version);

	return;
}

gboolean
gs_plugin_refine_app (GsPlugin *plugin,
		      GsApp *app,
		      GsPluginRefineFlags flags,
		      GCancellable *cancellable,
		      GError **error)
{
	/* we have to whitelist Chrome which has been blacklisted so previous
	 * versions of the OS (without the helper app) would not see it */
	if (g_strcmp0 (gs_app_get_id (app), "com.google.Chrome.desktop") == 0)
		gs_app_remove_category (app, "Blacklisted");
	return TRUE;
}

gboolean
gs_plugin_app_remove (GsPlugin *plugin,
		      GsApp *app,
		      GCancellable *cancellable,
		      GError **error)
{
	setup_ext_runtime_version (plugin, app, cancellable);
	return TRUE;
}

gboolean
gs_plugin_update_app (GsPlugin *plugin,
		      GsApp *app,
		      GCancellable *cancellable,
		      GError **error)
{
	setup_ext_runtime_version (plugin, app, cancellable);
	return TRUE;
}
