/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
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
#include <glib/gstdio.h>
#include <gnome-software.h>
#include <string.h>

#include "gs-flatpak.h"

#define METADATA_ASSET_URL "GnomeSoftware::external-app::url"
#define METADATA_HEADLESS_APP "GnomeSoftware::external-apps::headless-app"

struct GsPluginData {
	GsFlatpak	*flatpak;
	GHashTable	*external_runtimes;
};

void
gs_plugin_initialize (GsPlugin *plugin)
{
	GsPluginData *priv = gs_plugin_alloc_data (plugin, sizeof(GsPluginData));

	priv->flatpak = gs_flatpak_new (plugin, GS_FLATPAK_SCOPE_USER);
	priv->external_runtimes = g_hash_table_new_full (g_str_hash,
							 g_str_equal,
							 g_free,
							 (GDestroyNotify) g_object_unref);

	/* Run plugin after the flatpak ones because we need them to install
	 * the app's headless part first */
	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_BEFORE, "flatpak-system");
	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_BEFORE, "flatpak-user");
}

void
gs_plugin_destroy (GsPlugin *plugin)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	g_clear_object (&priv->external_runtimes);
}

static gboolean
gs_app_is_flatpak (const GsApp *app)
{
	const gchar *id = gs_app_get_id (app);
	return id != NULL &&
		(g_str_has_prefix (id, GS_FLATPAK_USER_PREFIX ":") ||
		 g_str_has_prefix (id, GS_FLATPAK_SYSTEM_PREFIX ":"));
}

void
gs_plugin_adopt_app (GsPlugin *plugin, GsApp *app)
{
	const gchar *id = gs_app_get_id (app);
	if (gs_app_is_flatpak (app)) {
		const gchar *metadata;
		metadata = gs_app_get_metadata_item (app,
						     "flatpack-3rdparty::external-assets");

		if (!metadata)
			return;

		g_debug ("Adopt '%s' as an external app", id);
		gs_app_set_management_plugin (app, gs_plugin_get_name (plugin));
	}
}


gboolean
gs_plugin_setup (GsPlugin *plugin, GCancellable *cancellable, GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	return gs_flatpak_setup (priv->flatpak, cancellable, error);
}

static char *
gs_plugin_download_asset (GsPlugin *plugin,
			  GsApp *app,
			  const char *runtime,
			  const char *asset,
			  GError **error)
{
	gsize data_len;
	g_autofree gchar *cache_basename = NULL;
	g_autofree gchar *cache_fn = NULL;
	g_autofree gchar *cache_png = NULL;
	g_autofree gchar *data = NULL;
	g_autoptr(AsIcon) icon = NULL;
	g_autoptr(GdkPixbuf) pb = NULL;

	/* download icons from the cdn */
	cache_basename = g_path_get_basename (asset);
	cache_fn = gs_utils_get_cache_filename (gs_plugin_get_name (plugin),
						cache_basename,
						GS_UTILS_CACHE_FLAG_NONE,
						error);
	if (cache_fn == NULL)
		return FALSE;
	if (g_file_test (cache_fn, G_FILE_TEST_EXISTS)) {
		if (!g_file_get_contents (cache_fn, &data, &data_len, error))
			return NULL;
	} else {
		if (!gs_mkdir_parent (cache_fn, error))
			return NULL;
		if (!gs_plugin_download_file (plugin,
					      app,
					      asset,
					      cache_fn,
					      NULL, /* GCancellable */
					      error))
			return NULL;
	}

	return g_strdup (cache_fn);
}

