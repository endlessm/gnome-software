/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2013-2014 Richard Hughes <richard@hughsie.com>
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

#define _GNU_SOURCE
#include <string.h>

#include <gnome-software.h>

/*
 * SECTION:
 * Uses epiphany to launch web applications.
 *
 * If the epiphany binary is not present then it self-disables.
 */

void
gs_plugin_initialize (GsPlugin *plugin)
{
	g_autofree gchar *epiphany = NULL;

	/* we can only work with epiphany */
	epiphany = g_find_program_in_path ("epiphany");
	if (epiphany == NULL) {
		gs_plugin_set_enabled (plugin, FALSE);
		g_debug ("disabling '%s' as epiphany does not exist",
			 gs_plugin_get_name (plugin));
	}

	/* need help from appstream */
	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_AFTER, "appstream");
}

void
gs_plugin_adopt_app (GsPlugin *plugin, GsApp *app)
{
	if (gs_app_get_kind (app) == AS_APP_KIND_WEB_APP)
		gs_app_set_management_plugin (app, gs_plugin_get_name (plugin));
}

static gchar *
_gs_app_get_id_nonfull (GsApp *app)
{
	gchar *id;
	gchar *tmp;

	id = g_strdup (gs_app_get_id (app));
	tmp = g_strrstr (id, ".desktop");
	if (tmp != NULL)
		*tmp = '\0';
	return id;
}

gboolean
gs_plugin_app_install (GsPlugin *plugin, GsApp *app,
		       GCancellable *cancellable, GError **error)
{
	AsIcon *icon;
	GPtrArray *icons;
	gboolean ret = TRUE;
	gsize kf_length;
	g_autoptr(GError) error_local = NULL;
	g_autofree gchar *app_desktop = NULL;
	g_autofree gchar *epi_desktop = NULL;
	g_autofree gchar *epi_dir = NULL;
	g_autofree gchar *epi_icon = NULL;
	g_autofree gchar *exec = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *id_nonfull = NULL;
	g_autofree gchar *kf_data = NULL;
	g_autofree gchar *wmclass = NULL;
	g_autoptr(GKeyFile) kf = NULL;
	g_autoptr(GFile) symlink_desktop = NULL;
	g_autoptr(GFile) symlink_icon = NULL;

	/* only process this app if was created by this plugin */
	if (g_strcmp0 (gs_app_get_management_plugin (app),
		       gs_plugin_get_name (plugin)) != 0)
		return TRUE;

	/* create the correct directory */
	id_nonfull = _gs_app_get_id_nonfull (app);
	hash = g_compute_checksum_for_string (G_CHECKSUM_SHA1, gs_app_get_name (app), -1);
	epi_dir = g_strdup_printf ("%s/epiphany/app-%s-%s",
				   g_get_user_config_dir (),
				   id_nonfull,
				   hash);
	g_mkdir_with_parents (epi_dir, 0755);

	/* symlink icon */
	epi_icon = g_build_filename (epi_dir, "app-icon.png", NULL);
	symlink_icon = g_file_new_for_path (epi_icon);
	icons = gs_app_get_icons (app);
	if (icons->len == 0) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "no icons for %s",
			     gs_app_get_id (app));
		return FALSE;
	}
	icon = g_ptr_array_index (icons, 0);
	ret = g_file_make_symbolic_link (symlink_icon,
					 as_icon_get_filename (icon),
					 NULL,
					 &error_local);
	if (!ret) {
		if (g_error_matches (error_local, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
			g_debug ("ignoring icon symlink failure: %s",
				 error_local->message);
		} else {
			g_set_error (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_FAILED,
				     "Can't symlink icon: %s",
				     error_local->message);
			return FALSE;
		}
	}

	/* add desktop file */
	wmclass = g_strdup_printf ("%s-%s", id_nonfull, hash);
	kf = g_key_file_new ();
	g_key_file_set_string (kf,
			       G_KEY_FILE_DESKTOP_GROUP,
			       G_KEY_FILE_DESKTOP_KEY_NAME,
			       gs_app_get_name (app));
	g_key_file_set_string (kf,
			       G_KEY_FILE_DESKTOP_GROUP,
			       G_KEY_FILE_DESKTOP_KEY_COMMENT,
			       gs_app_get_summary (app));
	exec = g_strdup_printf ("epiphany --application-mode --profile=\"%s\" %s",
				epi_dir,
				gs_app_get_url (app, AS_URL_KIND_HOMEPAGE));
	g_key_file_set_string (kf,
			       G_KEY_FILE_DESKTOP_GROUP,
			       G_KEY_FILE_DESKTOP_KEY_EXEC,
			       exec);
	g_key_file_set_boolean (kf,
				G_KEY_FILE_DESKTOP_GROUP,
				G_KEY_FILE_DESKTOP_KEY_STARTUP_NOTIFY,
				TRUE);
	g_key_file_set_boolean (kf,
				G_KEY_FILE_DESKTOP_GROUP,
				G_KEY_FILE_DESKTOP_KEY_TERMINAL,
				FALSE);
	g_key_file_set_boolean (kf,
				G_KEY_FILE_DESKTOP_GROUP,
				G_KEY_FILE_DESKTOP_KEY_NO_DISPLAY,
				FALSE);
	g_key_file_set_string (kf,
			       G_KEY_FILE_DESKTOP_GROUP,
			       G_KEY_FILE_DESKTOP_KEY_TYPE,
			       G_KEY_FILE_DESKTOP_TYPE_APPLICATION);
	g_key_file_set_string (kf,
			       G_KEY_FILE_DESKTOP_GROUP,
			       G_KEY_FILE_DESKTOP_KEY_ICON,
			       epi_icon);
	g_key_file_set_string (kf,
			       G_KEY_FILE_DESKTOP_GROUP,
			       G_KEY_FILE_DESKTOP_KEY_STARTUP_WM_CLASS,
			       wmclass);

	/* save keyfile */
	kf_data = g_key_file_to_data (kf, &kf_length, error);
	if (kf_data == NULL)
		return FALSE;
	epi_desktop = g_strdup_printf ("%s/%s.desktop", epi_dir, wmclass);
	if (!g_file_set_contents (epi_desktop, kf_data, kf_length, error))
		return FALSE;

	/* symlink it to somewhere the shell will notice */
	app_desktop = g_build_filename (g_get_user_data_dir (),
	                                "applications",
	                                gs_app_get_id (app),
	                                NULL);
	symlink_desktop = g_file_new_for_path (app_desktop);
	ret = g_file_make_symbolic_link (symlink_desktop,
					 epi_desktop,
					 NULL,
					 error);
	if (!ret)
		return FALSE;

	/* update state */
	gs_app_set_state (app, AS_APP_STATE_INSTALLING);
	gs_app_set_state (app, AS_APP_STATE_INSTALLED);
	return TRUE;
}

