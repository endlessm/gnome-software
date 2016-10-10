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
#include <libsoup/soup.h>
#include <locale.h>

#define APPSTREAM_SYSTEM_DIR LOCALSTATEDIR "/cache/app-info/xmls"

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

	g_debug ("appstream system dir: %s", APPSTREAM_SYSTEM_DIR);
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

static char *
get_modification_date (const char *file_path)
{
	g_autoptr(GFile) file = NULL;
	g_autoptr(GFileInfo) info = NULL;
	char *mdate = NULL;
	g_autoptr(GDateTime) date_time = NULL;
	GTimeVal time_val;

	file = g_file_new_for_path (file_path);
	info = g_file_query_info (file,
				  G_FILE_ATTRIBUTE_TIME_MODIFIED,
				  G_FILE_QUERY_INFO_NONE,
				  NULL,
				  NULL);
	if (info == NULL)
		return NULL;

	g_file_info_get_modification_time (info, &time_val);

	date_time = g_date_time_new_from_timeval_local (&time_val);
	mdate = g_date_time_format (date_time, "%a, %d %b %Y %H:%M:%S %Z");

	return mdate;
}

static gboolean
update_external_appstream (GsPlugin *plugin,
			   const char *url,
			   guint cache_age,
			   GCancellable *cancellable,
			   GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	g_autoptr(GError) local_error = NULL;
	g_autoptr(SoupMessage) msg = NULL;
	g_autofree char *file_name = g_path_get_basename (url);
	g_autofree char *tmp_file_name = g_strdup_printf ("XXXXXX_%s",
							  file_name);
	g_autofree char *tmp_file = NULL;
	guint status_code;
	g_autofree char *target_file_path =
		g_build_filename (APPSTREAM_SYSTEM_DIR, file_name, NULL);
	g_autofree char *local_mod_date = NULL;
	SoupSession *soup_session;

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

	msg = soup_message_new (SOUP_METHOD_GET, url);

	/* Set the If-Modified-Since header if the target file exists */
	target_file_path = g_build_filename (APPSTREAM_SYSTEM_DIR, file_name,
					     NULL);
	local_mod_date = get_modification_date (target_file_path);

	if (local_mod_date != NULL) {
		g_debug ("Requesting contents of %s if modified since %s",
			 url, local_mod_date);
		soup_message_headers_append (msg->request_headers,
					     "If-Modified-Since",
					     local_mod_date);
	}

	soup_session = gs_plugin_get_soup_session (plugin);
	status_code = soup_session_send_message (soup_session, msg);

	if (status_code != SOUP_STATUS_OK) {
		/* The temporary file will no longer be moved so remove it */
		if (g_unlink (tmp_file) == -1)
			g_debug ("Could not delete temporary file %s",
				 tmp_file);

		if (status_code == SOUP_STATUS_NOT_MODIFIED) {
			g_debug ("Not updating %s has not modified since %s",
				 target_file_path, local_mod_date);
			return TRUE;
		}

		g_set_error (error, GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_DOWNLOAD_FAILED,
			     "Failed to download appstream file %s: %s",
			     url, soup_status_get_phrase (status_code));
		return FALSE;
	}

	/* A new version of the appstream file was retrieved so set the
	 * contents in the temporary file we created */
	if (!g_file_set_contents (tmp_file, msg->response_body->data,
				  msg->response_body->length, &local_error)) {
		g_set_error (error, GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_WRITE_FAILED,
			     "Failed to create appstream file: %s",
			     local_error->message);
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