static gboolean
gs_plugin_run_command (const char *working_dir,
		       const char **argv,
		       GError **error)
{
	g_autofree char *stderr_buf = NULL;
	g_autofree char *stdout_buf = NULL;
	g_autofree char *cmd = NULL;
	int exit_val = -1;

	if (!g_spawn_sync (working_dir, (char **) argv, NULL, G_SPAWN_SEARCH_PATH, NULL,
			   NULL, &stdout_buf, &stderr_buf, &exit_val, error)) {
		return FALSE;
	}

	cmd = g_strjoinv (" ", (char **) argv);
	g_debug ("Result of running '%s': retcode=%d stdout='%s' stderr='%s'",
		 cmd, exit_val,	(stdout_buf ? stdout_buf : "NULL"),
		 (stderr_buf ? stderr_buf : "NULL"));

	return (exit_val == 0) ? TRUE : FALSE;
}

static gboolean
gs_plugin_xdg_app_repo_build_init (const char *repo_dir,
				   const char *repo_name,
				   GError **error)
{
	const char *endless_runtime = "com.endlessm.Platform";

	const char *argv[] = {"flatpak", "build-init",  repo_dir, repo_name,
			      endless_runtime, endless_runtime, NULL};

	if (!gs_plugin_run_command (NULL, argv, error))
		return FALSE;

	const char *argv2[] = {"sed", "-i", "s/Application/Runtime/",
			       "metadata", NULL};

	if (!gs_plugin_run_command (repo_dir, argv2, error))
		return FALSE;

	return TRUE;
}

static gboolean
gs_plugin_xdg_app_repo_build_export (const char *repo_dir,
				     const char *repo_name,
				     GError **error)
{
	const char *argv[] = {"flatpak", "build-export",  "--runtime",
			      repo_name, repo_dir, NULL};
	return gs_plugin_run_command (NULL, argv, error);
}

static gboolean
gs_plugin_xdg_app_remote_add (const char *repo_dir,
			      const char *repo_name,
			      GError **error)
{
	const char *argv[] = {"flatpak", "remote-add",  "--user",
			      "--no-gpg-verify", repo_name, repo_dir, NULL};
	return gs_plugin_run_command (NULL, argv, error);
}

static gboolean
gs_plugin_xdg_app_remote_delete (const char *repo_name,
				 GError **error)
{
	const char *argv[] = {"flatpak", "remote-delete",  "--user", "--force",
			      repo_name, NULL};
	return gs_plugin_run_command (NULL, argv, error);
}

static gboolean
gs_plugin_xdg_app_install_runtime (const char *repo_name,
				   const char *runtime,
				   GError **error)
{
	const char *argv[] = {"flatpak", "install",  "--user",
			      repo_name, runtime, NULL};
	return gs_plugin_run_command (NULL, argv, error);
}

static gboolean
gs_plugin_add_runtime_deb_asset (const gchar *build_dir,
				 const gchar *repo_dir,
				 const char *asset_path,
				 GError **error)
{
	const char *argv[] = {"ar", "x",  asset_path, NULL};
	if (!gs_plugin_run_command (build_dir, argv, error)) {
		return FALSE;
	}

	const char *argv2[] = {"tar", "xzf",  "data.tar.gz", "-C", repo_dir,
			       NULL};

	if (!gs_plugin_run_command (build_dir, argv2, error)) {
		return FALSE;
	}

	return TRUE;
}

static gboolean
gs_plugin_add_runtime_asset (GsPlugin *plugin,
			     GsApp *app,
			     const gchar *build_dir,
			     const gchar *repo_dir,
			     const char *runtime,
			     const char *asset,
			     GError **error)
{
	g_autofree gchar *content_type = NULL;
	g_autoptr(GFile) download_file = NULL;

	char *download_name = gs_plugin_download_asset (plugin, app, runtime,
							asset, error);

	if (!download_name) {
		return FALSE;
	}

	download_file = g_file_new_for_path (download_name);
	content_type = gs_utils_get_content_type (download_file, NULL,
						  error);

	if (!content_type) {
		return FALSE;
	}

	g_debug ("Adding runtime asset with type '%s'", content_type);

	if (g_strcmp0 (content_type, "application/x-deb") == 0) {
		gs_plugin_add_runtime_deb_asset (build_dir, repo_dir,
						 download_name, error);
	} else {
		g_debug ("Cannot deal with asset type '%s'",
			 content_type);

		return FALSE;
	}

	return TRUE;
}

