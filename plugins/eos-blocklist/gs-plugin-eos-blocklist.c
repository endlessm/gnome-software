/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2016, 2017, 2018, 2019 Endless Mobile, Inc
 *
 * Authors:
 *   Joaquim Rocha <jrocha@endlessm.com>
 *   Philip Withnall <withnall@endlessm.com>
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

#include "config.h"

#include <errno.h>
#include <flatpak.h>
#include <gnome-software.h>
#include <glib/gi18n.h>
#include <gs-plugin.h>
#include <gs-utils.h>
#include <locale.h>
#include <math.h>
#include <sys/types.h>
#include <sys/xattr.h>

#define GNOME_DESKTOP_USE_UNSTABLE_API "unstable"
#include <libgnome-desktop/gnome-languages.h>

#define ENDLESS_ID_PREFIX "com.endlessm."

#define EOS_IMAGE_VERSION_XATTR "user.eos-image-version"
#define EOS_IMAGE_VERSION_PATH "/sysroot"
#define EOS_IMAGE_VERSION_ALT_PATH "/"

/*
 * SECTION:
 *
 * Plugin to blocklist certain apps on Endless OS, depending on the OS’s locale,
 * version, or architecture.
 */

struct GsPluginData
{
	char *personality;
	char *product_name;
	FlatpakInstallation *installation;
	char **flatpak_default_locales;
};

static char *
get_image_version_for_path (const char  *path,
                            GError     **error)
{
	ssize_t xattr_size = 0;
	g_autofree char *image_version = NULL;
	int errsv;

	xattr_size = getxattr (path, EOS_IMAGE_VERSION_XATTR, NULL, 0);
	errsv = errno;

	if (xattr_size < 0) {
		g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errsv),
			     "Error when getting xattr ‘%s’ from path ‘%s’: %s",
			     EOS_IMAGE_VERSION_XATTR, path, g_strerror (errsv));
		return NULL;
	}

	image_version = g_malloc0 ((size_t) xattr_size + 1  /* (nul terminator) */);

	xattr_size = getxattr (path, EOS_IMAGE_VERSION_XATTR,
			       image_version, xattr_size);
	errsv = errno;

	/* this check is just in case the xattr has changed in between the
	 * size checks */
	if (xattr_size < 0) {
		g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errsv),
			     "Error when getting xattr ‘%s’ from path ‘%s’: %s",
			     EOS_IMAGE_VERSION_XATTR, path, g_strerror (errsv));
		return NULL;
	}

	return g_steal_pointer (&image_version);
}

static char *
get_image_version (GError **error)
{
	g_autofree char *image_version = NULL;
	g_autoptr(GError) local_error = NULL;

	image_version = get_image_version_for_path (EOS_IMAGE_VERSION_PATH, &local_error);
	if (image_version == NULL)
		image_version = get_image_version_for_path (EOS_IMAGE_VERSION_ALT_PATH, NULL);

	if (image_version == NULL)
		g_propagate_error (error, g_steal_pointer (&local_error));

	return g_steal_pointer (&image_version);
}

static char *
get_personality (GError **error)
{
	g_autofree char *image_version = NULL;
	g_auto(GStrv) tokens = NULL;
	guint num_tokens = 0;
	const char *personality = NULL;

	image_version = get_image_version (error);
	if (image_version == NULL)
		return NULL;

	tokens = g_strsplit (image_version, ".", 0);
	num_tokens = g_strv_length (tokens);

	if (num_tokens < 1) {
		g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			     "Invalid image version: %s", image_version);
		return NULL;
	}

	personality = tokens[num_tokens - 1];

	return g_strdup (personality);
}

static char *
get_product_name (GError **error)
{
	g_autofree char *image_version = NULL;
	char *hyphen_index = NULL;

	image_version = get_image_version (error);
	if (image_version == NULL)
		return NULL;

	hyphen_index = strchr (image_version, '-');
	if (hyphen_index == NULL) {
		g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			     "Invalid image version: %s", image_version);
		return NULL;
	}

	return g_strndup (image_version, hyphen_index - image_version);
}

gboolean
gs_plugin_setup (GsPlugin *plugin,
		 GCancellable *cancellable,
		 GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);

	{
		g_autoptr(GError) local_error = NULL;
		priv->personality = get_personality (&local_error);

		if (local_error != NULL) {
			g_warning ("No system personality could be retrieved! %s", local_error->message);
			g_clear_error (&local_error);
		}

		priv->product_name = get_product_name (&local_error);

		if (local_error != NULL) {
			g_warning ("No system product name could be retrieved! %s", local_error->message);
			g_clear_error (&local_error);
		}

		priv->installation = flatpak_installation_new_system (cancellable, &local_error);
		if (local_error != NULL) {
			g_warning ("No system installation could be retrieved! %s", local_error->message);
			g_clear_error (&local_error);
		}

		if (priv->installation) {
			priv->flatpak_default_locales = flatpak_installation_get_default_locales (priv->installation, &local_error);
			if (local_error != NULL) {
				g_warning ("No user locales could be retrieved! %s", local_error->message);
				g_clear_error (&local_error);
			}
		}
	}

	return TRUE;
}