gboolean
gs_plugin_app_remove (GsPlugin *plugin, GsApp *app,
		      GCancellable *cancellable, GError **error)
{
	const gchar *epi_desktop;
	g_autofree gchar *basename = NULL;
	g_autofree gchar *app_desktop = NULL;
	g_autoptr(GFile) file_epi = NULL;
	g_autoptr(GFile) file_app = NULL;

	/* only process this app if was created by this plugin */
	if (g_strcmp0 (gs_app_get_management_plugin (app),
		       gs_plugin_get_name (plugin)) != 0)
		return TRUE;

	/* remove the epi 'config' file */
	gs_app_set_state (app, AS_APP_STATE_REMOVING);
	epi_desktop = gs_app_get_source_id_default (app);
	file_epi = g_file_new_for_path (epi_desktop);
	if (!g_file_delete (file_epi, NULL, error))
		return FALSE;

	/* remove the shared desktop file */
	basename = g_file_get_basename (file_epi);
	app_desktop = g_build_filename (g_get_user_data_dir (),
	                                "applications",
	                                gs_app_get_id (app),
	                                NULL);
	file_app = g_file_new_for_path (app_desktop);
	if (!g_file_delete (file_app, NULL, error))
		return FALSE;
	gs_app_set_state (app, AS_APP_STATE_AVAILABLE);
	return TRUE;
}

gboolean
gs_plugin_refine_app (GsPlugin *plugin,
		      GsApp *app,
		      GsPluginRefineFlags flags,
		      GCancellable *cancellable,
		      GError **error)
{
	const gchar *name;
	g_autofree gchar *fn = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *id_nonfull = NULL;

	/* only process this app if was created by this plugin */
	if (g_strcmp0 (gs_app_get_management_plugin (app),
		       gs_plugin_get_name (plugin)) != 0)
		return TRUE;

	gs_app_set_size_installed (app, 4096);

	/* i guess this is technically true */
	gs_app_add_kudo (app, GS_APP_KUDO_SANDBOXED_SECURE);

	name = gs_app_get_name (app);
	if (name == NULL) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "name unset for %s",
			     gs_app_get_id (app));
		return FALSE;
	}
	if (gs_app_get_summary (app) == NULL) {
		g_debug ("faking summary for %s", gs_app_get_id (app));
		gs_app_set_summary (app, GS_APP_QUALITY_LOWEST,
				    "Web Application");
	}
	hash = g_compute_checksum_for_string (G_CHECKSUM_SHA1, name, -1);
	id_nonfull = _gs_app_get_id_nonfull (app);
	fn = g_strdup_printf ("%s/epiphany/app-%s-%s/%s-%s.desktop",
			      g_get_user_config_dir (),
			      id_nonfull,
			      hash,
			      id_nonfull,
			      hash);
	/* try the new-style location */
	if (!g_file_test (fn, G_FILE_TEST_EXISTS)) {
		g_free (fn);
		fn = g_strdup_printf ("%s/epiphany/app-%s/%s.desktop",
				      g_get_user_config_dir (),
				      id_nonfull, id_nonfull);
	}
	if (g_file_test (fn, G_FILE_TEST_EXISTS)) {
		gs_app_set_state (app, AS_APP_STATE_INSTALLED);
		gs_app_add_source_id (app, fn);
		gs_app_set_management_plugin (app, gs_plugin_get_name (plugin));
		return TRUE;
	}
	gs_app_set_state (app, AS_APP_STATE_AVAILABLE);
	return TRUE;
}

gboolean
gs_plugin_launch (GsPlugin *plugin,
		  GsApp *app,
		  GCancellable *cancellable,
		  GError **error)
{
	/* only process this app if was created by this plugin */
	if (g_strcmp0 (gs_app_get_management_plugin (app),
		       gs_plugin_get_name (plugin)) != 0)
		return TRUE;
	return gs_plugin_app_launch (plugin, app, error);
}