static gboolean
gs_plugin_build_runtime (GsPlugin *plugin,
			 GsApp *app,
			 GsApp *runtime,
			 GError **error)
{
	gboolean ret;
	g_autofree char *tmp_dir;
	const char *template = "3rdparty_XXXXXX";
	const char *build_dir = "3rd_party_repo";
	g_autofree char *repo_dir = NULL;
	g_autofree char *tmp_build_dir = NULL;
	g_autofree char *repo_name = NULL;
	g_autofree char *dir_basename = NULL;
	const char *runtime_name = gs_app_get_id_no_prefix (runtime);
	const char *asset = gs_app_get_metadata_item (runtime,
						      METADATA_ASSET_URL);

	tmp_dir = g_dir_make_tmp (template, error);

	gs_app_set_progress (app, 5);

	if (!tmp_dir) {
		g_debug ("Could not create temporary dir for building a "
			 "runtime extension!");
		return FALSE;
	}

	g_debug ("Building runtime extension '%s' in dir '%s'", runtime_name,
		 tmp_dir);

	tmp_build_dir = g_strconcat (tmp_dir, "/", build_dir, NULL);
	if (g_mkdir_with_parents (tmp_build_dir, 0777) != 0) {
		g_debug ("Failed to make dir %s", tmp_build_dir);
		return FALSE;
	}

	gs_app_set_progress (app, 10);

	gs_plugin_xdg_app_repo_build_init (tmp_build_dir, runtime_name, error);

	gs_app_set_progress (app, 15);

	gs_plugin_add_runtime_asset (plugin, app, tmp_dir, tmp_build_dir,
				     runtime_name, asset, error);

	repo_dir = g_strconcat (tmp_dir, "/", runtime_name, NULL);

	gs_app_set_progress (app, 30);

	g_debug ("Exporting repo in '%s'... (this may take a while)",
		 tmp_build_dir);
	ret = gs_plugin_xdg_app_repo_build_export (tmp_build_dir, repo_dir,
						   error);

	gs_app_set_progress (app, 35);

	if (!ret) {
		g_debug ("Failed to export repo '%s' in '%s'!",
			 repo_dir, tmp_build_dir);
		if (*error) {
			g_debug ("Error: %s", (*error)->message);
		}
		return FALSE;
	}

	g_debug ("Repo '%s' exported!", repo_dir);

	/* Use the temp dir random name as a prefix for the runtime. This avoids
	 * any clashes with existing repos and makes cleaning up easier. */
	dir_basename = g_path_get_basename (tmp_dir);
	repo_name = g_strconcat (dir_basename, runtime_name, NULL);

	gs_app_set_progress (app, 50);

	g_debug ("Adding remote!");
	ret = gs_plugin_xdg_app_remote_add (repo_dir, repo_name, error);
	if (!ret) {
		if (*error) {
			g_debug ("Error: %s", (*error)->message);
		}
		return FALSE;
	}

	gs_app_set_origin (runtime, repo_name);
	gs_app_set_state (runtime, AS_APP_STATE_AVAILABLE);

	gs_app_set_progress (app, 70);

	/* ret = gs_plugin_xdg_app_install_runtime (repo_name, runtime_name, error); */
	/* if (!ret) { */
	/* 	if (*error) { */
	/* 		g_debug ("Error: %s", (*error)->message); */
	/* 	} */
	/* 	return FALSE; */
	/* } */

	/* g_debug ("Runtime extension '%s' successfully installed!", runtime_name); */

	/* gs_app_set_progress (app, 90); */

	/* ret = gs_plugin_xdg_app_remote_delete (repo_name, error); */
	/* if (!ret) { */
	/* 	if (*error) { */
	/* 		g_debug ("Error: %s", (*error)->message); */
	/* 	} */
	/* 	return FALSE; */
	/* } */


	return TRUE;
}

