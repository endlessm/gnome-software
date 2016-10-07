/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2016 Endless Mobile, Inc.
 *
 * Authors: Joaquim Rocha <jrocha@endlessm.com>
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

#include <errno.h>
#include <glib/gstdio.h>
#include <gnome-software.h>
#include <gs-common.h>

#define APPSTREAM_SYSTEM_DIR LOCALSTATEDIR "/cache/app-info/xmls"

struct GsPluginData {
	GSettings *settings;
};

void
gs_plugin_initialize (GsPlugin *plugin)
{
	GsPluginData *priv = gs_plugin_alloc_data (plugin, sizeof(GsPluginData));

	priv->settings = g_settings_new ("org.gnome.software");

	/* run it before the appstream plugin */
	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_BEFORE, "appstream");
}

void
gs_plugin_destroy (GsPlugin *plugin)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);

	g_object_unref (priv->settings);
}

static gboolean
should_update_appstream_file (const char *appstream_path,
			      guint cache_age)
{
	g_autoptr(GFile) file = g_file_new_for_path (appstream_path);
	guint appstream_file_age = gs_utils_get_file_age (file);

	return appstream_file_age >= cache_age;
}

static gboolean
install_appstream (const char *appstream_file,
		   const char *target_file_name,
		   GCancellable *cancellable,
		   GError **error)
{
	g_autoptr(GSubprocess) subprocess = NULL;
	const char *argv[] = {"pkexec",
			      LIBEXECDIR "/gnome-software-install-appstream",
			      appstream_file, target_file_name, NULL};

	g_debug ("Installing the appstream file %s in the system",
		 appstream_file);

	subprocess = g_subprocess_newv (argv,
					G_SUBPROCESS_FLAGS_STDOUT_PIPE |
					G_SUBPROCESS_FLAGS_STDIN_PIPE, error);

	if (subprocess == NULL)
		return FALSE;

	return g_subprocess_wait_check (subprocess, cancellable, error);
}

static gboolean
create_tmp_file (const char *tmp_file_tmpl,
		 char **tmp_file_name,
		 GError **error)
{
	gint handle = g_file_open_tmp (tmp_file_tmpl, tmp_file_name, error);

	if (handle != -1) {
		close (handle);
		return TRUE;
	}
	return FALSE;
}

static gboolean
update_external_appstream (GsPlugin *plugin,
			   const char *url,
			   guint cache_age,
			   GCancellable *cancellable,
			   GError **error)
{
	g_autofree char *file_name = g_path_get_basename (url);
	g_autofree char *tmp_file_name = g_strdup_printf ("XXXXXX_%s",
							  file_name);
	g_autofree char *tmp_file = NULL;
	guint status_code;
	g_autofree char *target_file_path =
		g_build_filename (APPSTREAM_SYSTEM_DIR, file_name, NULL);

	if (!should_update_appstream_file (target_file_path, cache_age)) {
		g_debug ("Skipping updating external appstream file %s: "
			 "cache age is older than file",
			 target_file_path);
		return TRUE;
	}

	/* Create a temporary file that will hold the contents of the appstream
	 * file to avoid clashes with existing files */
	if (!create_tmp_file (tmp_file_name, &tmp_file, error))
		return FALSE;

	if (!gs_plugin_download_file (plugin, NULL, url, tmp_file, cancellable,
				      error)) {
		return FALSE;
	}

	g_debug ("Downloaded appstream file %s", tmp_file);

	if (!install_appstream (tmp_file, file_name, cancellable, error))
		return FALSE;

	g_debug ("Installed appstream file %s as %s", tmp_file, file_name);

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
	g_auto(GStrv) appstream_urls = NULL;
	GError *local_error = NULL;
	guint i;

	if ((flags & GS_PLUGIN_REFRESH_FLAGS_METADATA) == 0)
		return TRUE;

	appstream_urls = g_settings_get_strv (priv->settings,
					      "external-appstream-urls");

	for (i = 0; appstream_urls[i] != NULL; ++i) {
		if (!g_str_has_prefix (appstream_urls[i], "https")) {
			g_warning ("Not considering %s as an external "
				   "appstream source: please use an https URL",
				   appstream_urls[i]);
			continue;
		}

		if (!update_external_appstream (plugin, appstream_urls[i],
						cache_age, cancellable,
						&local_error)) {
			g_warning ("Failed to update external appstream "
				   "file: %s", local_error->message);
			g_clear_error (&local_error);
		}
	}

	return TRUE;
}