void
gs_plugin_initialize (GsPlugin *plugin)
{
	gs_plugin_alloc_data (plugin, sizeof(GsPluginData));

	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_AFTER, "appstream");
	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_AFTER, "flatpak");
}

void
gs_plugin_destroy (GsPlugin *plugin)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);

	g_free (priv->personality);
	g_free (priv->product_name);
	g_clear_object (&priv->installation);
	g_strfreev (priv->flatpak_default_locales);
}

/* Copy of the implementation of gs_flatpak_app_get_ref_name(). */
static const gchar *
app_get_flatpak_ref_name (GsApp *app)
{
	return gs_app_get_metadata_item (app, "flatpak::RefName");
}

static gboolean
assert_valid_locale_match (const char *app_locale,
                           const char * const *locale_options)
{
	int idx;
	g_autofree char *app_language_code = NULL;
	g_autofree char *app_territory_code = NULL;
	g_autofree char *app_modifier = NULL;

	if (!gnome_parse_locale (app_locale, &app_language_code, &app_territory_code, NULL, &app_modifier))
		return FALSE;

	for (idx = 0; locale_options[idx] != 0; idx++) {
		g_autofree char *opt_language_code = NULL;
		g_autofree char *opt_territory_code = NULL;
		g_autofree char *opt_modifier = NULL;

		if (!gnome_parse_locale (locale_options[idx], &opt_language_code, &opt_territory_code, NULL, &opt_modifier))
			return FALSE;

		if (g_strcmp0 (app_language_code, opt_language_code) == 0) {
			/* If the main is a match, try to match territory eg. US or GB */
			if (app_territory_code != NULL && opt_territory_code != NULL) {
				if (g_strcmp0 (app_territory_code, opt_territory_code) == 0) {
					/* If the territory is a match, try to match the modifier eg. latin/cyrillic */
					if (app_modifier != NULL && opt_modifier != NULL) {
						if (g_strcmp0 (app_modifier, opt_modifier) == 0)
							return TRUE;
					} else {
						/* If the [modifier] of the app_locale or from the locale options
						 * is not defined, this is a desireable app */
						return TRUE;
					}
				}
			} else {
				/* If the [territory] of the app_locale or from the locale options
				 * is not defined, this is a desireable app */
				return TRUE;
			}
		}
	}

	return FALSE;
}

static gboolean
gs_plugin_locale_is_compatible (GsPlugin *plugin,
                                const char *app_locale)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	g_auto(GStrv) plugin_locale_variants = NULL;
	const char *plugin_locale = setlocale (LC_MESSAGES, NULL);

	/* Check if a variant of a locale is compatible */
	plugin_locale_variants = g_get_locale_variants (plugin_locale);
	if (plugin_locale_variants != NULL && assert_valid_locale_match (app_locale, (const char * const *) plugin_locale_variants))
		return TRUE;

	/* check if the app's locale is compatible with the languages key on the ostree repo file */
	if (priv->flatpak_default_locales != NULL &&
		assert_valid_locale_match (app_locale, (const char * const *) priv->flatpak_default_locales))
		return TRUE;

	return FALSE;
}

static char *
get_app_locale_cache_key (const char *app_name)
{
	guint name_length = strlen (app_name);
	char *suffix = NULL;
	/* locales can be as long as 5 chars (e.g. pt_PT) so  */
	const guint locale_max_length = 5;
	char *locale_cache_name;

	if (name_length <= locale_max_length)
		return NULL;

	locale_cache_name = g_strdup_printf ("locale:%s", app_name);
	/* include the 'locale:' prefix */
	name_length += 7;

	/* get the suffix after the last '.' so we can get
	 * e.g. com.endlessm.FooBar.pt or com.endlessm.FooBar.pt_BR */
	suffix = g_strrstr (locale_cache_name + name_length - locale_max_length,
			    ".");

	if (suffix) {
		/* get the language part of the eventual locale suffix
		 * e.g. pt_BR -> pt */
		char *locale_split = g_strrstr (suffix + 1, "_");

		if (locale_split)
			*locale_split = '\0';
	}

	return locale_cache_name;
}

static gboolean
gs_plugin_app_is_locale_best_match (GsPlugin *plugin,
				    GsApp *app)
{
	return g_str_has_suffix (app_get_flatpak_ref_name (app),
				 setlocale (LC_MESSAGES, NULL));
}