static GsApp *
gs_plugin_get_app_external_asset (GsPlugin *plugin,
				  GsApp *headless_app)
{
	GsApp *app = NULL;
	GsPluginData *priv;
	g_autofree char *id = NULL;
	g_autofree char *url = NULL;
	g_autofree char *full_id = NULL;
	guint i;
	const char *metadata;

	metadata = gs_app_get_metadata_item (headless_app,
					     "flatpack-3rdparty::external-assets");

	for (i = 0; i < strlen (metadata); ++i) {
		if (metadata[i] == ':') {
			id = g_strndup (metadata, i);
			url = g_strdup (metadata + i + 1);
			break;
		}
	}

	if (!id || !url) {
		g_debug ("No id or url found in metadata: %s", metadata);
		return NULL;
	}

	priv = gs_plugin_get_data (plugin);

	full_id = g_strdup_printf ("%s:%s",
				   gs_flatpak_get_prefix (priv->flatpak),
				   id);

	app = gs_plugin_cache_lookup (plugin, full_id);
	if (app) {
		g_debug ("Found cached '%s'", full_id);
		if (g_strcmp0 (gs_app_get_management_plugin (app),
			       gs_plugin_get_name (plugin)) == 0) {
			gs_app_set_management_plugin (app,
						      gs_plugin_get_name (plugin));
		}

		return app;
	}

	app = gs_app_new (full_id);
	gs_app_set_metadata (app, METADATA_HEADLESS_APP,
			     gs_app_get_id (headless_app));
	gs_app_set_metadata (app, METADATA_ASSET_URL, g_strdup (url));
	gs_app_set_metadata (app, "flatpak::kind", "runtime");
	gs_app_set_kind (app, AS_APP_KIND_RUNTIME);
	gs_app_set_flatpak_name (app, id);
	gs_app_set_flatpak_arch (app, flatpak_get_default_arch ());
	gs_app_set_flatpak_branch (app, "master");
	gs_app_set_management_plugin (app, gs_plugin_get_name (plugin));

	gs_plugin_cache_add (plugin, full_id, app);

	return app;
}

