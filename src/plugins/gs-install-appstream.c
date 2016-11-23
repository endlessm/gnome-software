/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2009-2016 Richard Hughes <richard@hughsie.com>
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
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include <locale.h>
#include <stdlib.h>

#include <appstream-glib.h>
#include <gio/gio.h>
#include <glib/gi18n.h>
#include <glib-object.h>

static gboolean
gs_install_appstream_copy_file (GFile *file, GError **error)
{
	g_autofree gchar *basename = NULL;
	g_autofree gchar *basename_prefixed = NULL;
	g_autofree gchar *cachedir = NULL;
	g_autofree gchar *cachefn = NULL;
	g_autoptr(GFile) cachedir_file = NULL;
	g_autoptr(GFile) cachefn_file = NULL;

	/* make sure the parent directory exists, but if not then create with
	 * the ownership and permissions of the current process */
	cachedir = g_build_filename (LOCALSTATEDIR, "cache", "app-info", "xmls", NULL);
	cachedir_file = g_file_new_for_path (cachedir);
	if (!g_file_query_exists (cachedir_file, NULL)) {
		if (!g_file_make_directory_with_parents (cachedir_file, NULL, error))
			return FALSE;
	}

	/* do the copy, overwriting existing files and setting the permissions
	 * of the current process (so that should be -rw-r--r--) */
	basename = g_file_get_basename (file);
	basename_prefixed = g_strdup_printf ("org.gnome.Software-%s", basename);
	cachefn = g_build_filename (cachedir, basename_prefixed, NULL);
	cachefn_file = g_file_new_for_path (cachefn);
	return g_file_copy (file, cachefn_file,
			    G_FILE_COPY_OVERWRITE |
			    G_FILE_COPY_NOFOLLOW_SYMLINKS |
			    G_FILE_COPY_TARGET_DEFAULT_PERMS,
			    NULL, NULL, NULL, error);
}

static gboolean
gs_install_appstream_check_content_type (GFile *file, GError **error)
{
	const gchar *type;
	g_autoptr(AsStore) store = NULL;
	g_autoptr(GFileInfo) info = NULL;

	/* check is correct type */
	info = g_file_query_info (file,
				  G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
				  G_FILE_QUERY_INFO_NONE,
				  NULL, error);
	if (info == NULL)
		return FALSE;
	type = g_file_info_get_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE);
	if (g_strcmp0 (type, "application/gzip") != 0 &&
	    g_strcmp0 (type, "application/xml") != 0) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_INVALID_DATA,
			     "Invalid type %s: ", type);
		return FALSE;
	}

	/* check is an AppStream file */
	store = as_store_new ();
	if (!as_store_from_file (store, file, NULL, NULL, error))
		return FALSE;
	if (as_store_get_size (store) == 0) {
		g_set_error_literal (error,
				     G_IO_ERROR,
				     G_IO_ERROR_INVALID_DATA,
				     "No applications found in the AppStream XML");
		return FALSE;
	}

	return TRUE;
}

int
main (int argc, char *argv[])
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GFile) file = NULL;
	g_autoptr(GOptionContext) context = NULL;

	/* setup translations */
	setlocale (LC_ALL, "");
	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	context = g_option_context_new (NULL);
	/* TRANSLATORS: tool that is used when copying profiles system-wide */
	g_option_context_set_summary (context, _("GNOME Software AppStream system-wide installer"));
	if (!g_option_context_parse (context, &argc, &argv, &error)) {
		g_print ("%s\n", _("Failed to parse command line arguments"));
		return EXIT_FAILURE;
	}

	/* check input */
	if (g_strv_length (argv) != 2) {
		/* TRANSLATORS: user did not specify a valid filename */
		g_print ("%s\n", _("You need to specify exactly one filename"));
		return EXIT_FAILURE;
	}

	/* check calling process */
	if (getuid () != 0 || geteuid () != 0) {
		/* TRANSLATORS: only able to install files as root */
		g_print ("%s\n", _("This program can only be used by the root user"));
		return EXIT_FAILURE;
	}

	/* check content type for file */
	file = g_file_new_for_path (argv[1]);
	if (!gs_install_appstream_check_content_type (file, &error)) {
		/* TRANSLATORS: error details */
		g_print ("%s: %s\n", _("Failed to validate content type"), error->message);
		return EXIT_FAILURE;
	}

	/* do the copy */
	if (!gs_install_appstream_copy_file (file, &error)) {
		/* TRANSLATORS: error details */
		g_print ("%s: %s\n", _("Failed to copy"), error->message);
		return EXIT_FAILURE;
	}

	/* success */
	return EXIT_SUCCESS;
}
