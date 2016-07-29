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
#include <glib.h>
#include <gnome-software.h>
#include <json-glib/json-glib.h>
#include <string.h>

#include "gs-flatpak.h"

#define EXTERNAL_ASSETS_SPEC_VERSION 1
#define JSON_SPEC_KEY "spec"
#define JSON_RUNTIME_KEY "runtime"
#define JSON_RUNTIME_NAME_KEY "name"
#define JSON_RUNTIME_URL_KEY "url"
#define JSON_RUNTIME_TYPE_KEY "type"

#define METADATA_URL "GnomeSoftware::external-app::url"
#define METADATA_TYPE "GnomeSoftware::external-app::type"
#define METADATA_HEADLESS_APP "GnomeSoftware::external-app::headless-app"
#define METADATA_BUILD_DIR "GnomeSoftware::external-app::build-dir"
#define METADATA_EXTERNAL_ASSETS "flatpak-3rdparty::external-assets"

#define TMP_ASSSETS_PREFIX "gs-external-apps"

typedef enum {
	GS_PLUGIN_EXTERNAL_TYPE_UNKNOWN = 0,
	GS_PLUGIN_EXTERNAL_TYPE_DEB,
	GS_PLUGIN_EXTERNAL_TYPE_TAR
} GsPluginExternalType;

struct GsPluginData {
	GsFlatpak	*flatpak;
	char		*runtimes_build_dir;
};

static gboolean flatpak_remote_delete (const char *repo_name, GError **error);

static void
remove_runtimes_build_dir (GsPlugin *plugin)
{
	g_autoptr(GError) error = NULL;
	GsPluginData *priv = gs_plugin_get_data (plugin);

	if (gs_utils_rmtree (priv->runtimes_build_dir, &error))
		return;

	if (!g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
		g_debug ("Cannot remove previously created external apps build "
			 "dir '%s': %s", priv->runtimes_build_dir,
			 error->message);
	}
}

static void
remove_ext_apps_remotes (GsPlugin *plugin)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	guint i;
	GPtrArray *names;

	names = gs_flatpak_get_remotes_names (priv->flatpak, NULL, NULL);

	if (!names)
		return;

	for (i = 0; i < names->len; i++) {
		const char *name = g_ptr_array_index (names, i);
		if (g_str_has_prefix (name, TMP_ASSSETS_PREFIX)) {
			flatpak_remote_delete (name, NULL);
		}
	}

	g_ptr_array_free (names, TRUE);
}

void
gs_plugin_initialize (GsPlugin *plugin)
{
	GsPluginData *priv = gs_plugin_alloc_data (plugin, sizeof(GsPluginData));
	const char *ext_apps_build_dir = g_get_user_cache_dir ();

	priv->flatpak = gs_flatpak_new (plugin, GS_FLATPAK_SCOPE_USER);
	priv->runtimes_build_dir = g_build_filename (ext_apps_build_dir,
						     TMP_ASSSETS_PREFIX,
						     NULL);

	/* Run plugin before the flatpak ones because we need them to install
	 * the app's headless part first */
	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_BEFORE, "flatpak-system");
	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_BEFORE, "flatpak-user");
}

void
gs_plugin_destroy (GsPlugin *plugin)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);

	/* Remove the runtimes build directory to clean any contents eventually
	 * left from previous builds */
	remove_runtimes_build_dir (plugin);

	g_clear_object (&priv->flatpak);
	g_free (priv->runtimes_build_dir);
}

static gboolean
app_is_flatpak (GsApp *app)
{
	const gchar *id = gs_app_get_unique_id (app);
	return id && (g_str_has_prefix (id, GS_FLATPAK_USER_PREFIX) ||
		      g_str_has_prefix (id, GS_FLATPAK_SYSTEM_PREFIX));
}