gboolean
gs_plugin_refine_app (GsPlugin *plugin,
		      GsApp *app,
		      GsPluginRefineFlags flags,
		      GCancellable *cancellable,
		      GError **error)
{
	GsApp *app_asset;
	const char *metadata = NULL;
	GsPluginData *priv;

	if (gs_app_is_flatpak (app) &&
	    g_strcmp0 (gs_app_get_flatpak_kind_as_str (app), "runtime") == 0) {
		gs_plugin_cache_add (plugin, gs_app_get_id (app), app);
		g_debug ("Caching remote '%s'", gs_app_get_id (app));
	}

		/* only process this app if was created by this plugin */
	if (g_strcmp0 (gs_app_get_management_plugin (app),
		       gs_plugin_get_name (plugin)) != 0)
		return TRUE;

	metadata = gs_app_get_metadata_item (app,
					     "flatpack-3rdparty::external-assets");

	g_debug ("Refining external app %s", gs_app_get_id (app));

	if (!metadata) {
		g_debug ("App '%s' is not an external headless app.",
			 gs_app_get_id (app));
		return TRUE;
	}

	app_asset = gs_plugin_get_app_external_asset (plugin, app);

	if (!app_asset) {
		g_debug ("Could not understand the asset from the "
			 "metadata in "
			 "app %s: %s",
			 gs_app_get_id_no_prefix (app), metadata);
		return TRUE;
	}

	priv = gs_plugin_get_data (plugin);

	if (!gs_flatpak_refine_app (priv->flatpak, app, flags, cancellable, 
				    error)) {
		g_debug ("Refining app %s failed!", gs_app_get_id (app));
		return FALSE;
	}

	/* We set the state to available because we assume that we can build
	 * the runtime */
	if (gs_app_get_state (app_asset) != AS_APP_STATE_INSTALLED) {
		gs_app_set_state (app, AS_APP_STATE_UNKNOWN);
		gs_app_set_state (app, AS_APP_STATE_AVAILABLE);
	}

	return TRUE;
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
gs_plugin_app_install (GsPlugin *plugin,
		       GsApp *app,
		       GCancellable *cancellable,
		       GError **error)
{
	guint i;
	const char *metadata = NULL;
	g_autofree char *runtime = NULL;
	g_autofree char *url = NULL;
	gboolean ret = TRUE;
	GsApp *app_asset;
	GsPluginData *priv;

	/* only process this app if was created by this plugin */
	if (g_strcmp0 (gs_app_get_management_plugin (app),
		       gs_plugin_get_name (plugin)) != 0)
		return TRUE;

	priv = gs_plugin_get_data (plugin);

	app_asset = gs_plugin_get_app_external_asset (plugin, app);

	if (!app_asset) {
		g_debug ("External app '%s' didn't have any asset! "
			 "Not installing and marking as state unknown!",
			 gs_app_get_id (app));
		gs_app_set_state (app, AS_APP_STATE_UNKNOWN);

		return TRUE;
	}

	gs_app_set_state (app, AS_APP_STATE_INSTALLING);

	if (gs_app_get_state (app_asset) == AS_APP_STATE_UNKNOWN) {
		if (!gs_plugin_build_runtime (plugin, app, app_asset, error)) {
			g_debug ("Failed to build runtime '%s'", runtime);
			return FALSE;
		}

		ret = gs_flatpak_refine_app (priv->flatpak, app_asset,
					     GS_PLUGIN_REFINE_FLAGS_DEFAULT,
					     cancellable, error);
		if (!ret) {
			g_debug ("Failed to refine '%s'",
				 gs_app_get_id (app_asset));
			return ret;
		}
	}

	if (gs_app_get_state (app_asset) == AS_APP_STATE_UNKNOWN) {
		g_debug ("Cannot install app asset '%s': state is unknown",
			 gs_app_get_id (app_asset));
		return FALSE;
	}

	g_debug ("INSTALLLING!");

	switch (gs_app_get_state (app_asset)) {
	case AS_APP_STATE_INSTALLED:
		g_debug ("App asset '%s' is already installed",
			 gs_app_get_id (app_asset));
		break;

	case AS_APP_STATE_UPDATABLE:
		g_debug ("Updating '%s'", gs_app_get_id (app_asset));
		ret = gs_flatpak_update_app (priv->flatpak, app_asset,
					     cancellable, error);
		if (!ret) {
			g_debug ("Failed to update '%s'",
				 gs_app_get_id (app_asset));
			return ret;
		}
		break;

	case AS_APP_STATE_AVAILABLE:
		g_debug ("Installing '%s'", gs_app_get_id (app_asset));
		gs_flatpak_app_install (priv->flatpak, app_asset, cancellable,
					error);
		if (gs_app_get_state (app_asset) != AS_APP_STATE_INSTALLED) {
			g_debug ("Failed to install '%s'",
				 gs_app_get_id (app_asset));
			return FALSE;
		}
		break;
	}

	if (gs_app_get_state (app_asset) != AS_APP_STATE_INSTALLED) {
		gs_app_set_state (app, gs_app_get_state (app_asset));
		g_debug ("Cannot install '%s' because runtime '%s' is not "
			 "installed", gs_app_get_id (app),
			 gs_app_get_id (app_asset));
		return FALSE;
	}

	gs_flatpak_app_install (priv->flatpak, app, cancellable, error);

	if (gs_app_get_state (app) != AS_APP_STATE_INSTALLED) {
		g_debug ("Failed to install '%s'", gs_app_get_id (app));
		return FALSE;
	}

	return TRUE;
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
gs_plugin_launch (GsPlugin *plugin,
		  GsApp *app,
		  GCancellable *cancellable,
		  GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	return gs_flatpak_launch (priv->flatpak, app, cancellable, error);
}