static gboolean
is_same_app (GsApp *app_a, GsApp *app_b)
{
	const char *app_a_id;
	const char *app_b_id;

	if (!app_a || !app_b)
		return FALSE;

	app_a_id = gs_app_get_unique_id (app_a);
	app_b_id = gs_app_get_unique_id (app_b);

	return (app_a == app_b) || (g_strcmp0 (app_a_id, app_b_id) == 0);
}

static void
gs_plugin_update_locale_cache_app (GsPlugin *plugin,
				   const char *locale_cache_key,
				   GsApp *app)
{
	GsApp *cached_app = gs_plugin_cache_lookup (plugin, locale_cache_key);

	/* avoid blocklisting the same app that's already cached */
	if (is_same_app (cached_app, app))
		return;

	if (cached_app && !gs_app_is_installed (cached_app) &&
	    !gs_app_has_category (cached_app, "usb")) {
		const char *app_id = gs_app_get_unique_id (app);
		const char *cached_app_id = gs_app_get_unique_id (cached_app);

		g_debug ("Blocklisting '%s': using '%s' due to its locale",
			 cached_app_id, app_id);
		gs_app_add_quirk (cached_app, GS_APP_QUIRK_HIDE_EVERYWHERE);
	}

	gs_plugin_cache_add (plugin, locale_cache_key, app);
}

static gboolean
gs_plugin_eos_blocklist_kapp_if_needed (GsPlugin *plugin, GsApp *app)
{
	guint endless_prefix_len = strlen (ENDLESS_ID_PREFIX);
	g_autofree char *locale_cache_key = NULL;
	g_auto(GStrv) tokens = NULL;
	const char *last_token = NULL;
	guint num_tokens = 0;
	/* getting the app name, besides skipping the '.desktop' part of the id
	 * also makes sure we're dealing with a Flatpak app */
	const char *app_name = app_get_flatpak_ref_name (app);
	GsApp *cached_app = NULL;

	if (!app_name || !g_str_has_prefix (app_name, ENDLESS_ID_PREFIX))
		return FALSE;

	tokens = g_strsplit (app_name + endless_prefix_len, ".", -1);
	num_tokens = g_strv_length (tokens);

	/* we need at least 2 tokens: app-name & locale */
	if (num_tokens < 2)
		return FALSE;

	/* last token may be the locale */
	last_token = tokens[num_tokens - 1];

	if (!gs_plugin_locale_is_compatible (plugin, last_token) &&
	    !gs_app_has_category (app, "usb")) {
		if (gs_app_is_installed (app))
			return FALSE;

		g_debug ("Blocklisting '%s': incompatible with the current "
			 "locale", gs_app_get_unique_id (app));
		gs_app_add_quirk (app, GS_APP_QUIRK_HIDE_EVERYWHERE);

		return TRUE;
	}

	locale_cache_key = get_app_locale_cache_key (app_name);
	cached_app = gs_plugin_cache_lookup (plugin, locale_cache_key);

	if (is_same_app (cached_app, app))
		return FALSE;

	/* skip if the cached app is already our best */
	if (cached_app &&
	    gs_plugin_app_is_locale_best_match (plugin, cached_app) &&
	    !gs_app_has_category (cached_app, "usb")) {
		if (!gs_app_is_installed (app)) {
			g_debug ("Blocklisting '%s': cached app '%s' is best "
				 "match", gs_app_get_unique_id (app),
				 gs_app_get_unique_id (cached_app));
			gs_app_add_quirk (app, GS_APP_QUIRK_HIDE_EVERYWHERE);
		}

		return TRUE;
	}

	gs_plugin_update_locale_cache_app (plugin, locale_cache_key, app);
	return FALSE;
}

