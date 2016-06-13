/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * gs-plugin-external-runtimes-cleaner: This plugin handles the removal of no
 * longer needed external apps' "external runtimes".
 * It should be removed once the transition path to the Flatpak implementation
 * of external apps is complete.
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

struct GsPluginData {
	FlatpakInstallation	*installation;
};

void
gs_plugin_initialize (GsPlugin *plugin)
{
	GsPluginData *priv = gs_plugin_alloc_data (plugin, sizeof(GsPluginData));

	/* Run plugin after the flatpak plugin because we need to complement its
	 * update/removal implementations */
	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_AFTER, "flatpak");
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

static void
command_cancelled_cb (GCancellable *cancellable,
		      GSubprocess *subprocess)
{
	g_debug ("Killing process '%s' after a cancellation!",
		 g_subprocess_get_identifier (subprocess));
	g_subprocess_force_exit (subprocess);
}

static gboolean
run_command (const char **argv,
	     GCancellable *cancellable,
	     GError **error)
{
	g_autofree char *cmd = NULL;
	int exit_val = -1;
	g_autoptr(GSubprocess) subprocess = NULL;
	gboolean result = FALSE;

	subprocess = g_subprocess_newv (argv,
					G_SUBPROCESS_FLAGS_STDOUT_PIPE |
					G_SUBPROCESS_FLAGS_STDIN_PIPE,
					error);

	if (!subprocess)
		return FALSE;

	if (cancellable)
		g_cancellable_connect (cancellable,
				       G_CALLBACK (command_cancelled_cb),
				       subprocess, NULL);

	result = g_subprocess_wait_check (subprocess, NULL, error);
	exit_val = g_subprocess_get_exit_status (subprocess);
	cmd = g_strjoinv (" ", (char **) argv);

	g_debug ("Result of running '%s': retcode=%d", cmd, exit_val);

	return result;
}

static gboolean
remove_legacy_ext_runtime (GsApp *app,
			   const char *version,
			   GCancellable *cancellable,
			   GError **error)
{
	g_autofree char *name = g_strdup_printf ("%s.external",
						 gs_app_get_flatpak_name (app));

	/* run the external apps removal script as the configured helper user */
	const char *argv[] = {"flatpak", "uninstall", "--runtime",
			      name, version,
			      NULL};

	g_debug ("Removing runtime extension '%s' with branch '%s'...", name,
		 version);

	return run_command (argv, cancellable, error);
}

gboolean
gs_plugin_app_remove (GsPlugin *plugin,
		      GsApp *app,
		      GCancellable *cancellable,
		      GError **error)
{
	const char *version;
	g_autoptr(GError) local_error = NULL;

	/* only try to remove runtimes if the app has been or is being updated;
	 * this prevents any removal if the app update has failed */
	if (gs_app_is_installed (app) &&
	    gs_app_get_state (app) != AS_APP_STATE_REMOVING)
		return TRUE;

	/* if the external runtime version is not installed, it is not a
	 * legacy external app */
	version = gs_app_get_metadata_item (app,
					    LEGACY_RUNTIME_INSTALLED_MTD_KEY);
	if (!version)
		return TRUE;

	if (!remove_legacy_ext_runtime (app, version, cancellable,
					&local_error)) {
		g_debug ("Could not remove legacy external runtime for app "
			 "%s when updating it: %s", gs_app_get_unique_id (app),
			 local_error->message);
	}
	return TRUE;
}

static gboolean
app_is_new_external_app (GsPlugin *plugin,
			 GsApp *app,
			 GCancellable *cancellable)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	const char *deploy_dir;
	g_autoptr(FlatpakInstalledRef) ref = NULL;
	g_autofree char *extra_dir = NULL;
	g_autoptr(GError) error = NULL;

	ref = flatpak_installation_get_installed_ref (priv->installation,
						      FLATPAK_REF_KIND_APP,
						      gs_app_get_flatpak_name (app),
						      gs_app_get_flatpak_arch (app),
						      gs_app_get_flatpak_branch (app),
						      cancellable,
						      &error);

	if (!ref) {
		g_debug ("Failed to get ref for app '%s': %s",
			 gs_app_get_unique_id (app), error->message);
		return FALSE;
	}

	deploy_dir = flatpak_installed_ref_get_deploy_dir (ref);

	/* new external apps (implemented in Flatpak) have their external assets
	 * downloaded into an "extra" directory on install/update time; this
	 * proves that the app is a new external app */
	extra_dir = g_build_filename (deploy_dir, "files", "extra", NULL);
	return g_file_test (extra_dir, G_FILE_TEST_IS_DIR | G_FILE_TEST_EXISTS);
}

gboolean
gs_plugin_update_app (GsPlugin *plugin,
		      GsApp *app,
		      GCancellable *cancellable,
		      GError **error)
{
	const char *version;
	g_autoptr(GError) local_error = NULL;

	/* only try to remove runtimes if the app has been or is being updated;
	 * this prevents any removal if the app update has failed */
	if (!gs_app_is_installed (app) ||
	    gs_app_get_state (app) != AS_APP_STATE_INSTALLING)
		return TRUE;

	/* if the external runtime version is not installed, it is not a
	 * legacy external app */
	version = gs_app_get_metadata_item (app,
					    LEGACY_RUNTIME_INSTALLED_MTD_KEY);
	if (!version)
		return TRUE;

	/* making sure that the updated app is a new external app is another
	 * safety check to ensure we don't break apps for users */
	if (!app_is_new_external_app (plugin, app, cancellable)) {
		g_warning ("Will not remove external runtime after upgrading "
			   "app '%s': there is no 'extra' dir, so removing the "
			   "runtime could break the app for the user.",
			   gs_app_get_unique_id (app));
		return TRUE;
	}

	if (!remove_legacy_ext_runtime (app, version, cancellable,
					&local_error)) {
		g_debug ("Could not remove legacy external runtime for app "
			 "%s when updating it: %s", gs_app_get_unique_id (app),
			 local_error->message);
	}
	return TRUE;
}