void
gs_plugin_adopt_app (GsPlugin *plugin,
		     GsApp *app)
{
	if (!app_is_flatpak (app) ||
	    !gs_app_get_metadata_item (app, METADATA_EXTERNAL_ASSETS))
		return;

	g_debug ("Adopt '%s' as an external app", gs_app_get_id (app));

	gs_app_set_management_plugin (app, gs_plugin_get_name (plugin));
}

gboolean
gs_plugin_setup (GsPlugin *plugin,
		 GCancellable *cancellable,
		 GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	gboolean ret = gs_flatpak_setup (priv->flatpak, cancellable, error);

	if (ret) {
		/* Remove the runtimes build directories and remotes to clean
		 * any contents eventually left from previous builds */
		remove_runtimes_build_dir (plugin);
		remove_ext_apps_remotes (plugin);
	}

	return ret;
}

static char *
download_asset (GsPlugin *plugin,
		GsApp *app,
		const char *runtime,
		const char *asset,
		GError **error)
{
	g_autofree gchar *cache_basename = NULL;
	g_autofree gchar *cache_fn = NULL;
	g_autofree gchar *cache_png = NULL;
	g_autofree gchar *data = NULL;

	cache_basename = g_path_get_basename (asset);
	cache_fn = gs_utils_get_cache_filename (gs_plugin_get_name (plugin),
						cache_basename,
						GS_UTILS_CACHE_FLAG_NONE,
						error);
	if (cache_fn == NULL)
		return NULL;

	if (!g_file_test (cache_fn, G_FILE_TEST_EXISTS)) {
		if (!gs_mkdir_parent (cache_fn, error))
			return NULL;

		if (!gs_plugin_download_file (plugin, app, asset, cache_fn,
					      NULL, error))
			return NULL;
	}

	return g_steal_pointer (&cache_fn);
}

static gboolean
run_command (const char *working_dir,
	     const char **argv,
	     GError **error)
{
	g_autofree char *stderr_buf = NULL;
	g_autofree char *stdout_buf = NULL;
	g_autofree char *cmd = NULL;
	int exit_val = -1;

	if (!g_spawn_sync (working_dir, (char **) argv, NULL,
			   G_SPAWN_SEARCH_PATH, NULL, NULL, &stdout_buf,
			   &stderr_buf, &exit_val, error)) {
		return FALSE;
	}

	cmd = g_strjoinv (" ", (char **) argv);
	g_debug ("Result of running '%s': retcode=%d stdout='%s' stderr='%s'",
		 cmd, exit_val,	(stdout_buf ? stdout_buf : "NULL"),
		 (stderr_buf ? stderr_buf : "NULL"));

	return g_spawn_check_exit_status (exit_val, error);
}

static gboolean
flatpak_repo_build_init (const char *repo_dir,
			 const char *repo_name,
			 GError **error)
{
	const char *endless_runtime = "com.endlessm.Platform";
	const char *argv[] = {"flatpak", "build-init",  repo_dir, repo_name,
			      endless_runtime, endless_runtime, NULL};
	const char *argv2[] = {"sed", "-i", "s/Application/Runtime/",
			       "metadata", NULL};

	if (!run_command (NULL, argv, error))
		return FALSE;

	if (!run_command (repo_dir, argv2, error))
		return FALSE;

	return TRUE;
}

static gboolean
flatpak_repo_build_export (const char *repo_dir,
			   const char *repo_name,
			   GError **error)
{
	const char *argv[] = {"flatpak", "build-export",  "--runtime",
			      repo_name, repo_dir, NULL};
	return run_command (NULL, argv, error);
}

static gboolean
flatpak_remote_add (const char *repo_dir,
		    const char *repo_name,
		    GError **error)
{
	const char *argv[] = {"flatpak", "remote-add",  "--user",
			      "--no-gpg-verify", repo_name, repo_dir, NULL};
	return run_command (NULL, argv, error);
}

static gboolean
flatpak_remote_delete (const char *repo_name,
		       GError **error)
{
	const char *argv[] = {"flatpak", "remote-delete",  "--user", "--force",
			      repo_name, NULL};
	return run_command (NULL, argv, error);
}