static gboolean
gs_plugin_eos_blocklist_app_for_remote_if_needed (GsPlugin *plugin,
						  GsApp *app)
{
	gboolean do_blocklist = FALSE;

	static const char *duplicated_apps[] = {
		"com.google.Chrome",
		"com.stencyl.Game",
		"org.learningequality.KALite",
		"org.snap4arduino.App",
		NULL
	};

	static const char *core_apps[] = {
		"org.gnome.Evince",
		"org.gnome.Evolution",
		"org.gnome.Nautilus",
		"org.gnome.clocks",
		"org.gnome.eog",
		NULL
	};

	const char *hostname = NULL;
	const char *app_name = NULL;

	if (gs_app_get_scope (app) != AS_COMPONENT_SCOPE_SYSTEM ||
	    gs_app_is_installed (app))
		return FALSE;

	hostname = gs_app_get_origin_hostname (app);
	if (hostname == NULL)
		return FALSE;

	app_name = app_get_flatpak_ref_name (app);
	if (app_name == NULL)
		return FALSE;

	/* We need to check for the app's origin, otherwise we'd be
	 * blocklisting matching apps coming from any repo */
	if (g_strcmp0 (hostname, "sdk.gnome.org") == 0 ||
		   g_strcmp0 (hostname, "flathub.org") == 0 ||
		   g_str_has_suffix (hostname, ".flathub.org")) {

		if (g_strv_contains (duplicated_apps, app_name)) {
			g_debug ("Blocklisting '%s': app is in the duplicated list",
				 gs_app_get_unique_id (app));
			do_blocklist = TRUE;
		} else if (g_strv_contains (core_apps, app_name)) {
			g_debug ("Blocklisting '%s': app is in the core apps list",
				 gs_app_get_unique_id (app));
			do_blocklist = TRUE;
		}
	}

	if (do_blocklist)
		gs_app_add_quirk (app, GS_APP_QUIRK_HIDE_EVERYWHERE);

	return do_blocklist;
}

static void
gs_plugin_eos_remove_blocklist_from_usb_if_needed (GsPlugin *plugin, GsApp *app)
{
	if (!gs_app_has_quirk (app, GS_APP_QUIRK_HIDE_EVERYWHERE) ||
	    !gs_app_has_category (app, "usb"))
		return;

	g_debug ("Removing blocklisting from '%s': app is from USB", gs_app_get_unique_id (app));
	gs_app_remove_quirk (app, GS_APP_QUIRK_HIDE_EVERYWHERE);
}

static gboolean
gs_plugin_eos_blocklist_if_needed (GsPlugin *plugin, GsApp *app)
{
	gboolean blocklist_app = FALSE;
	const char *id = gs_app_get_id (app);

	if (gs_app_get_kind (app) != AS_COMPONENT_KIND_DESKTOP_APP &&
	    gs_app_has_quirk (app, GS_APP_QUIRK_COMPULSORY) &&
	    !gs_app_has_quirk (app, GS_APP_QUIRK_IS_PROXY)) {
		g_debug ("Blocklisting '%s': it's a compulsory, non-desktop app",
			 gs_app_get_unique_id (app));
		blocklist_app = TRUE;
	} else if (gs_app_has_quirk (app, GS_APP_QUIRK_COMPULSORY) &&
		   g_strcmp0 (id, "org.gnome.Software.desktop") == 0) {
		g_debug ("Blocklisting '%s': app is GNOME Software itself",
			 gs_app_get_unique_id (app));
		blocklist_app = TRUE;
	}

	if (blocklist_app)
		gs_app_add_quirk (app, GS_APP_QUIRK_HIDE_EVERYWHERE);

	return blocklist_app;
}

static gboolean
refine_app (GsPlugin             *plugin,
	    GsApp                *app,
	    GsPluginRefineFlags   flags,
	    GCancellable         *cancellable,
	    GError              **error)
{
	/* if we don't know yet the state of an app then we shouldn't
	 * do any further operations on it */
	if (gs_app_get_state (app) == GS_APP_STATE_UNKNOWN)
		return TRUE;

	/* If it’s already blocklisted, there is little we need to do here. */
	if (gs_app_has_quirk (app, GS_APP_QUIRK_HIDE_EVERYWHERE))
		return TRUE;

	if (gs_plugin_eos_blocklist_if_needed (plugin, app))
		return TRUE;

	if (gs_app_get_kind (app) != AS_COMPONENT_KIND_DESKTOP_APP)
		return TRUE;

	if (gs_plugin_eos_blocklist_kapp_if_needed (plugin, app))
		return TRUE;

	if (gs_plugin_eos_blocklist_app_for_remote_if_needed (plugin, app))
		return TRUE;

	gs_plugin_eos_remove_blocklist_from_usb_if_needed (plugin, app);

	return TRUE;
}

gboolean
gs_plugin_refine (GsPlugin             *plugin,
		  GsAppList            *list,
		  GsPluginRefineFlags   flags,
		  GCancellable         *cancellable,
		  GError              **error)
{
	for (guint i = 0; i < gs_app_list_length (list); i++) {
		GsApp *app = gs_app_list_index (list, i);
		if (!refine_app (plugin, app, flags, cancellable, error))
			return FALSE;
	}

	return TRUE;
}

gboolean
gs_plugin_add_category_apps (GsPlugin *plugin,
			     GsCategory *category,
			     GsAppList *list,
			     GCancellable *cancellable,
			     GError **error)
{
	for (guint i = 0; i < gs_app_list_length (list); ++i) {
		GsApp *app = gs_app_list_index (list, i);

		gs_plugin_eos_remove_blocklist_from_usb_if_needed (plugin, app);
	}

	return TRUE;
}