static gboolean
add_runtime_deb_asset (const gchar *build_dir,
		       const gchar *repo_dir,
		       const char *asset_path,
		       GError **error)
{
	const char *argv[] = {"ar", "x",  asset_path, NULL};
	const char **argv2 = NULL;
	g_autofree char *data_tar_path = NULL;
	gboolean ret = FALSE;

	if (!run_command (build_dir, argv, error)) {
		return FALSE;
	}

	data_tar_path = g_build_filename (build_dir, "data.tar.gz", NULL);
	if (!g_file_test (data_tar_path, G_FILE_TEST_EXISTS)) {
		g_free (data_tar_path);
		data_tar_path = g_build_filename (build_dir, "data.tar.xz",
						  NULL);

		if (!g_file_test (data_tar_path, G_FILE_TEST_EXISTS)) {
			g_set_error (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_FAILED,
				     "Could not find data.tar.gz or "
				     "data.tar.xz after decompressing Debian "
				     "package '%s' in '%s'", asset_path,
				     build_dir);

			return FALSE;
		}
	}

	/* Build array for command: tar xf TAR_FILE -C REPO_DIR */
	argv2 = (const char **) g_new0 (char *, 6);
	argv2[0] = "tar";
	argv2[1] = "xf";
	argv2[2] = data_tar_path;
	argv2[3] = "-C";
	argv2[4] = repo_dir;

	ret = run_command (build_dir, argv2, error);

	g_free (argv2);

	return ret;
}

static inline GsPluginExternalType
get_type_from_string (const char *type)
{
	if (g_strcmp0 (type, "deb") == 0)
		return GS_PLUGIN_EXTERNAL_TYPE_DEB;
	else if (g_strcmp0 (type, "tar") == 0)
		return  GS_PLUGIN_EXTERNAL_TYPE_TAR;

	return GS_PLUGIN_EXTERNAL_TYPE_UNKNOWN;
}

static gboolean
add_runtime_tar_asset (const gchar *build_dir,
		       const gchar *repo_dir,
		       const char *asset_path,
		       GError **error)
{
	/* flatpak build --runtime needs files in /usr, when coming from a
	   debian package we can assume some files in /usr, when coming from
	   a tgz, it's harder, so force that here. */
	g_autofree gchar *extract_path = g_build_filename (repo_dir, "usr", NULL);
	const char *argv[] = {"tar", "xvf",  asset_path, "-C", extract_path, NULL};

	g_mkdir_with_parents (extract_path, 0700);

	return run_command (build_dir, argv, error);
}

static gboolean
add_runtime_asset (GsPlugin *plugin,
		   GsApp *app,
		   const gchar *build_dir,
		   const gchar *repo_dir,
		   const char *runtime,
		   const char *archive_type,
		   const char *asset,
		   GError **error)
{
	g_autofree char *content_type = NULL;
	g_autoptr(GFile) download_file = NULL;
	g_autofree char *download_name = download_asset (plugin, app, runtime,
							 asset, error);
	GsPluginExternalType type = get_type_from_string (archive_type);

	if (!download_name)
		return FALSE;

	download_file = g_file_new_for_path (download_name);
	content_type = gs_utils_get_content_type (download_file, NULL,
						  error);

	if (!content_type)
		return FALSE;

	g_debug ("Adding runtime asset with type '%s'", content_type);

	if ((type == GS_PLUGIN_EXTERNAL_TYPE_DEB) ||
	    g_content_type_is_a (content_type, "application/x-deb")) {
		return add_runtime_deb_asset (build_dir, repo_dir,
					      download_name, error);
	}

	if ((type == GS_PLUGIN_EXTERNAL_TYPE_TAR) ||
	    g_content_type_is_a (content_type, "application/x-tar")) {
		return add_runtime_tar_asset (build_dir, repo_dir,
					      download_name, error);
	}

	g_set_error (error,
		     GS_PLUGIN_ERROR,
		     GS_PLUGIN_ERROR_FAILED,
		     "Cannot deal with asset type '%s'", content_type);

	return FALSE;
}

static gboolean
clean_runtime_build_dir (GsPlugin *plugin,
			 GsApp *runtime,
			 GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	GError *local_error = NULL;
	const char *runtime_name = gs_app_get_id (runtime);
	g_autofree char *build_dir = g_build_filename (priv->runtimes_build_dir,
						       runtime_name,
						       NULL);

	if (!gs_utils_rmtree (build_dir, &local_error)) {
		if (!g_error_matches (local_error, G_FILE_ERROR,
				     G_FILE_ERROR_NOENT)) {
			g_debug ("Cannot remove runtime build dir '%s': %s",
				 build_dir, local_error->message);

			g_propagate_error (error, local_error);

			return FALSE;
		}

		g_clear_error (&local_error);
	}

	return TRUE;
}

static gboolean
build_runtime (GsPlugin *plugin,
	       GsApp *app,
	       GsApp *runtime,
	       GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	g_autofree char *tmp_dir;
	g_autofree char *repo_dir = NULL;
	g_autofree char *build_dir = NULL;
	g_autoptr(GFile) tmp_build_dir = NULL;
	g_autofree char *repo_name = NULL;
	g_autofree char *dir_basename = NULL;
	const char *repo_build_dir = "3rd-party-repo";
	const char *runtime_name = gs_app_get_id (runtime);
	const char *runtime_url = gs_app_get_metadata_item (runtime,
							    METADATA_URL);
	const char *runtime_type = gs_app_get_metadata_item (runtime,
							     METADATA_TYPE);

	tmp_dir = g_build_filename (priv->runtimes_build_dir, runtime_name,
				    NULL);

	g_debug ("Building runtime extension '%s' in dir '%s'", runtime_name,
		 tmp_dir);

	/* Remove a directory if left over from a previous build */
	if (!clean_runtime_build_dir (plugin, app, error))
		return FALSE;

	build_dir = g_build_filename (tmp_dir, repo_build_dir, NULL);
	tmp_build_dir = g_file_new_for_path (build_dir);

	if (!g_file_make_directory_with_parents (tmp_build_dir, NULL, error)) {
		g_debug ("Failed to make build dir '%s'", build_dir);
		return FALSE;
	}

	gs_app_set_progress (app, 10);

	if (!flatpak_repo_build_init (build_dir, runtime_name, error)) {
		g_debug ("Failed to initialize the repo build in directory "
			 "'%s'", build_dir);
		return FALSE;
	}

	gs_app_set_progress (app, 15);

	if (!add_runtime_asset (plugin, app, tmp_dir, build_dir, runtime_name,
				runtime_type, runtime_url, error)) {
		g_debug ("Failed to add the asset '%s'", runtime_name);
		return FALSE;
	}

	gs_app_set_progress (app, 30);

	g_debug ("Exporting repo in '%s'... (this may take a while)", build_dir);

	repo_dir = g_build_filename (tmp_dir, runtime_name, NULL);

	if (!flatpak_repo_build_export (build_dir, repo_dir, error)) {
		g_debug ("Failed to export repo '%s' in '%s'!", repo_dir,
			 build_dir);
		return FALSE;
	}

	gs_app_set_progress (app, 50);

	g_debug ("Repo '%s' exported! Adding it now.", repo_dir);

	repo_name = g_strconcat (TMP_ASSSETS_PREFIX, "_", runtime_name, NULL);

	/* Delete any previously uncleaned remotes for this runtime */
	flatpak_remote_delete (repo_name, NULL);

	if (!flatpak_remote_add (repo_dir, repo_name, error)) {
		g_debug ("Failed to add remote '%s' from dir '%s'",
			 repo_name, repo_dir);
		return FALSE;
	}

	gs_app_set_origin (runtime, repo_name);
	gs_app_set_metadata (runtime, METADATA_BUILD_DIR, build_dir);
	gs_app_set_state (runtime, AS_APP_STATE_AVAILABLE);

	gs_app_set_progress (app, 70);

	return TRUE;
}

static char *
extract_runtime_info_from_json_data (const char *data,
				     char **url,
				     char **type,
				     GError **error)
{
	gboolean ret;
	g_autoptr(JsonParser) parser = NULL;
	JsonObject *root, *runtime;
	g_autoptr(GList) members = NULL;
	const char *runtime_name = NULL;
	const char *json_url = NULL;
	const char *type_str = NULL;
	guint spec = 0;

	parser = json_parser_new ();

	ret = json_parser_load_from_data (parser, data, -1, error);
	if (!ret)
		return NULL;

	root = json_node_get_object (json_parser_get_root (parser));
	if (!root) {
		g_set_error (error,
		             GS_PLUGIN_ERROR,
		             GS_PLUGIN_ERROR_FAILED,
		             "no root object");
		return NULL;
	}

	spec = json_object_get_int_member (root, JSON_SPEC_KEY);
	if (spec != EXTERNAL_ASSETS_SPEC_VERSION) {
		g_set_error (error,
		             GS_PLUGIN_ERROR,
		             GS_PLUGIN_ERROR_FAILED,
		             "External asset's json spec version '%u' does "
			     "not match the plugin. Expected '%u'", spec,
			     EXTERNAL_ASSETS_SPEC_VERSION);
		return NULL;
	}

	runtime = json_object_get_object_member (root, JSON_RUNTIME_KEY);
	if (!runtime) {
		g_set_error (error,
		             GS_PLUGIN_ERROR,
		             GS_PLUGIN_ERROR_FAILED,
		             "External asset's json has no '%s' member set",
			     JSON_RUNTIME_KEY);
		return NULL;
	}

	runtime_name = json_object_get_string_member (runtime,
						      JSON_RUNTIME_NAME_KEY);
	if (!runtime_name) {
		g_set_error (error,
		             GS_PLUGIN_ERROR,
		             GS_PLUGIN_ERROR_FAILED,
		             "External asset's runtime member has no '%s' key "
			     "set", JSON_RUNTIME_NAME_KEY);
		return NULL;
	}

	json_url = json_object_get_string_member (runtime, JSON_RUNTIME_URL_KEY);
	if (!json_url) {
		g_set_error (error,
		             GS_PLUGIN_ERROR,
		             GS_PLUGIN_ERROR_FAILED,
		             "External asset's runtime member has no '%s' key "
			     "set", JSON_RUNTIME_URL_KEY);
		return NULL;
	}

	/* optional elements */
	type_str = json_object_get_string_member (runtime,
						  JSON_RUNTIME_TYPE_KEY);
	*type = g_strdup (type_str);
	*url = g_strdup (json_url);

	return g_strdup (runtime_name);
}

static GsApp *
gs_plugin_get_app_external_runtime (GsPlugin *plugin,
				    GsApp *headless_app)
{
	GsApp *runtime = NULL;
	GsPluginData *priv;
	g_autofree char *id = NULL;
	g_autofree char *full_id = NULL;
	g_autofree char *url = NULL;
	g_autofree char *type = NULL;
	g_autofree char *json_data = NULL;
	g_autoptr (GError) error = NULL;
	const char *metadata;

	metadata = gs_app_get_metadata_item (headless_app,
					     METADATA_EXTERNAL_ASSETS);

	if (!metadata)
		return NULL;

	json_data = g_uri_unescape_string (metadata, NULL);
	id = extract_runtime_info_from_json_data (json_data, &url, &type, &error);

	if (!id) {
		g_debug ("Error getting external runtime from "
			 "metadata: %s", error->message);
		return NULL;
	}

	priv = gs_plugin_get_data (plugin);

	full_id = g_strdup_printf ("%s:%s",
				   gs_flatpak_get_prefix (priv->flatpak),
				   id);

	runtime = gs_plugin_cache_lookup (plugin, full_id);
	if (runtime) {
		g_debug ("Found cached '%s'", full_id);
		gs_app_set_management_plugin (runtime,
					      gs_plugin_get_name (plugin));

		return runtime;
	}

	runtime = gs_app_new (full_id);
	gs_app_set_metadata (runtime, METADATA_HEADLESS_APP,
			     gs_app_get_id (headless_app));
	gs_app_set_metadata (runtime, METADATA_URL, url);
	gs_app_set_metadata (runtime, METADATA_TYPE, type);
	gs_app_set_metadata (runtime, "flatpak::kind", "runtime");
	gs_app_set_kind (runtime, AS_APP_KIND_RUNTIME);
	gs_app_set_flatpak_name (runtime, id);
	gs_app_set_flatpak_arch (runtime, flatpak_get_default_arch ());
	gs_app_set_flatpak_branch (runtime, "master");
	gs_app_set_management_plugin (runtime, gs_plugin_get_name (plugin));

	gs_plugin_cache_add (plugin, full_id, runtime);

	if (gs_flatpak_is_installed (priv->flatpak, runtime, NULL, NULL)) {
		gs_app_set_state (runtime, AS_APP_STATE_INSTALLED);
	}

	return runtime;
}

gboolean
gs_plugin_refine_app (GsPlugin *plugin,
		      GsApp *app,
		      GsPluginRefineFlags flags,
		      GCancellable *cancellable,
		      GError **error)
{
	GsApp *ext_runtime;
	const char *metadata = NULL;
	GsPluginData *priv;

	/* We cache all runtimes because an external runtime may have been
	 * adopted by the flatpak plugins */
	if (app_is_flatpak (app) && gs_flatpak_app_is_runtime (app)) {
		gs_plugin_cache_add (plugin, gs_app_get_id (app), app);
		g_debug ("Caching remote '%s'", gs_app_get_id (app));
	}

	/* only process this app if was created by this plugin */
	if (g_strcmp0 (gs_app_get_management_plugin (app),
		       gs_plugin_get_name (plugin)) != 0)
		return TRUE;

	g_debug ("Refining external app %s", gs_app_get_id (app));

	ext_runtime = gs_plugin_get_app_external_runtime (plugin, app);

	if (!ext_runtime) {
		g_debug ("Could not understand the asset from the "
			 "metadata in "
			 "app %s: %s",
			 gs_app_get_id (app), metadata);
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
	if (gs_app_get_state (ext_runtime) != AS_APP_STATE_INSTALLED) {
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
	return gs_flatpak_add_installed (priv->flatpak, list, cancellable,
					 error);
}

gboolean
gs_plugin_app_install (GsPlugin *plugin,
		       GsApp *app,
		       GCancellable *cancellable,
		       GError **error)
{
	g_autofree char *runtime = NULL;
	g_autofree char *url = NULL;
	const char *remote_name;
	GError *internal_error = NULL;
	GsApp *ext_runtime;
	GsPluginData *priv;
	gboolean ret = FALSE;

	/* only process this app if was created by this plugin */
	if (g_strcmp0 (gs_app_get_management_plugin (app),
		       gs_plugin_get_name (plugin)) != 0)
		return TRUE;

	priv = gs_plugin_get_data (plugin);

	ext_runtime = gs_plugin_get_app_external_runtime (plugin, app);

	if (!ext_runtime) {
		g_debug ("External app '%s' didn't have any asset! "
			 "Not installing and marking as state unknown!",
			 gs_app_get_id (app));
		gs_app_set_state (app, AS_APP_STATE_UNKNOWN);

		return TRUE;
	}

	gs_app_set_state (app, AS_APP_STATE_INSTALLING);

	if (gs_app_get_state (ext_runtime) == AS_APP_STATE_UNKNOWN) {
		if (!build_runtime (plugin, app, ext_runtime, error)) {
			g_debug ("Failed to build runtime '%s'", runtime);
			return FALSE;
		}

		if (!gs_flatpak_refine_app (priv->flatpak, ext_runtime,
					    GS_PLUGIN_REFINE_FLAGS_DEFAULT,
					    cancellable, error)) {
			g_debug ("Failed to refine '%s'",
				 gs_app_get_id (ext_runtime));
			return FALSE;
		}
	}

	switch (gs_app_get_state (ext_runtime)) {
	case AS_APP_STATE_INSTALLED:
		g_debug ("App asset '%s' is already installed",
			 gs_app_get_id (ext_runtime));
		break;

	case AS_APP_STATE_UPDATABLE:
		g_debug ("Updating '%s'", gs_app_get_id (ext_runtime));

		if (!gs_flatpak_update_app (priv->flatpak, ext_runtime,
					    cancellable, error)) {
			g_debug ("Failed to update '%s'",
				 gs_app_get_id (ext_runtime));
			return FALSE;
		}
		break;

	case AS_APP_STATE_AVAILABLE:
		g_debug ("Installing '%s'", gs_app_get_id (ext_runtime));
		ret = gs_flatpak_app_install (priv->flatpak, ext_runtime,
					      cancellable, error);

		/* Clean-up remote (we only needed it for installing the app) */
		remote_name = gs_app_get_origin (ext_runtime);
		if (!flatpak_remote_delete (remote_name,
					    &internal_error)) {
			g_debug ("Failed to delete remote '%s': %s",
				 remote_name, internal_error->message);
			g_clear_error (&internal_error);
		}

		if (!ret) {
			g_debug ("Failed to install '%s'",
				 gs_app_get_id (ext_runtime));
			return FALSE;
		}

		break;

	case AS_APP_STATE_UNKNOWN:
	default:
		/* In case we may end up here somehow, just let the situation
		 * be dealt with in the check for the 'installed' state below */
		break;
	}

	if (gs_app_get_state (ext_runtime) != AS_APP_STATE_INSTALLED) {
		gs_app_set_state (app, gs_app_get_state (ext_runtime));
		g_set_error (error, GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_NOT_SUPPORTED,
			     "Could not install external app '%s' because its "
			     "extension runtime '%s' is not installed",
			     gs_app_get_id (app),
			     gs_app_get_id (ext_runtime));
		return FALSE;
	}

	if (!gs_flatpak_app_install (priv->flatpak, app, cancellable, error)) {
		g_debug ("Failed to install '%s'", gs_app_get_id (app));
		return FALSE;
	}

	/* Everything went fine so clean the runtime build directory */
	clean_runtime_build_dir (plugin, ext_runtime, NULL);

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

gboolean
gs_plugin_app_remove (GsPlugin *plugin,
		      GsApp *app,
		      GCancellable *cancellable,
		      GError **error)
{
	GsApp *ext_runtime;
	GsPluginData *priv;

	/* only process this app if was created by this plugin */
	if (g_strcmp0 (gs_app_get_management_plugin (app),
		       gs_plugin_get_name (plugin)) != 0)
		return TRUE;

	gs_app_set_state (app, AS_APP_STATE_REMOVING);

	priv = gs_plugin_get_data (plugin);

	ext_runtime = gs_plugin_get_app_external_runtime (plugin, app);

	if (!ext_runtime) {
		g_debug ("External app '%s' has no external runtime to be"
			 "removed", gs_app_get_id (app));
	} else if (gs_app_get_state (ext_runtime) == AS_APP_STATE_INSTALLED ||
		   gs_app_get_state (ext_runtime) == AS_APP_STATE_UPDATABLE) {
		g_autoptr(GError) local_error = NULL;

		if (!gs_flatpak_app_remove (priv->flatpak, ext_runtime,
					    cancellable, &local_error)) {
			g_debug ("Cannot remove '%s': %s. Will try to "
				 "remove app '%s'.",
				 gs_app_get_id (ext_runtime),
				 local_error->message,
				 gs_app_get_id (app));
		}
	}

	return gs_flatpak_app_remove (priv->flatpak, app, cancellable, error);
}
