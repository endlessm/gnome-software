/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2016 Joaquim Rocha <jrocha@endlessm.com>
 * Copyright (C) 2016 Richard Hughes <richard@hughsie.com>
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
#include <string.h>

#include "gs-appstream.h"
#include "gs-flatpak.h"
#include "gs-flatpak-symlinks.h"

struct _GsFlatpak {
	GObject			 parent_instance;
	GsFlatpakFlags		 flags;
	FlatpakInstallation	*installation;
	GHashTable		*broken_remotes;
	GFileMonitor		*monitor;
	AsAppScope		 scope;
	GsPlugin		*plugin;
	AsStore			*store;
	gchar			*id;
	guint			 changed_id;
};

G_DEFINE_TYPE (GsFlatpak, gs_flatpak, G_TYPE_OBJECT)

static gboolean
gs_flatpak_refresh_appstream (GsFlatpak *self, guint cache_age,
			      GsPluginRefreshFlags flags,
			      GCancellable *cancellable, GError **error);

void
gs_plugin_flatpak_error_convert (GError **perror)
{
	GError *error = perror != NULL ? *perror : NULL;

	/* not set */
	if (error == NULL)
		return;

	/* this are allowed for low-level errors */
	if (gs_utils_error_convert_gio (perror))
		return;

	/* this are allowed for low-level errors */
	if (gs_utils_error_convert_gdbus (perror))
		return;

	/* this are allowed for network ops */
	if (gs_utils_error_convert_gresolver (perror))
		return;

	/* custom to this plugin */
	if (error->domain == FLATPAK_ERROR) {
		switch (error->code) {
		case FLATPAK_ERROR_ALREADY_INSTALLED:
		case FLATPAK_ERROR_NOT_INSTALLED:
			error->code = GS_PLUGIN_ERROR_NOT_SUPPORTED;
			break;
		default:
			error->code = GS_PLUGIN_ERROR_FAILED;
			break;
		}
	} else {
		g_warning ("can't reliably fixup error from domain %s",
			   g_quark_to_string (error->domain));
		error->code = GS_PLUGIN_ERROR_FAILED;
	}
	error->domain = GS_PLUGIN_ERROR;
}

static gchar *
gs_flatpak_build_id (FlatpakRef *xref)
{
	if (flatpak_ref_get_kind (xref) == FLATPAK_REF_KIND_APP) {
		return g_strdup_printf ("%s.desktop",
					flatpak_ref_get_name (xref));
	}
	return g_strdup (flatpak_ref_get_name (xref));
}

static GsApp *
gs_flatpak_create_app (GsFlatpak *self, FlatpakRef *xref)
{
	GsApp *app_cached;
	g_autofree gchar *id = NULL;
	g_autoptr(GsApp) app = NULL;

	/* create a temp GsApp */
	id = gs_flatpak_build_id (xref);
	app = gs_app_new (id);
	gs_app_set_bundle_kind (app, AS_BUNDLE_KIND_FLATPAK);
	gs_app_set_branch (app, flatpak_ref_get_branch (xref));
	gs_app_set_flatpak_object_id (app, gs_flatpak_get_id (self));
	if (flatpak_installation_get_is_user (self->installation)) {
		gs_app_set_scope (app, AS_APP_SCOPE_USER);
	} else {
		gs_app_set_scope (app, AS_APP_SCOPE_SYSTEM);
	}
	if (flatpak_ref_get_kind (xref) == FLATPAK_REF_KIND_APP) {
		gs_app_set_kind (app, AS_APP_KIND_DESKTOP);
	} else if (flatpak_ref_get_kind (xref) == FLATPAK_REF_KIND_RUNTIME) {
		gs_app_set_kind (app, AS_APP_KIND_RUNTIME);
	}

	/* we already have one, returned the ref'd cached copy */
	app_cached = gs_plugin_cache_lookup (self->plugin, gs_app_get_unique_id (app));
	if (app_cached != NULL)
		return app_cached;

	/* no existing match, just steal the temp object */
	gs_plugin_cache_add (self->plugin, NULL, app);
	return g_steal_pointer (&app);
}

static void
gs_plugin_flatpak_changed_cb (GFileMonitor *monitor,
			      GFile *child,
			      GFile *other_file,
			      GFileMonitorEvent event_type,
			      GsFlatpak *self)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GError) error_md = NULL;

	/* don't refresh when it's us ourselves doing the change */
	if (gs_plugin_has_flags (self->plugin, GS_PLUGIN_FLAGS_RUNNING_SELF))
		return;

	/* manually drop the cache */
	if (!flatpak_installation_drop_caches (self->installation,
					       NULL, &error)) {
		g_warning ("failed to drop cache: %s", error->message);
		return;
	}

	/* if this is a new remote, get the AppStream data */
	if (!gs_flatpak_refresh_appstream (self, G_MAXUINT, 0, NULL, &error_md)) {
		g_warning ("failed to get initial available data: %s",
			   error_md->message);
	}
}

static void
gs_flatpak_remove_prefixed_names (AsApp *app)
{
	GHashTable *names;
	GList *l;
	g_autoptr(GList) keys = NULL;

	names = as_app_get_names (app);
	keys = g_hash_table_get_keys (names);
	for (l = keys; l != NULL; l = l->next) {
		const gchar *locale = l->data;
		const gchar *value = g_hash_table_lookup (names, locale);
		if (value == NULL)
			continue;
		if (!g_str_has_prefix (value, "(Nightly) "))
			continue;
		as_app_set_name (app, locale, value + 10);
	}
}

static gboolean
remote_is_eos_apps (FlatpakRemote *remote)
{
	g_autoptr(SoupURI) soup_uri = NULL;
	const char *remote_host = NULL;
	const char *remote_path = NULL;
	g_autofree char *remote_url = NULL;

	if (g_strcmp0 (flatpak_remote_get_name (remote), "eos-apps") != 0)
		return FALSE;

	remote_url = flatpak_remote_get_url (remote);
	if (remote_url == NULL)
		return FALSE;

	soup_uri = soup_uri_new (remote_url);
	remote_host = soup_uri_get_host (soup_uri);
	remote_path = soup_uri_get_path (soup_uri);

	/* support *ostree.endlessm.com domains with '/eos-apps' end points */
	return remote_path != NULL &&
		(g_strstr_len (remote_path, -1, "eos-apps") != NULL) &&
		g_str_has_suffix (remote_host, "ostree.endlessm.com");
}

static gboolean
gs_flatpak_add_apps_from_xremote (GsFlatpak *self,
				  FlatpakRemote *xremote,
				  GCancellable *cancellable,
				  GError **error)
{
	GPtrArray *apps;
	guint i;
	g_autofree gchar *appstream_dir_fn = NULL;
	g_autofree gchar *appstream_fn = NULL;
	g_autofree gchar *default_branch = NULL;
	g_autofree gchar *only_app_id = NULL;
	g_autoptr(AsProfileTask) ptask = NULL;
	g_autoptr(AsStore) store = NULL;
	g_autoptr(GFile) appstream_dir = NULL;
	g_autoptr(GFile) file = NULL;
	g_autoptr(GSettings) settings = NULL;
	g_autoptr(GPtrArray) app_filtered = NULL;

	/* profile */
	ptask = as_profile_start (gs_plugin_get_profile (self->plugin),
				  "%s::add-apps-from-remote{%s}",
				  gs_flatpak_get_id (self),
				  flatpak_remote_get_name (xremote));
	g_assert (ptask != NULL);

	/* get the AppStream data location */
	appstream_dir = flatpak_remote_get_appstream_dir (xremote, NULL);
	if (appstream_dir == NULL) {
		g_debug ("no appstream dir for %s, skipping",
			 flatpak_remote_get_name (xremote));
		return TRUE;
	}

	/* load the file into a temp store */
	appstream_dir_fn = g_file_get_path (appstream_dir);
	appstream_fn = g_build_filename (appstream_dir_fn,
					 "appstream.xml.gz", NULL);
	if (!g_file_test (appstream_fn, G_FILE_TEST_EXISTS)) {
		g_debug ("no %s appstream metadata found: %s",
			 flatpak_remote_get_name (xremote),
			 appstream_fn);
		return TRUE;
	}
	file = g_file_new_for_path (appstream_fn);
	store = as_store_new ();
	as_store_set_add_flags (store,
				AS_STORE_ADD_FLAG_USE_UNIQUE_ID |
				AS_STORE_ADD_FLAG_ONLY_NATIVE_LANGS);
	as_store_set_search_match (store,
				   AS_APP_SEARCH_MATCH_MIMETYPE |
				   AS_APP_SEARCH_MATCH_PKGNAME |
				   AS_APP_SEARCH_MATCH_COMMENT |
				   AS_APP_SEARCH_MATCH_NAME |
				   AS_APP_SEARCH_MATCH_KEYWORD |
				   AS_APP_SEARCH_MATCH_ID);
	if (!as_store_from_file (store, file, NULL, cancellable, error)) {
		gs_utils_error_convert_appstream (error);
		return FALSE;
	}

	/* ensure the token cache */
	as_store_load_search_cache (store);

	/* only add the specific app for noenumerate=true */
	if (flatpak_remote_get_noenumerate (xremote)) {
		g_autofree gchar *tmp = NULL;
		tmp = g_strdup (flatpak_remote_get_name (xremote));
		g_strdelimit (tmp, "-", '\0');
		only_app_id = g_strdup_printf ("%s.desktop", tmp);
	}

	/* do we want to filter to the default branch */
	settings = g_settings_new ("org.gnome.software");
	if (g_settings_get_boolean (settings, "filter-default-branch"))
		default_branch = flatpak_remote_get_default_branch (xremote);

	/* on Endless we rely heavily on always having the default branch
	 * defined for the 'eos-apps' repository, so we need a fallback value
	 * to cover the case where GS is run without being connected to the
	 * Internet before (otherwise it would have pulled it from the server).
	 */
	if (default_branch == NULL && self->scope == AS_APP_SCOPE_SYSTEM &&
	    remote_is_eos_apps (xremote)) {
		g_warning ("No default branch configured for Endless eos-apps remote! "
			   "Using fallback value");
		default_branch = g_strdup ("eos3");
	}

	/* get all the apps and fix them up */
	apps = as_store_get_apps (store);
	app_filtered = g_ptr_array_new ();
	for (i = 0; i < apps->len; i++) {
		AsApp *app = g_ptr_array_index (apps, i);

		/* filter to app */
		if (only_app_id != NULL &&
		    g_strcmp0 (as_app_get_id (app), only_app_id) != 0) {
			as_app_set_kind (app, AS_APP_KIND_UNKNOWN);
			continue;
		}

		/* filter by branch */
		if (default_branch != NULL &&
		    g_strcmp0 (as_app_get_branch (app), default_branch) != 0) {
			g_debug ("not adding app with branch %s as filtering to %s",
				 as_app_get_branch (app), default_branch);
			continue;
		}

		/* fix the names when using old versions of appstream-compose */
		gs_flatpak_remove_prefixed_names (app);

		/* add */
		as_app_set_scope (app, self->scope);
		as_app_set_origin (app, flatpak_remote_get_name (xremote));
		as_app_add_keyword (app, NULL, "flatpak");
		g_debug ("adding %s", as_app_get_unique_id (app));
		g_ptr_array_add (app_filtered, app);
	}

	/* add them to the main store */
	as_store_add_apps (self->store, app_filtered);
	return TRUE;
}

static gchar *
gs_flatpak_discard_desktop_suffix (const gchar *app_id)
{
	const gchar *desktop_suffix = ".desktop";
	guint app_prefix_len;

	if (!g_str_has_suffix (app_id, desktop_suffix))
		return g_strdup (app_id);

	app_prefix_len = strlen (app_id) - strlen (desktop_suffix);
	return g_strndup (app_id, app_prefix_len);
}

static void
gs_flatpak_rescan_installed (GsFlatpak *self,
			     GCancellable *cancellable,
			     GError **error)
{
	GPtrArray *icons;
	const gchar *fn;
	guint i;
	g_autoptr(AsProfileTask) ptask = NULL;
	g_autoptr(GFile) path = NULL;
	g_autoptr(GDir) dir = NULL;
	g_autofree gchar *path_str = NULL;
	g_autofree gchar *path_exports = NULL;
	g_autofree gchar *path_apps = NULL;

	/* profile */
	ptask = as_profile_start (gs_plugin_get_profile (self->plugin),
				  "%s::rescan-installed",
				  gs_flatpak_get_id (self));
	g_assert (ptask != NULL);

	/* add all installed desktop files */
	path = flatpak_installation_get_path (self->installation);
	path_str = g_file_get_path (path);
	path_exports = g_build_filename (path_str, "exports", NULL);
	path_apps = g_build_filename (path_exports, "share", "applications", NULL);
	dir = g_dir_open (path_apps, 0, NULL);
	if (dir == NULL)
		return;
	while ((fn = g_dir_read_name (dir)) != NULL) {
		g_autofree gchar *fn_desktop = NULL;
		g_autoptr(GError) error_local = NULL;
		g_autoptr(AsApp) app = NULL;
		g_autoptr(FlatpakInstalledRef) app_ref = NULL;
		g_autofree gchar *app_id = NULL;

		/* ignore */
		if (g_strcmp0 (fn, "mimeinfo.cache") == 0)
			continue;

		/* parse desktop files */
		app = as_app_new ();
		fn_desktop = g_build_filename (path_apps, fn, NULL);
		if (!as_app_parse_file (app, fn_desktop, 0, &error_local)) {
			g_warning ("failed to parse %s: %s",
				   fn_desktop, error_local->message);
			continue;
		}

		/* fix up icons */
		icons = as_app_get_icons (app);
		for (i = 0; i < icons->len; i++) {
			AsIcon *ic = g_ptr_array_index (icons, i);
			if (as_icon_get_kind (ic) == AS_ICON_KIND_UNKNOWN) {
				as_icon_set_kind (ic, AS_ICON_KIND_STOCK);
				as_icon_set_prefix (ic, path_exports);
			}
		}

		/* fix the names when using old versions of appstream-compose */
		gs_flatpak_remove_prefixed_names (app);

		/* add */
		as_app_set_state (app, AS_APP_STATE_INSTALLED);
		as_app_set_scope (app, self->scope);
#if AS_CHECK_VERSION(0,6,9)
{
		g_autoptr(AsFormat) format = as_format_new ();
		as_format_set_kind (format, AS_FORMAT_KIND_DESKTOP);
		as_format_set_filename (format, fn_desktop);
		as_app_add_format (app, format);
}
#else
		as_app_set_source_kind (app, AS_APP_SOURCE_KIND_DESKTOP);
		as_app_set_source_file (app, fn_desktop);
#endif
		app_id = gs_flatpak_discard_desktop_suffix (fn);
		app_ref = flatpak_installation_get_current_installed_app (self->installation,
									  app_id,
									  cancellable,
									  &error_local);
		if (app_ref == NULL) {
			g_warning ("Could not get app (from ID '%s') for installed desktop "
				   "file %s: %s", app_id, fn_desktop, error_local->message);
			continue;
		}

		as_app_set_branch (app, flatpak_ref_get_branch (FLATPAK_REF (app_ref)));
		as_app_set_icon_path (app, path_exports);
		as_app_add_keyword (app, NULL, "flatpak");
		as_store_add_app (self->store, app);
	}
}

static gboolean
gs_flatpak_rescan_appstream_store (GsFlatpak *self,
				   GCancellable *cancellable,
				   GError **error)
{
	guint i;
	g_autoptr(AsProfileTask) ptask = NULL;
	g_autoptr(GPtrArray) xremotes = NULL;

	/* profile */
	ptask = as_profile_start (gs_plugin_get_profile (self->plugin),
				  "%s::rescan-appstream",
				  gs_flatpak_get_id (self));
	g_assert (ptask != NULL);

	/* remove all components */
	as_store_remove_all (self->store);

	/* go through each remote adding metadata */
	xremotes = flatpak_installation_list_remotes (self->installation,
						      cancellable,
						      error);
	if (xremotes == NULL) {
		gs_plugin_flatpak_error_convert (error);
		return FALSE;
	}
	for (i = 0; i < xremotes->len; i++) {
		FlatpakRemote *xremote = g_ptr_array_index (xremotes, i);
		if (flatpak_remote_get_disabled (xremote))
			continue;
		g_debug ("found remote %s",
			 flatpak_remote_get_name (xremote));
		if (!gs_flatpak_add_apps_from_xremote (self, xremote, cancellable, error))
			return FALSE;
	}

	/* add any installed files without AppStream info */
	gs_flatpak_rescan_installed (self, cancellable, error);

	return TRUE;
}

static void
ensure_default_branches (GsFlatpak *self)
{
	g_autoptr(GPtrArray) xremotes = NULL;
	guint i;

	xremotes = flatpak_installation_list_remotes (self->installation, NULL, NULL);
	if (xremotes == NULL)
		return;

	for (i = 0; i < xremotes->len; i++) {
		FlatpakRemote *xremote = g_ptr_array_index (xremotes, i);
		const gchar *remote_name;
		g_autofree char *default_branch = NULL;
		g_autoptr(GError) error_local = NULL;

		/* we only try to ensure the default branch is set for the repos
		 * we officially support as other repos, especially the ones with
		 * only runtimes may not have the default branch set, and trying
		 * to fetch it takes a considerable amount of time */
		if (!remote_is_eos_apps (xremote))
			continue;

		default_branch = flatpak_remote_get_default_branch (xremote);
		if (default_branch != NULL)
			continue;

		remote_name = flatpak_remote_get_name (xremote);
		if (!flatpak_installation_update_remote_sync (self->installation,
							      remote_name, NULL,
							      &error_local)) {
			g_warning ("failed to update metadata from remote %s: %s",
				   remote_name, error_local->message);
		}
	}
}

gboolean
gs_flatpak_setup (GsFlatpak *self, GCancellable *cancellable, GError **error)
{
	g_autoptr(AsProfileTask) ptask = NULL;
	g_autoptr(GError) error_md = NULL;

	ptask = as_profile_start (gs_plugin_get_profile (self->plugin),
				  "%s::setup",
				  gs_flatpak_get_id (self));
	g_assert (ptask != NULL);

	if (!gs_flatpak_refresh_appstream (self, G_MAXUINT, 0,
					   cancellable,
					   &error_md)) {
		g_warning ("failed to get initial available data on setup: %s",
			   error_md->message);
	}

	ensure_default_branches (self);

	/* load just the wildcards */
	if (!as_store_load (self->store,
			    AS_STORE_LOAD_FLAG_ONLY_MERGE_APPS |
			    AS_STORE_LOAD_FLAG_ONLY_UNCOMPRESSED |
			    AS_STORE_LOAD_FLAG_APP_INFO_USER |
			    AS_STORE_LOAD_FLAG_APP_INFO_SYSTEM,
			    cancellable, error)) {
		gs_utils_error_convert_appstream (error);
		return FALSE;
	}


	/* watch for changes */
	self->monitor = flatpak_installation_create_monitor (self->installation,
							     cancellable,
							     error);
	if (self->monitor == NULL) {
		gs_plugin_flatpak_error_convert (error);
		return FALSE;
	}
	self->changed_id =
		g_signal_connect (self->monitor, "changed",
				  G_CALLBACK (gs_plugin_flatpak_changed_cb), self);

	/* ensure the legacy AppStream symlink cache is deleted */
	if (!gs_flatpak_symlinks_cleanup (self->installation, cancellable, error))
		return FALSE;

	/* success */
	return TRUE;
}

static gboolean
gs_flatpak_refresh_appstream_remote (GsFlatpak *self,
				     const gchar *remote_name,
				     GCancellable *cancellable,
				     GError **error)
{
	g_autoptr(AsProfileTask) ptask = NULL;
	ptask = as_profile_start (gs_plugin_get_profile (self->plugin),
				  "%s::refresh-appstream{%s}",
				  gs_flatpak_get_id (self),
				  remote_name);
	g_assert (ptask != NULL);
	if (!flatpak_installation_update_appstream_sync (self->installation,
							 remote_name,
							 NULL,
							 NULL,
							 cancellable,
							 error)) {
		gs_plugin_flatpak_error_convert (error);
		return FALSE;
	}
	return TRUE;
}

static gboolean
gs_flatpak_refresh_appstream (GsFlatpak *self, guint cache_age,
			      GsPluginRefreshFlags flags,
			      GCancellable *cancellable, GError **error)
{
	gboolean ret;
	gboolean something_changed = FALSE;
	guint i;
	g_autoptr(AsProfileTask) ptask = NULL;
	g_autoptr(GPtrArray) xremotes = NULL;

	/* profile */
	ptask = as_profile_start (gs_plugin_get_profile (self->plugin),
				  "%s::refresh-appstream",
				  gs_flatpak_get_id (self));
	g_assert (ptask != NULL);

	/* do not care */
	if (self->flags & GS_FLATPAK_FLAG_IS_TEMPORARY)
		return TRUE;

	/* get remotes */
	xremotes = flatpak_installation_list_remotes (self->installation,
						      cancellable,
						      error);
	if (xremotes == NULL) {
		gs_plugin_flatpak_error_convert (error);
		return FALSE;
	}
	for (i = 0; i < xremotes->len; i++) {
		const gchar *remote_name;
		guint tmp;
		g_autoptr(GError) error_local = NULL;
		g_autoptr(GFile) file = NULL;
		g_autoptr(GFile) file_timestamp = NULL;
		g_autofree gchar *appstream_fn = NULL;
		FlatpakRemote *xremote = g_ptr_array_index (xremotes, i);

		/* not enabled */
		if (flatpak_remote_get_disabled (xremote))
			continue;

		/* skip known-broken repos */
		remote_name = flatpak_remote_get_name (xremote);
		if (g_hash_table_lookup (self->broken_remotes, remote_name) != NULL) {
			g_debug ("skipping known broken remote: %s", remote_name);
			continue;
		}

		/* is the timestamp new enough */
		file_timestamp = flatpak_remote_get_appstream_timestamp (xremote, NULL);
		tmp = gs_utils_get_file_age (file_timestamp);
		if (tmp < cache_age) {
			g_autofree gchar *fn = g_file_get_path (file_timestamp);
			g_debug ("%s is only %u seconds old, so ignoring refresh",
				 fn, tmp);
			continue;
		}

		/* download new data */
		g_debug ("%s is %u seconds old, so downloading new data",
			 remote_name, tmp);
		ret = gs_flatpak_refresh_appstream_remote (self,
							   remote_name,
							   cancellable,
							   &error_local);
		if (!ret) {
			g_warning ("Failed to get AppStream metadata: %s",
				   error_local->message);
			/* don't try to fetch this again until refresh() */
			g_hash_table_insert (self->broken_remotes,
					     g_strdup (remote_name),
					     GUINT_TO_POINTER (1));
			continue;
		}

		/* add the new AppStream repo to the shared store */
		file = flatpak_remote_get_appstream_dir (xremote, NULL);
		appstream_fn = g_file_get_path (file);
		g_debug ("using AppStream metadata found at: %s", appstream_fn);

		/* trigger the symlink rebuild */
		something_changed = TRUE;
	}

	/* ensure the AppStream store is up to date */
	if (something_changed ||
	    as_store_get_size (self->store) == 0) {
		if (!gs_flatpak_rescan_appstream_store (self, cancellable, error))
			return FALSE;
	}

	return TRUE;
}

static void
gs_app_set_flatpak_kind (GsApp *app, FlatpakRefKind kind)
{
	if (kind == FLATPAK_REF_KIND_APP)
		gs_app_set_metadata (app, "flatpak::kind", "app");
	else if (kind == FLATPAK_REF_KIND_RUNTIME)
		gs_app_set_metadata (app, "flatpak::kind", "runtime");
	else
		g_assert_not_reached ();
}

static void
gs_plugin_refine_item_scope (GsFlatpak *self, GsApp *app)
{
	if (gs_app_get_scope (app) == AS_APP_SCOPE_UNKNOWN) {
		gboolean is_user = flatpak_installation_get_is_user (self->installation);
		gs_app_set_scope (app, is_user ? AS_APP_SCOPE_USER : AS_APP_SCOPE_SYSTEM);
	}
}

static void
gs_flatpak_set_metadata (GsFlatpak *self, GsApp *app, FlatpakRef *xref)
{
	gs_app_set_bundle_kind (app, AS_BUNDLE_KIND_FLATPAK);
	gs_app_set_management_plugin (app, gs_plugin_get_name (self->plugin));
	gs_app_set_branch (app, flatpak_ref_get_branch (xref));
	gs_app_set_flatpak_kind (app, flatpak_ref_get_kind (xref));
	gs_app_set_flatpak_name (app, flatpak_ref_get_name (xref));
	gs_app_set_flatpak_arch (app, flatpak_ref_get_arch (xref));
	gs_app_set_flatpak_branch (app, flatpak_ref_get_branch (xref));
	gs_app_set_flatpak_commit (app, flatpak_ref_get_commit (xref));
	gs_plugin_refine_item_scope (self, app);
}

static void
gs_flatpak_set_metadata_installed (GsFlatpak *self, GsApp *app,
				   FlatpakInstalledRef *xref)
{
	guint64 mtime;
	guint64 size_installed;
	g_autofree gchar *metadata_fn = NULL;
	g_autoptr(GFile) file = NULL;
	g_autoptr(GFileInfo) info = NULL;

	/* for all types */
	gs_flatpak_set_metadata (self, app, FLATPAK_REF (xref));
	if (gs_app_get_metadata_item (app, "GnomeSoftware::Creator") == NULL) {
		gs_app_set_metadata (app, "GnomeSoftware::Creator",
				     gs_plugin_get_name (self->plugin));
	}

	/* get the last time the app was updated */
	metadata_fn = g_build_filename (flatpak_installed_ref_get_deploy_dir (xref),
					"..",
					"active",
					"metadata",
					NULL);
	file = g_file_new_for_path (metadata_fn);
	info = g_file_query_info (file,
				  G_FILE_ATTRIBUTE_TIME_MODIFIED,
				  G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
				  NULL, NULL);
	if (info != NULL) {
		mtime = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
		gs_app_set_install_date (app, mtime);
	}

	/* this is faster than resolving */
	if (gs_app_get_origin (app) == NULL)
		gs_app_set_origin (app, flatpak_installed_ref_get_origin (xref));

	/* this is faster than flatpak_installation_fetch_remote_size_sync() */
	size_installed = flatpak_installed_ref_get_installed_size (xref);
	if (size_installed != 0)
		gs_app_set_size_installed (app, size_installed);
}

static GsApp *
gs_flatpak_create_installed (GsFlatpak *self,
			     FlatpakInstalledRef *xref,
			     GError **error)
{
	g_autoptr(AsIcon) icon = NULL;
	g_autoptr(GsApp) app = NULL;

	g_return_val_if_fail (xref != NULL, NULL);

	/*
	 * Only show the current application in GNOME Software
	 *
	 * You can have multiple versions/branches of a particular app-id
	 * installed but only one of them is "current" where this means:
	 *  1) the default to launch unless you specify a version
	 *  2) The one that gets its exported files exported
	 */
	if (!flatpak_installed_ref_get_is_current (xref) &&
	    flatpak_ref_get_kind (FLATPAK_REF(xref)) == FLATPAK_REF_KIND_APP) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_NOT_SUPPORTED,
			     "%s not current, ignoring",
			     flatpak_ref_get_name (FLATPAK_REF (xref)));
		return NULL;
	}

	/* create new object */
	app = gs_flatpak_create_app (self, FLATPAK_REF (xref));
	gs_flatpak_set_metadata_installed (self, app, xref);

	switch (flatpak_ref_get_kind (FLATPAK_REF(xref))) {
	case FLATPAK_REF_KIND_APP:
		gs_app_set_kind (app, AS_APP_KIND_DESKTOP);
		break;
	case FLATPAK_REF_KIND_RUNTIME:
		gs_app_set_flatpak_kind (app, FLATPAK_REF_KIND_RUNTIME);
		gs_app_set_kind (app, AS_APP_KIND_RUNTIME);
		gs_app_set_name (app, GS_APP_QUALITY_NORMAL,
				 flatpak_ref_get_name (FLATPAK_REF (xref)));
		gs_app_set_summary (app, GS_APP_QUALITY_NORMAL,
				    "Framework for applications");
		gs_app_set_version (app, flatpak_ref_get_branch (FLATPAK_REF (xref)));
		icon = as_icon_new ();
		as_icon_set_kind (icon, AS_ICON_KIND_STOCK);
		as_icon_set_name (icon, "system-run-symbolic");
		gs_app_add_icon (app, icon);
		break;
	default:
		g_set_error_literal (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_NOT_SUPPORTED,
				     "FlatpakRefKind not known");
		return NULL;
	}
	return g_object_ref (app);
}

gboolean
gs_flatpak_add_installed (GsFlatpak *self, GsAppList *list,
			  GCancellable *cancellable,
			  GError **error)
{
	g_autoptr(GPtrArray) xrefs = NULL;
	guint i;

	/* get apps and runtimes */
	xrefs = flatpak_installation_list_installed_refs (self->installation,
							  cancellable, error);
	if (xrefs == NULL) {
		gs_plugin_flatpak_error_convert (error);
		return FALSE;
	}
	for (i = 0; i < xrefs->len; i++) {
		FlatpakInstalledRef *xref = g_ptr_array_index (xrefs, i);
		g_autoptr(GError) error_local = NULL;
		g_autoptr(GsApp) app = NULL;

		/* only apps */
		if (flatpak_ref_get_kind (FLATPAK_REF (xref)) != FLATPAK_REF_KIND_APP)
			continue;

		app = gs_flatpak_create_installed (self, xref, &error_local);
		if (app == NULL) {
			g_warning ("failed to add flatpak: %s", error_local->message);
			continue;
		}
		if (gs_app_get_state (app) == AS_APP_STATE_UNKNOWN)
			gs_app_set_state (app, AS_APP_STATE_INSTALLED);
		gs_app_list_add (list, app);
	}

	return TRUE;
}

gboolean
gs_flatpak_add_sources (GsFlatpak *self, GsAppList *list,
			GCancellable *cancellable,
			GError **error)
{
	g_autoptr(GPtrArray) xrefs = NULL;
	g_autoptr(GPtrArray) xremotes = NULL;
	guint i;
	guint j;

	/* get installed apps and runtimes */
	xrefs = flatpak_installation_list_installed_refs (self->installation,
							  cancellable,
							  error);
	if (xrefs == NULL) {
		gs_plugin_flatpak_error_convert (error);
		return FALSE;
	}

	/* get available remotes */
	xremotes = flatpak_installation_list_remotes (self->installation,
						      cancellable,
						      error);
	if (xremotes == NULL) {
		gs_plugin_flatpak_error_convert (error);
		return FALSE;
	}
	for (i = 0; i < xremotes->len; i++) {
		FlatpakRemote *xremote = g_ptr_array_index (xremotes, i);
		g_autoptr(GsApp) app = NULL;
		g_autofree gchar *url = NULL;
		g_autofree gchar *title = NULL;;

		/* apps installed from bundles add their own remote that only
		 * can be used for updating that app only -- so hide them */
		if (flatpak_remote_get_noenumerate (xremote))
			continue;

		/* create both enabled and disabled and filter in the UI */
		app = gs_app_new (flatpak_remote_get_name (xremote));
		gs_app_set_management_plugin (app, gs_plugin_get_name (self->plugin));
		gs_app_set_kind (app, AS_APP_KIND_SOURCE);
		gs_app_set_state (app, flatpak_remote_get_disabled (xremote) ?
				  AS_APP_STATE_AVAILABLE : AS_APP_STATE_INSTALLED);
		gs_app_add_quirk (app, AS_APP_QUIRK_NOT_LAUNCHABLE);
		gs_app_set_name (app,
				 GS_APP_QUALITY_LOWEST,
				 flatpak_remote_get_name (xremote));

		/* title */
		title = flatpak_remote_get_title (xremote);
		if (title != NULL)
			gs_app_set_summary (app, GS_APP_QUALITY_LOWEST, title);

		/* url */
		url = flatpak_remote_get_url (xremote);
		if (url != NULL)
			gs_app_set_url (app, AS_URL_KIND_HOMEPAGE, url);
		gs_app_list_add (list, app);

		/* add related apps, i.e. what was installed from there */
		for (j = 0; j < xrefs->len; j++) {
			FlatpakInstalledRef *xref = g_ptr_array_index (xrefs, j);
			g_autoptr(GError) error_local = NULL;
			g_autoptr(GsApp) related = NULL;

			/* only apps */
			if (flatpak_ref_get_kind (FLATPAK_REF (xref)) != FLATPAK_REF_KIND_APP)
				continue;
			if (g_strcmp0 (flatpak_installed_ref_get_origin (xref),
				       flatpak_remote_get_name (xremote)) != 0)
				continue;
			related = gs_flatpak_create_installed (self,
							       xref,
							       &error_local);
			if (related == NULL) {
				g_warning ("failed to add flatpak: %s",
					   error_local->message);
				continue;
			}
			gs_app_set_state (related, AS_APP_STATE_INSTALLED);
			gs_app_add_related (app, related);
		}
	}
	return TRUE;
}

static gboolean
gs_flatpak_app_install_source (GsFlatpak *self, GsApp *app,
			       GCancellable *cancellable,
			       GError **error)
{
	const gchar *gpg_key;
	const gchar *branch;
	g_autoptr(FlatpakRemote) xremote = NULL;

	/* does the remote already exist and is disabled */
	xremote = flatpak_installation_get_remote_by_name (self->installation,
							   gs_app_get_id (app),
							   cancellable, NULL);
	if (xremote != NULL) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "flatpak source %s already exists",
			     flatpak_remote_get_name (xremote));
		return FALSE;
	}

	/* create a new remote */
	xremote = flatpak_remote_new (gs_app_get_id (app));
	flatpak_remote_set_url (xremote, gs_app_get_metadata_item (app, "flatpak::url"));
	flatpak_remote_set_noenumerate (xremote, FALSE);
	if (gs_app_get_summary (app) != NULL)
		flatpak_remote_set_title (xremote, gs_app_get_summary (app));

	/* decode GPG key if set */
	gpg_key = gs_app_get_metadata_item (app, "flatpak::gpg-key");
	if (gpg_key != NULL && g_strcmp0 (gpg_key, "FOOBAR==") != 0) {
		gsize data_len = 0;
		g_autofree guchar *data = NULL;
		g_autoptr(GBytes) bytes = NULL;
		data = g_base64_decode (gpg_key, &data_len);
		bytes = g_bytes_new (data, data_len);
		flatpak_remote_set_gpg_verify (xremote, TRUE);
		flatpak_remote_set_gpg_key (xremote, bytes);
	} else {
		flatpak_remote_set_gpg_verify (xremote, FALSE);
	}

	/* default branch */
	branch = gs_app_get_branch (app);
	if (branch != NULL)
		flatpak_remote_set_default_branch (xremote, branch);

	/* install it */
	gs_app_set_state (app, AS_APP_STATE_INSTALLING);
	if (!flatpak_installation_modify_remote (self->installation,
						 xremote,
						 cancellable,
						 error)) {
		gs_plugin_flatpak_error_convert (error);
		g_prefix_error (error, "cannot modify remote: ");
		gs_app_set_state_recover (app);
		return FALSE;
	}

	/* refresh the AppStream data manually */
	if (!gs_flatpak_add_apps_from_xremote (self, xremote, cancellable, error)) {
		g_prefix_error (error, "cannot refresh remote AppStream: ");
		return FALSE;
	}

	/* success */
	gs_app_set_state (app, AS_APP_STATE_INSTALLED);
	return TRUE;
}

/* return a new list which contains all the GsApps that are not related to
 * each other */
static GsAppList *
gs_flatpak_list_convert_related (GsFlatpak *self, GsAppList *list,
				 GCancellable *cancellable,
				 GError **error)
{
	g_autoptr(GHashTable) hash_rel = NULL;
	g_autoptr(GsAppList) list_new = gs_app_list_new ();

	/* use a hash to map the related package back to the main package */
	hash_rel = g_hash_table_new_full (g_str_hash, g_str_equal,
					  g_free, (GDestroyNotify) g_object_unref);
	for (guint i = 0; i < gs_app_list_length (list); i++) {
		GsApp *app = gs_app_list_index (list, i);
		const gchar *remote_name = gs_app_get_origin (app);
		g_autoptr(GPtrArray) related_xrefs = NULL;
		g_autofree gchar *ref = NULL;

		/* lookup any related refs for this ref */
		ref = g_strdup_printf ("%s/%s/%s/%s",
				       gs_app_get_flatpak_kind_as_str (app),
				       gs_app_get_flatpak_name (app),
				       gs_app_get_flatpak_arch (app),
				       gs_app_get_flatpak_branch (app));
		related_xrefs = flatpak_installation_list_installed_related_refs_sync (self->installation,
										       remote_name,
										       ref,
										       cancellable,
										       error);
		if (related_xrefs == NULL) {
			gs_plugin_flatpak_error_convert (error);
			return NULL;
		}
		for (guint j = 0; j < related_xrefs->len; j++) {
			FlatpakRelatedRef *xref_rel = g_ptr_array_index (related_xrefs, j);
			g_autoptr(GsApp) app_rel = NULL;

			/* not related */
			if (!flatpak_related_ref_should_download (xref_rel))
				continue;

			/* keep track of this so we don't add it on its own */
			app_rel = gs_flatpak_create_app (self, FLATPAK_REF (xref_rel));
			g_hash_table_insert (hash_rel,
					     g_strdup (gs_app_get_unique_id (app_rel)),
					     g_object_ref (app));
		}
	}

	/* is this related to something else */
	for (guint i = 0; i < gs_app_list_length (list); i++) {
		GsApp *app = gs_app_list_index (list, i);
		GsApp *app_parent;
		app_parent = g_hash_table_lookup (hash_rel, gs_app_get_unique_id (app));
		if (app_parent != NULL) {
			g_debug ("adding %s as related to %s",
				 gs_app_get_unique_id (app),
				 gs_app_get_unique_id (app_parent));
			gs_app_add_related (app_parent, app);
		} else {
			gs_app_list_add (list_new, app);
		}
	}

	/* success */
	return g_steal_pointer (&list_new);
}

gboolean
gs_flatpak_add_updates (GsFlatpak *self, GsAppList *list,
			GCancellable *cancellable,
			GError **error)
{
	g_autoptr(GPtrArray) xrefs = NULL;
	g_autoptr(GsAppList) list_new = NULL;
	g_autoptr(GsAppList) list_tmp = NULL;

	/* get all the installed apps (no network I/O) */
	xrefs = flatpak_installation_list_installed_refs (self->installation,
							  cancellable,
							  error);
	if (xrefs == NULL) {
		gs_plugin_flatpak_error_convert (error);
		return FALSE;
	}

	/* look at each installed xref */
	list_tmp = gs_app_list_new ();
	for (guint i = 0; i < xrefs->len; i++) {
		FlatpakInstalledRef *xref = g_ptr_array_index (xrefs, i);
		const gchar *commit;
		const gchar *latest_commit;
		g_autoptr(GsApp) app = NULL;
		g_autoptr(GError) error_local = NULL;

		/* check the application has already been downloaded */
		commit = flatpak_ref_get_commit (FLATPAK_REF (xref));
		latest_commit = flatpak_installed_ref_get_latest_commit (xref);
		if (latest_commit == NULL) {
			g_debug ("could not get latest commit for %s",
				 flatpak_ref_get_name (FLATPAK_REF (xref)));
			continue;
		}
		if (g_strcmp0 (commit, latest_commit) == 0) {
			g_debug ("no downloaded update for %s",
				 flatpak_ref_get_name (FLATPAK_REF (xref)));
			continue;
		}

		/* we have an update to show */
		g_debug ("%s has a downloaded update %s->%s",
			 flatpak_ref_get_name (FLATPAK_REF (xref)),
			 commit, latest_commit);
		app = gs_flatpak_create_installed (self, xref, &error_local);
		if (app == NULL) {
			g_warning ("failed to add flatpak: %s", error_local->message);
			continue;
		}
		gs_app_set_state (app, AS_APP_STATE_UPDATABLE_LIVE);
		gs_app_set_update_details (app, NULL);
		gs_app_set_update_version (app, NULL);
		gs_app_set_update_urgency (app, AS_URGENCY_KIND_UNKNOWN);
		gs_app_set_size_download (app, 0);
		gs_app_list_add (list_tmp, app);
	}

	/* convert the results to be related */
	list_new = gs_flatpak_list_convert_related (self, list_tmp, cancellable, error);
	if (list_new == NULL)
		return FALSE;
	gs_app_list_add_list (list, list_new);

	/* success */
	return TRUE;
}

gboolean
gs_flatpak_add_updates_pending (GsFlatpak *self, GsAppList *list,
				GCancellable *cancellable,
				GError **error)
{
	g_autoptr(GPtrArray) xrefs = NULL;
	g_autoptr(GsAppList) list_new = gs_app_list_new ();
	g_autoptr(GsAppList) list_tmp = gs_app_list_new ();

	/* get all the updatable apps and runtimes (no network I/O) */
	xrefs = flatpak_installation_list_installed_refs_for_update (self->installation,
								     cancellable,
								     error);
	if (xrefs == NULL) {
		gs_plugin_flatpak_error_convert (error);
		return FALSE;
	}
	for (guint i = 0; i < xrefs->len; i++) {
		FlatpakInstalledRef *xref = g_ptr_array_index (xrefs, i);
		guint64 download_size = 0;
		g_autoptr(GsApp) app = NULL;
		g_autoptr(GError) error_local = NULL;

		/* we have an update to show */
		g_debug ("%s has update", flatpak_ref_get_name (FLATPAK_REF (xref)));
		app = gs_flatpak_create_installed (self, xref, &error_local);
		if (app == NULL) {
			g_warning ("failed to add flatpak: %s", error_local->message);
			continue;
		}
		gs_app_set_state (app, AS_APP_STATE_UPDATABLE_LIVE);

		/* get the current download size */
		if (gs_app_get_size_download (app) == 0) {
			if (!flatpak_installation_fetch_remote_size_sync (self->installation,
									  gs_app_get_origin (app),
									  FLATPAK_REF (xref),
									  &download_size,
									  NULL,
									  cancellable,
									  &error_local)) {
				g_warning ("failed to get download size: %s",
					   error_local->message);
				gs_app_set_size_download (app, GS_APP_SIZE_UNKNOWABLE);
			} else {
				gs_app_set_size_download (app, download_size);
			}
		}

		gs_app_list_add (list_tmp, app);
	}

	/* convert the results to be related */
	list_new = gs_flatpak_list_convert_related (self, list_tmp, cancellable, error);
	if (list_new == NULL)
		return FALSE;
	gs_app_list_add_list (list, list_new);

	/* success */
	return TRUE;
}

static void
gs_flatpak_progress_cb (const gchar *status,
			guint progress,
			gboolean estimating,
			gpointer user_data)
{
	GsApp *app = GS_APP (user_data);
	gs_app_set_progress (app, progress);
}

gboolean
gs_flatpak_refresh (GsFlatpak *self,
		    guint cache_age,
		    GsPluginRefreshFlags flags,
		    GCancellable *cancellable,
		    GError **error)
{
	guint i;
	g_autoptr(GPtrArray) xrefs = NULL;

	/* give all the repos a second chance */
	g_hash_table_remove_all (self->broken_remotes);

	/* manually drop the cache */
	if (!flatpak_installation_drop_caches (self->installation,
					       cancellable,
					       error)) {
		gs_plugin_flatpak_error_convert (error);
		return FALSE;
	}

	/* update AppStream metadata */
	if (flags & GS_PLUGIN_REFRESH_FLAGS_METADATA) {
		if (!gs_flatpak_refresh_appstream (self, cache_age, flags,
						   cancellable, error))
			return FALSE;
	}

	/* no longer interesting */
	if ((flags & GS_PLUGIN_REFRESH_FLAGS_PAYLOAD) == 0)
		return TRUE;

	/* get all the updates available from all remotes */
	xrefs = flatpak_installation_list_installed_refs_for_update (self->installation,
								     cancellable,
								     error);
	if (xrefs == NULL) {
		gs_plugin_flatpak_error_convert (error);
		return FALSE;
	}
	for (i = 0; i < xrefs->len; i++) {
		FlatpakInstalledRef *xref = g_ptr_array_index (xrefs, i);
		g_autoptr(GsApp) app = NULL;
		g_autoptr(FlatpakInstalledRef) xref2 = NULL;

		/* try to create a GsApp so we can do progress reporting */
		app = gs_flatpak_create_installed (self, xref,
						   NULL);

		/* fetch but do not deploy */
		g_debug ("pulling update for %s",
			 flatpak_ref_get_name (FLATPAK_REF (xref)));
		xref2 = flatpak_installation_update (self->installation,
						     FLATPAK_UPDATE_FLAGS_NO_DEPLOY,
						     flatpak_ref_get_kind (FLATPAK_REF (xref)),
						     flatpak_ref_get_name (FLATPAK_REF (xref)),
						     flatpak_ref_get_arch (FLATPAK_REF (xref)),
						     flatpak_ref_get_branch (FLATPAK_REF (xref)),
						     gs_flatpak_progress_cb, app,
						     cancellable, error);
		if (xref2 == NULL) {
			gs_plugin_flatpak_error_convert (error);
			return FALSE;
		}
	}

	return TRUE;
}

static gboolean
gs_plugin_refine_item_origin_ui (GsFlatpak *self, GsApp *app,
				 GCancellable *cancellable,
				 GError **error)
{
	const gchar *origin;
	guint i;
	g_autoptr(GPtrArray) xremotes = NULL;
	g_autoptr(AsProfileTask) ptask = NULL;

	/* already set */
	origin = gs_app_get_origin_ui (app);
	if (origin != NULL)
		return TRUE;

	/* find list of remotes */
	ptask = as_profile_start (gs_plugin_get_profile (self->plugin),
				  "%s::refine-origin-ui",
				  gs_flatpak_get_id (self));
	g_assert (ptask != NULL);
	xremotes = flatpak_installation_list_remotes (self->installation,
						      cancellable,
						      error);
	if (xremotes == NULL) {
		gs_plugin_flatpak_error_convert (error);
		return FALSE;
	}
	for (i = 0; i < xremotes->len; i++) {
		FlatpakRemote *xremote = g_ptr_array_index (xremotes, i);
		if (flatpak_remote_get_disabled (xremote))
			continue;
		if (g_strcmp0 (gs_app_get_origin (app),
			       flatpak_remote_get_name (xremote)) == 0) {
			g_autofree gchar *title = NULL;
			title = flatpak_remote_get_title (xremote);
			gs_app_set_origin_ui (app, title);
			break;
		}
	}

	return TRUE;
}

static gboolean
gs_plugin_refine_item_origin_hostname (GsFlatpak *self, GsApp *app,
				       GCancellable *cancellable,
				       GError **error)
{
	g_autoptr(FlatpakRemote) xremote = NULL;
	g_autofree gchar *url = NULL;
	g_autoptr(AsProfileTask) ptask = NULL;

	/* profile */
	ptask = as_profile_start (gs_plugin_get_profile (self->plugin),
				  "%s::refine-origin-hostname{%s}",
				  gs_flatpak_get_id (self),
				  gs_app_get_id (app));
	g_assert (ptask != NULL);

	/* already set */
	if (gs_app_get_origin_hostname (app) != NULL)
		return TRUE;

	/* no origin */
	if (gs_app_get_origin (app) == NULL)
		return TRUE;

	/* get the remote  */
	xremote = flatpak_installation_get_remote_by_name (self->installation,
							   gs_app_get_origin (app),
							   cancellable,
							   error);
	if (xremote == NULL) {
		gs_plugin_flatpak_error_convert (error);
		return FALSE;
	}
	url = flatpak_remote_get_url (xremote);
	if (url == NULL) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_INVALID_FORMAT,
			     "no URL for remote %s",
			     flatpak_remote_get_name (xremote));
		return FALSE;
	}
	gs_app_set_origin_hostname (app, url);
	return TRUE;
}

static gboolean
gs_refine_item_metadata (GsFlatpak *self, GsApp *app,
			 GCancellable *cancellable,
			 GError **error)
{
	g_autoptr(FlatpakRef) xref = NULL;

	/* already set */
	if (gs_app_get_metadata_item (app, "flatpak::kind") != NULL)
		return TRUE;

	/* not a valid type */
	if (gs_app_get_kind (app) == AS_APP_KIND_SOURCE)
		return TRUE;

	/* AppStream sets the source to appname/arch/branch, if this isn't set
	 * we can't break out the fields */
	if (gs_app_get_source_default (app) == NULL) {
		g_autofree gchar *tmp = gs_app_to_string (app);
		g_warning ("no source set by appstream for %s: %s",
			   gs_plugin_get_name (self->plugin), tmp);
		return TRUE;
	}

	/* parse the ref */
	xref = flatpak_ref_parse (gs_app_get_source_default (app), error);
	if (xref == NULL) {
		gs_plugin_flatpak_error_convert (error);
		g_prefix_error (error, "failed to parse '%s': ",
				gs_app_get_source_default (app));
		return FALSE;
	}
	gs_flatpak_set_metadata (self, app, xref);

	/* success */
	return TRUE;
}

static FlatpakRefKind
gs_app_get_flatpak_kind (GsApp *app)
{
	const gchar *kind = gs_app_get_metadata_item (app, "flatpak::kind");
	if (g_strcmp0 (kind, "app") == 0)
		return FLATPAK_REF_KIND_APP;
	if (g_strcmp0 (kind, "runtime") == 0)
		return FLATPAK_REF_KIND_RUNTIME;
	g_warning ("unknown flatpak kind: %s", kind);
	return FLATPAK_REF_KIND_APP;
}

static gboolean
gs_flatpak_refine_origin_from_installation (GsFlatpak *self,
					    FlatpakInstallation *installation,
					    GsApp *app,
					    GCancellable *cancellable,
					    GError **error)
{
	guint i;
	g_autoptr(GPtrArray) xremotes = NULL;

	xremotes = flatpak_installation_list_remotes (installation,
						      cancellable,
						      error);
	if (xremotes == NULL) {
		gs_plugin_flatpak_error_convert (error);
		return FALSE;
	}
	for (i = 0; i < xremotes->len; i++) {
		const gchar *remote_name;
		FlatpakRemote *xremote = g_ptr_array_index (xremotes, i);
		g_autoptr(FlatpakRemoteRef) xref = NULL;
		g_autoptr(GError) error_local = NULL;

		/* not enabled */
		if (flatpak_remote_get_disabled (xremote))
			continue;

		/* sync */
		remote_name = flatpak_remote_get_name (xremote);
		g_debug ("looking at remote %s", remote_name);
		xref = flatpak_installation_fetch_remote_ref_sync (installation,
								   remote_name,
								   gs_app_get_flatpak_kind (app),
								   gs_app_get_flatpak_name (app),
								   gs_app_get_flatpak_arch (app),
								   gs_app_get_flatpak_branch (app),
								   cancellable,
								   &error_local);
		if (xref != NULL) {
			g_debug ("found remote %s", remote_name);
			gs_app_set_origin (app, remote_name);
			return TRUE;
		}
		g_debug ("failed to find remote %s: %s",
			 remote_name, error_local->message);
	}

	/* not found */
	return TRUE;
}

static FlatpakInstallation *
gs_flatpak_get_installation_counterpart (GsFlatpak *self,
					 GCancellable *cancellable,
					 GError **error)
{
	FlatpakInstallation *installation;
	if (flatpak_installation_get_is_user (self->installation))
		installation = flatpak_installation_new_system (cancellable, error);
	else
		installation = flatpak_installation_new_user (cancellable, error);
	if (installation == NULL) {
		gs_plugin_flatpak_error_convert (error);
		return NULL;
	}
	return installation;
}

static gboolean
gs_plugin_refine_item_origin (GsFlatpak *self,
			      GsApp *app,
			      GCancellable *cancellable,
			      GError **error)
{
	g_autoptr(AsProfileTask) ptask = NULL;
	g_autoptr(GError) local_error = NULL;

	/* already set */
	if (gs_app_get_origin (app) != NULL)
		return TRUE;

	/* not applicable */
	if (gs_app_get_state(app) == AS_APP_STATE_AVAILABLE_LOCAL)
		return TRUE;

	/* ensure metadata exists */
	ptask = as_profile_start (gs_plugin_get_profile (self->plugin),
				  "%s::refine-origin",
				  gs_flatpak_get_id (self));
	g_assert (ptask != NULL);
	if (!gs_refine_item_metadata (self, app, cancellable, error))
		return FALSE;

	/* find list of remotes */
	g_debug ("looking for a remote for %s/%s/%s",
		 gs_app_get_flatpak_name (app),
		 gs_app_get_flatpak_arch (app),
		 gs_app_get_flatpak_branch (app));

	/* first check the plugin's own flatpak installation */
	if (!gs_flatpak_refine_origin_from_installation (self,
							 self->installation,
							 app,
							 cancellable,
							 error)) {
		g_prefix_error (error, "failed to refine origin from self: ");
		return FALSE;
	}

	/* check the system installation if we're on a user one */
	if (gs_app_get_scope (app) == AS_APP_SCOPE_USER &&
	    gs_app_get_flatpak_kind (app) == FLATPAK_REF_KIND_RUNTIME) {
		g_autoptr(GError) error_local = NULL;
		g_autoptr(FlatpakInstallation) installation =
			gs_flatpak_get_installation_counterpart (self,
								 cancellable,
								 &error_local);
		if (installation == NULL) {
			if (g_error_matches (error_local,
					     GS_PLUGIN_ERROR,
					     GS_PLUGIN_ERROR_NO_SECURITY)) {
				g_debug ("ignoring: %s", error_local->message);
				return TRUE;
			}
			g_set_error (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_NOT_SUPPORTED,
				     "failed to get counterpart: %s",
				     error_local->message);
			return FALSE;
		}
		if (!gs_flatpak_refine_origin_from_installation (self,
								 installation,
								 app,
								 cancellable,
								 error)) {
			g_prefix_error (error,
					"failed to refine origin from counterpart: ");
			return FALSE;
		}
	}

	return TRUE;
}

static gboolean
gs_flatpak_app_matches_xref (GsFlatpak *self, GsApp *app, FlatpakRef *xref)
{
	g_autoptr(GsApp) app_tmp = gs_flatpak_create_app (self, xref);

	/* check ID */
	if (g_strcmp0 (gs_app_get_unique_id (app),
	    gs_app_get_unique_id (app_tmp)) == 0)
		return TRUE;

	/* do all the metadata items match? */
	if (g_strcmp0 (gs_app_get_flatpak_name (app),
		       flatpak_ref_get_name (xref)) == 0 &&
	    g_strcmp0 (gs_app_get_flatpak_arch (app),
		       flatpak_ref_get_arch (xref)) == 0 &&
	    g_strcmp0 (gs_app_get_flatpak_branch (app),
		       flatpak_ref_get_branch (xref)) == 0)
		return TRUE;

	/* sad panda */
	return FALSE;
}

static FlatpakRef *
gs_flatpak_create_fake_ref (GsApp *app, GError **error)
{
	FlatpakRef *xref;
	g_autofree gchar *id = NULL;
	id = g_strdup_printf ("%s/%s/%s/%s",
			      gs_app_get_flatpak_kind_as_str (app),
			      gs_app_get_flatpak_name (app),
			      gs_app_get_flatpak_arch (app),
			      gs_app_get_flatpak_branch (app));
	xref = flatpak_ref_parse (id, error);
	if (xref == NULL) {
		gs_plugin_flatpak_error_convert (error);
		return NULL;
	}
	return xref;
}

static gboolean
gs_plugin_refine_item_state (GsFlatpak *self,
			     GsApp *app,
			     GCancellable *cancellable,
			     GError **error)
{
	guint i;
	g_autoptr(GPtrArray) xrefs = NULL;
	g_autoptr(AsProfileTask) ptask = NULL;

	/* already found */
	if (gs_app_get_state (app) != AS_APP_STATE_UNKNOWN)
		return TRUE;

	/* need broken out metadata */
	if (!gs_refine_item_metadata (self, app, cancellable, error))
		return FALSE;

	/* get apps and runtimes */
	ptask = as_profile_start (gs_plugin_get_profile (self->plugin),
				  "%s::refine-action",
				  gs_flatpak_get_id (self));
	g_assert (ptask != NULL);
	xrefs = flatpak_installation_list_installed_refs (self->installation,
							  cancellable, error);
	if (xrefs == NULL) {
		gs_plugin_flatpak_error_convert (error);
		return FALSE;
	}
	for (i = 0; i < xrefs->len; i++) {
		FlatpakInstalledRef *xref = g_ptr_array_index (xrefs, i);

		/* check xref is app */
		if (!gs_flatpak_app_matches_xref (self, app, FLATPAK_REF(xref)))
			continue;

		/* mark as installed */
		g_debug ("marking %s as installed with flatpak",
			 gs_app_get_id (app));
		gs_flatpak_set_metadata_installed (self, app, xref);
		if (gs_app_get_state (app) == AS_APP_STATE_UNKNOWN)
			gs_app_set_state (app, AS_APP_STATE_INSTALLED);
	}

	/* ensure origin set */
	if (!gs_plugin_refine_item_origin (self, app, cancellable, error))
		return FALSE;

	/* special case: if this is per-user instance and the runtime is
	 * available system-wide then mark it installed, and vice-versa */
	if (gs_app_get_flatpak_kind (app) == FLATPAK_REF_KIND_RUNTIME &&
	    gs_app_get_state (app) == AS_APP_STATE_UNKNOWN) {
		g_autoptr(GError) error_local = NULL;
		g_autoptr(FlatpakInstallation) installation =
			gs_flatpak_get_installation_counterpart (self,
								 cancellable,
								 &error_local);
		if (installation == NULL) {
			if (g_error_matches (error_local,
					     GS_PLUGIN_ERROR,
					     GS_PLUGIN_ERROR_NO_SECURITY)) {
				g_debug ("ignoring: %s", error_local->message);
			} else {
				g_set_error (error,
					     GS_PLUGIN_ERROR,
					     GS_PLUGIN_ERROR_NOT_SUPPORTED,
					     "failed to get counterpart: %s",
					     error_local->message);
				return FALSE;
			}
		} else {
			g_autoptr(GPtrArray) xrefs2 = NULL;
			xrefs2 = flatpak_installation_list_installed_refs (installation,
									   cancellable,
									   error);
			if (xrefs2 == NULL) {
				gs_plugin_flatpak_error_convert (error);
				return FALSE;
			}
			for (i = 0; i < xrefs2->len; i++) {
				FlatpakInstalledRef *xref = g_ptr_array_index (xrefs2, i);
				if (!gs_flatpak_app_matches_xref (self, app, FLATPAK_REF(xref)))
					continue;
				gs_app_set_state (app, AS_APP_STATE_INSTALLED);
			}
		}
	}

	/* anything not installed just check the remote is still present */
	if (gs_app_get_state (app) == AS_APP_STATE_UNKNOWN &&
	    gs_app_get_origin (app) != NULL) {
		g_autoptr(FlatpakRemote) xremote = NULL;
		xremote = flatpak_installation_get_remote_by_name (self->installation,
								   gs_app_get_origin (app),
								   cancellable, NULL);
		if (xremote != NULL) {
			if (flatpak_remote_get_disabled (xremote)) {
				g_debug ("%s is available with flatpak "
					 "but %s is disabled",
					 gs_app_get_id (app),
					 flatpak_remote_get_name (xremote));
				gs_app_set_state (app, AS_APP_STATE_UNAVAILABLE);
			} else {
				g_debug ("marking %s as available with flatpak",
					 gs_app_get_id (app));
				gs_app_set_state (app, AS_APP_STATE_AVAILABLE);
			}
		} else {
			gs_app_set_state (app, AS_APP_STATE_UNKNOWN);
			g_debug ("failed to find %s remote %s for %s",
				 self->id,
				 gs_app_get_origin (app),
				 gs_app_get_unique_id (app));
		}
	}

	/* success */
	return TRUE;
}

static GsApp *
gs_flatpak_create_runtime_from_metadata (GsFlatpak *self,
					 const GsApp *app,
					 const gchar *data,
					 const gsize length,
					 GError **error)
{
	g_autofree gchar *runtime = NULL;
	g_autoptr(GKeyFile) kf = NULL;

	kf = g_key_file_new ();
	if (!g_key_file_load_from_data (kf, data, length, G_KEY_FILE_NONE, error)) {
		gs_utils_error_convert_gio (error);
		return NULL;
	}
	runtime = g_key_file_get_string (kf, "Application", "runtime", error);
	if (runtime == NULL) {
		gs_utils_error_convert_gio (error);
		return NULL;
	}
	return gs_appstream_create_runtime (self->plugin, app, runtime);
}

static gboolean
gs_flatpak_set_app_metadata (GsFlatpak *self,
			     GsApp *app,
			     const gchar *data,
			     gsize length,
			     GError **error)
{
	gboolean secure = TRUE;
	g_autofree gchar *name = NULL;
	g_autofree gchar *runtime = NULL;
	g_autofree gchar *source = NULL;
	g_autoptr(GKeyFile) kf = NULL;
	g_autoptr(GsApp) app_runtime = NULL;
	g_auto(GStrv) shared = NULL;
	g_auto(GStrv) sockets = NULL;
	g_auto(GStrv) filesystems = NULL;

	kf = g_key_file_new ();
	if (!g_key_file_load_from_data (kf, data, length, G_KEY_FILE_NONE, error)) {
		gs_plugin_flatpak_error_convert (error);
		return FALSE;
	}
	name = g_key_file_get_string (kf, "Application", "name", error);
	if (name == NULL) {
		gs_plugin_flatpak_error_convert (error);
		return FALSE;
	}
	gs_app_set_flatpak_name (app, name);
	runtime = g_key_file_get_string (kf, "Application", "runtime", error);
	if (runtime == NULL) {
		gs_plugin_flatpak_error_convert (error);
		return FALSE;
	}
	g_debug ("runtime for %s is %s", name, runtime);

	/* we always get this, but it's a low bar... */
	gs_app_add_kudo (app, GS_APP_KUDO_SANDBOXED);
	shared = g_key_file_get_string_list (kf, "Context", "shared", NULL, NULL);
	if (shared != NULL) {
		/* SHM isn't secure enough */
		if (g_strv_contains ((const gchar * const *) shared, "ipc"))
			secure = FALSE;
	}
	sockets = g_key_file_get_string_list (kf, "Context", "sockets", NULL, NULL);
	if (sockets != NULL) {
		/* X11 isn't secure enough */
		if (g_strv_contains ((const gchar * const *) sockets, "x11"))
			secure = FALSE;
	}
	filesystems = g_key_file_get_string_list (kf, "Context", "filesystems", NULL, NULL);
	if (filesystems != NULL) {
		/* secure apps should be using portals */
		if (g_strv_contains ((const gchar * const *) filesystems, "home"))
			secure = FALSE;
	}

	/* this is actually quite hard to achieve */
	if (secure)
		gs_app_add_kudo (app, GS_APP_KUDO_SANDBOXED_SECURE);

	/* create runtime */
	if (gs_app_get_runtime (app) == NULL) {
		app_runtime = gs_appstream_create_runtime (self->plugin, app, runtime);
		if (app_runtime != NULL) {
			gs_plugin_refine_item_scope (self, app_runtime);
			gs_app_set_runtime (app, app_runtime);
		}
	}

	return TRUE;
}

static GBytes *
gs_flatpak_fetch_remote_metadata (GsFlatpak *self,
				  GsApp *app,
				  GCancellable *cancellable,
				  GError **error)
{
	g_autoptr(GBytes) data = NULL;
	g_autoptr(FlatpakRef) xref = NULL;

	/* no origin */
	if (gs_app_get_origin (app) == NULL) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_NOT_SUPPORTED,
			     "no origin set when getting metadata for %s",
			     gs_app_get_unique_id (app));
		return NULL;
	}

	/* fetch from the server */
	xref = gs_flatpak_create_fake_ref (app, error);
	if (xref == NULL)
		return NULL;
	data = flatpak_installation_fetch_remote_metadata_sync (self->installation,
								gs_app_get_origin (app),
								xref,
								cancellable,
								error);
	if (data == NULL) {
		gs_plugin_flatpak_error_convert (error);
		return NULL;
	}
	return g_steal_pointer (&data);
}

static gboolean
gs_plugin_refine_item_metadata (GsFlatpak *self,
				GsApp *app,
				GCancellable *cancellable,
				GError **error)
{
	const gchar *str;
	gsize len = 0;
	g_autofree gchar *contents = NULL;
	g_autofree gchar *installation_path_str = NULL;
	g_autofree gchar *install_path = NULL;
	g_autoptr(AsProfileTask) ptask = NULL;
	g_autoptr(GBytes) data = NULL;
	g_autoptr(GFile) installation_path = NULL;

	/* profile */
	ptask = as_profile_start (gs_plugin_get_profile (self->plugin),
				  "%s::refine-metadata{%s}",
				  gs_flatpak_get_id (self),
				  gs_app_get_id (app));
	g_assert (ptask != NULL);

	/* not applicable */
	if (gs_app_get_kind (app) == AS_APP_KIND_SOURCE)
		return TRUE;
	if (gs_app_get_flatpak_kind (app) != FLATPAK_REF_KIND_APP)
		return TRUE;

	/* already done */
	if (gs_app_has_kudo (app, GS_APP_KUDO_SANDBOXED))
		return TRUE;

	/* this is quicker than doing network IO */
	installation_path = flatpak_installation_get_path (self->installation);
	installation_path_str = g_file_get_path (installation_path);
	install_path = g_build_filename (installation_path_str,
					 gs_app_get_flatpak_kind_as_str (app),
					 gs_app_get_flatpak_name (app),
					 gs_app_get_flatpak_arch (app),
					 gs_app_get_flatpak_branch (app),
					 "active",
					 "metadata",
					 NULL);
	if (g_file_test (install_path, G_FILE_TEST_EXISTS)) {
		if (!g_file_get_contents (install_path, &contents, &len, error))
			return FALSE;
		str = contents;
	} else {
		data = gs_flatpak_fetch_remote_metadata (self, app, cancellable,
							 error);
		if (data == NULL)
			return FALSE;
		str = g_bytes_get_data (data, &len);
	}

	/* parse key file */
	if (!gs_flatpak_set_app_metadata (self, app, str, len, error))
		return FALSE;
	return TRUE;
}

static FlatpakInstalledRef *
gs_flatpak_get_installed_ref (GsFlatpak *self,
			      GsApp *app,
			      GCancellable *cancellable,
			      GError **error)
{
	FlatpakInstalledRef *ref;
	ref = flatpak_installation_get_installed_ref (self->installation,
						      gs_app_get_flatpak_kind (app),
						      gs_app_get_flatpak_name (app),
						      gs_app_get_flatpak_arch (app),
						      gs_app_get_flatpak_branch (app),
						      cancellable,
						      error);
	if (ref == NULL)
		gs_plugin_flatpak_error_convert (error);
	return ref;
}

static gboolean
gs_plugin_refine_item_size (GsFlatpak *self,
			    GsApp *app,
			    GCancellable *cancellable,
			    GError **error)
{
	gboolean ret;
	guint64 download_size = GS_APP_SIZE_UNKNOWABLE;
	guint64 installed_size = GS_APP_SIZE_UNKNOWABLE;
	g_autoptr(AsProfileTask) ptask = NULL;

	/* not applicable */
	if (gs_app_get_state (app) == AS_APP_STATE_AVAILABLE_LOCAL)
		return TRUE;
	if (gs_app_get_kind (app) == AS_APP_KIND_SOURCE)
		return TRUE;

	/* already set */
	if (gs_app_is_installed (app)) {
		/* only care about the installed size if the app is installed */
		if (gs_app_get_size_installed (app) > 0)
			return TRUE;
	} else {
		if (gs_app_get_size_installed (app) > 0 &&
		    gs_app_get_size_download (app) > 0)
		return TRUE;
	}

	/* need runtime */
	if (!gs_plugin_refine_item_metadata (self, app, cancellable, error))
		return FALSE;

	/* calculate the platform size too if the app is not installed */
	if (gs_app_get_state (app) == AS_APP_STATE_AVAILABLE &&
	    gs_app_get_flatpak_kind (app) == FLATPAK_REF_KIND_APP) {
		GsApp *app_runtime;

		/* is the app_runtime already installed? */
		app_runtime = gs_app_get_runtime (app);
		if (!gs_plugin_refine_item_state (self,
						  app_runtime,
						  cancellable,
						  error))
			return FALSE;
		if (gs_app_get_state (app_runtime) == AS_APP_STATE_INSTALLED) {
			g_debug ("runtime %s is already installed, so not adding size",
				 gs_app_get_id (app_runtime));
		} else {
			if (!gs_plugin_refine_item_size (self,
							 app_runtime,
							 cancellable,
							 error))
				return FALSE;
		}
	}

	/* just get the size of the app */
	ptask = as_profile_start (gs_plugin_get_profile (self->plugin),
				  "%s::refine-size",
				  gs_flatpak_get_id (self));
	g_assert (ptask != NULL);
	if (!gs_plugin_refine_item_origin (self, app,
					   cancellable, error))
		return FALSE;

	/* if the app is installed we use the ref to fetch the installed size
	 * and ignore the download size as this is faster */
	if (gs_app_is_installed (app)) {
		g_autoptr(FlatpakInstalledRef) xref = NULL;
		xref = gs_flatpak_get_installed_ref (self, app, cancellable, error);
		if (xref == NULL)
			return FALSE;
		installed_size = flatpak_installed_ref_get_installed_size (xref);
		if (installed_size == 0)
			installed_size = GS_APP_SIZE_UNKNOWABLE;
	} else {
		g_autoptr(FlatpakRef) xref = NULL;
		g_autoptr(GError) error_local = NULL;

		/* no origin */
		if (gs_app_get_origin (app) == NULL) {
			g_set_error (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_NOT_SUPPORTED,
				     "no origin set for %s",
				     gs_app_get_unique_id (app));
			return FALSE;
		}
		xref = gs_flatpak_create_fake_ref (app, error);
		if (xref == NULL)
			return FALSE;
		ret = flatpak_installation_fetch_remote_size_sync (self->installation,
								   gs_app_get_origin (app),
								   xref,
								   &download_size,
								   &installed_size,
								   cancellable,
								   &error_local);

		if (!ret) {
			g_warning ("libflatpak failed to return application "
				   "size: %s", error_local->message);
		}
	}

	gs_app_set_size_installed (app, installed_size);
	gs_app_set_size_download (app, download_size);

	return TRUE;
}

static void
gs_flatpak_refine_appstream_release (AsApp *item, GsApp *app)
{
	AsRelease *rel = as_app_get_release_default (item);
	if (rel == NULL)
		return;
	if (as_release_get_version (rel) == NULL)
		return;
	switch (gs_app_get_state (app)) {
	case AS_APP_STATE_INSTALLED:
	case AS_APP_STATE_AVAILABLE:
	case AS_APP_STATE_AVAILABLE_LOCAL:
		gs_app_set_version (app, as_release_get_version (rel));
		break;
	default:
		g_debug ("%s is not installed, so ignoring version of %s",
			 as_app_get_id (item), as_release_get_version (rel));
		break;
	}
}

static gboolean
gs_flatpak_refine_appstream (GsFlatpak *self, GsApp *app, GError **error)
{
	AsApp *item;
	const gchar *unique_id = gs_app_get_unique_id (app);
	g_autoptr(AsProfileTask) ptask = NULL;

	/* profile */
	ptask = as_profile_start (gs_plugin_get_profile (self->plugin),
				  "%s::refine-appstream{%s}",
				  gs_flatpak_get_id (self),
				  gs_app_get_id (app));
	g_assert (ptask != NULL);

	if (unique_id == NULL)
		return TRUE;
	item = as_store_get_app_by_unique_id (self->store,
					      unique_id,
					      AS_STORE_SEARCH_FLAG_USE_WILDCARDS);
	if (item == NULL) {
		g_autoptr(GPtrArray) apps = NULL;
		apps = as_store_get_apps_by_id (self->store, gs_app_get_id (app));
		if (apps->len > 0) {
			g_debug ("potential matches for %s:", unique_id);
			for (guint i = 0; i < apps->len; i++) {
				AsApp *app_tmp = g_ptr_array_index (apps, i);
				g_debug ("- %s", as_app_get_unique_id (app_tmp));
			}
		}
		return TRUE;
	}

	if (!gs_appstream_refine_app (self->plugin, app, item, error))
		return FALSE;

	/* use the default release as the version number */
	gs_flatpak_refine_appstream_release (item, app);

	return TRUE;
}

gboolean
gs_flatpak_refine_app (GsFlatpak *self,
		       GsApp *app,
		       GsPluginRefineFlags flags,
		       GCancellable *cancellable,
		       GError **error)
{
	AsAppState old_state = gs_app_get_state (app);
	g_autoptr(AsProfileTask) ptask = NULL;

	/* profile */
	ptask = as_profile_start (gs_plugin_get_profile (self->plugin),
				  "%s::refine{%s}",
				  gs_flatpak_get_id (self),
				  gs_app_get_id (app));
	g_assert (ptask != NULL);

	/* always do AppStream properties */
	if (!gs_flatpak_refine_appstream (self, app, error))
		return FALSE;

	/* flatpak apps can always be removed */
	gs_app_remove_quirk (app, AS_APP_QUIRK_COMPULSORY);

	/* scope is fast, do unconditionally */
	gs_plugin_refine_item_scope (self, app);

	/* AppStream sets the source to appname/arch/branch */
	if (!gs_refine_item_metadata (self, app, cancellable, error)) {
		g_prefix_error (error, "failed to get metadata: ");
		return FALSE;
	}

	/* check the installed state */
	if (!gs_plugin_refine_item_state (self, app, cancellable, error)) {
		g_prefix_error (error, "failed to get state: ");
		return FALSE;
	}

	/* if the state was changed, perhaps set the version from the release */
	if (old_state != gs_app_get_state (app)) {
		if (!gs_flatpak_refine_appstream (self, app, error))
			return FALSE;
	}

	/* version fallback */
	if (flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_VERSION) {
		if (gs_app_get_version (app) == NULL) {
			const gchar *branch;
			branch = gs_app_get_flatpak_branch (app);
			gs_app_set_version (app, branch);
		}
	}

	/* size */
	if (flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_SIZE) {
		if (!gs_plugin_refine_item_size (self, app,
						 cancellable, error)) {
			g_prefix_error (error, "failed to get size: ");
			return FALSE;
		}
	}

	/* origin */
	if (flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_ORIGIN) {
		if (!gs_plugin_refine_item_origin_ui (self, app,
						      cancellable, error)) {
			g_prefix_error (error, "failed to get origin: ");
			return FALSE;
		}
	}

	/* origin-hostname */
	if (flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_ORIGIN_HOSTNAME) {
		if (!gs_plugin_refine_item_origin_hostname (self, app,
							    cancellable,
							    error)) {
			g_prefix_error (error, "failed to get origin-hostname: ");
			return FALSE;
		}
	}

	/* permissions */
	if (flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_RUNTIME ||
	    flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_PERMISSIONS) {
		if (!gs_plugin_refine_item_metadata (self, app,
						     cancellable, error)) {
			g_prefix_error (error, "failed to get permissions: ");
			return FALSE;
		}
	}

	return TRUE;
}

gboolean
gs_flatpak_refine_wildcard (GsFlatpak *self, GsApp *app,
			    GsAppList *list, GsPluginRefineFlags flags,
			    GCancellable *cancellable, GError **error)
{
	const gchar *id;
	guint i;
	g_autoptr(GPtrArray) items = NULL;

	/* not valid */
	id = gs_app_get_id (app);
	if (id == NULL)
		return TRUE;

	/* find all apps when matching any prefixes */
	items = as_store_get_apps_by_id (self->store, id);
	for (i = 0; i < items->len; i++) {
		AsApp *item = NULL;
		g_autoptr(GsApp) new = NULL;

		/* does the app have an installation method */
		item = g_ptr_array_index (items, i);
		if (as_app_get_bundle_default (item) == NULL) {
			g_debug ("not using %s for wildcard as no bundle",
				 as_app_get_id (item));
			continue;
		}

		/* new app */
		g_debug ("found %s for wildcard %s",
			 as_app_get_unique_id (item), id);
		new = gs_appstream_create_app (self->plugin, item, NULL);
		if (new == NULL)
			return FALSE;
		gs_app_set_scope (new, self->scope);
		if (!gs_flatpak_refine_app (self, new, flags, cancellable, error))
			return FALSE;
		gs_app_list_add (list, new);
	}
	return TRUE;
}

gboolean
gs_flatpak_launch (GsFlatpak *self,
		   GsApp *app,
		   GCancellable *cancellable,
		   GError **error)
{
	const gchar *branch = NULL;

	branch = gs_app_get_flatpak_branch (app);
	if (branch == NULL)
		branch = "master";

	/* launch the app */
	if (!flatpak_installation_launch (self->installation,
					  gs_app_get_flatpak_name (app),
					  NULL,
					  branch,
					  NULL,
					  cancellable,
					  error)) {
		gs_plugin_flatpak_error_convert (error);
		return FALSE;
	}
	return TRUE;
}

static gboolean
gs_flatpak_app_remove_source (GsFlatpak *self,
			      GsApp *app,
			      GCancellable *cancellable,
			      GError **error)
{
	g_autoptr(FlatpakRemote) xremote = NULL;

	/* find the remote */
	xremote = flatpak_installation_get_remote_by_name (self->installation,
							   gs_app_get_id (app),
							   cancellable, error);
	if (xremote == NULL) {
		gs_plugin_flatpak_error_convert (error);
		g_prefix_error (error,
				"flatpak source %s not found: ",
				gs_app_get_id (app));
		return FALSE;
	}

	/* remove */
	gs_app_set_state (app, AS_APP_STATE_REMOVING);
	if (!flatpak_installation_remove_remote (self->installation,
						 gs_app_get_id (app),
						 cancellable,
						 error)) {
		gs_plugin_flatpak_error_convert (error);
		gs_app_set_state_recover (app);
		return FALSE;
	}
	gs_app_set_state (app, AS_APP_STATE_AVAILABLE);
	return TRUE;
}

gboolean
gs_flatpak_app_remove (GsFlatpak *self,
		       GsApp *app,
		       GCancellable *cancellable,
		       GError **error)
{
	g_autoptr(FlatpakRemote) xremote = NULL;
	g_autofree gchar *remote_name = NULL;

	/* refine to get basics */
	if (!gs_flatpak_refine_app (self, app,
				    GS_PLUGIN_REFINE_FLAGS_DEFAULT,
				    cancellable, error))
		return FALSE;

	/* is a source */
	if (gs_app_get_kind (app) == AS_APP_KIND_SOURCE) {
		return gs_flatpak_app_remove_source (self,
						     app,
						     cancellable,
						     error);
	}

	/* remove */
	gs_app_set_state (app, AS_APP_STATE_REMOVING);
	if (!flatpak_installation_uninstall (self->installation,
					     gs_app_get_flatpak_kind (app),
					     gs_app_get_flatpak_name (app),
					     gs_app_get_flatpak_arch (app),
					     gs_app_get_flatpak_branch (app),
					     gs_flatpak_progress_cb, app,
					     cancellable, error)) {
		gs_plugin_flatpak_error_convert (error);
		gs_app_set_state_recover (app);
		return FALSE;
	}

	/* did app also install a noenumerate=True remote */
	remote_name = g_strdup_printf ("%s-origin", gs_app_get_flatpak_name (app));
	xremote = flatpak_installation_get_remote_by_name (self->installation,
							   remote_name,
							   cancellable,
							   NULL);
	if (xremote != NULL) {
		g_debug ("removing enumerate=true %s remote", remote_name);
		if (!flatpak_installation_remove_remote (self->installation,
							 remote_name,
							 cancellable,
							 error)) {
			gs_plugin_flatpak_error_convert (error);
			gs_app_set_state_recover (app);
			return FALSE;
		}
		if (!gs_flatpak_rescan_appstream_store (self, cancellable, error))
			return FALSE;
	}

	/* state is not known: we don't know if we can re-install this app */
	gs_app_set_state (app, AS_APP_STATE_UNKNOWN);

	/* refresh the state */
	if (!gs_plugin_refine_item_state (self, app, cancellable, error))
		return FALSE;

	/* success */
	return TRUE;
}

static gboolean
install_runtime_for_app (GsFlatpak *self,
			 GsApp *app,
			 GCancellable *cancellable,
			 GError **error)
{
	GsApp *runtime;
	GError *local_error = NULL;

	/* only required for ostree-based flatpak apps */
	if (g_strcmp0 (gs_app_get_flatpak_file_type (app), "flatpak") != 0) {
		gsize len;
		const gchar *str;
		g_autoptr(GBytes) data = NULL;
		g_autoptr(GsApp) runtime_new = NULL;

		data = gs_flatpak_fetch_remote_metadata (self, app, cancellable, error);
		if (data == NULL) {
			gs_utils_error_add_unique_id (error, app);
			return FALSE;
		}

		str = g_bytes_get_data (data, &len);
		runtime_new = gs_flatpak_create_runtime_from_metadata (self, app,
								       str, len,
								       error);
		if (runtime_new == NULL)
			return FALSE;
		gs_app_set_update_runtime (app, runtime_new);
	}

	/* no runtime required */
	runtime = gs_app_get_update_runtime (app);
	if (runtime == NULL)
		return TRUE;

	/* the runtime could come from a different remote to the app */
	if (!gs_refine_item_metadata (self, runtime, cancellable, error)) {
		gs_utils_error_add_unique_id (error, runtime);
		return FALSE;
	}
	if (!gs_plugin_refine_item_origin (self, runtime, cancellable, error)) {
		gs_utils_error_add_unique_id (error, runtime);
		return FALSE;
	}
	if (!gs_plugin_refine_item_state (self, runtime, cancellable, error)) {
		gs_utils_error_add_unique_id (error, runtime);
		return FALSE;
	}
	if (gs_app_get_state (runtime) == AS_APP_STATE_UNKNOWN) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_NOT_SUPPORTED,
			     "Failed to find runtime %s",
			     gs_app_get_source_default (runtime));
		gs_utils_error_add_unique_id (error, runtime);
		return FALSE;
	}

	/* not installed */
	if (gs_app_get_state (runtime) == AS_APP_STATE_AVAILABLE) {
		g_autoptr(FlatpakInstalledRef) xref = NULL;

		g_debug ("%s/%s is not already installed, so installing",
			 gs_app_get_id (runtime),
			 gs_app_get_flatpak_branch (runtime));
		gs_app_set_state (runtime, AS_APP_STATE_INSTALLING);

		xref = flatpak_installation_install (self->installation,
						     gs_app_get_origin (runtime),
						     gs_app_get_flatpak_kind (runtime),
						     gs_app_get_flatpak_name (runtime),
						     gs_app_get_flatpak_arch (runtime),
						     gs_app_get_flatpak_branch (runtime),
						     gs_flatpak_progress_cb, app,
						     cancellable, &local_error);
		if (xref == NULL) {
			gboolean already_installed =
				g_error_matches (local_error,
						 FLATPAK_ERROR,
						 FLATPAK_ERROR_ALREADY_INSTALLED);
			if (!already_installed) {
				g_propagate_error (error, local_error);
				gs_app_set_state_recover (runtime);
				gs_app_set_state_recover (app);
				return FALSE;
			}

			g_clear_error (&local_error);
			g_debug ("Error: runtime %s is already installed; "
				 "marking the GsApp as installed and continuing...",
				 gs_app_get_unique_id (runtime));
		}
		gs_app_set_state (runtime, AS_APP_STATE_INSTALLED);
	} else {
		g_debug ("%s is already installed, so skipping",
			 gs_app_get_id (runtime));
	}

	gs_app_set_runtime (app, runtime);

	return TRUE;
}

static gboolean
gs_flatpak_app_source_equals_remote (GsApp *source, FlatpakRemote *remote)
{
	const gchar *source_url = gs_app_get_metadata_item (source, "flatpak::url");
	g_autofree gchar *remote_url = flatpak_remote_get_url (remote);
	return g_strcmp0 (source_url, remote_url) == 0;
}

static FlatpakRemote *
gs_flatpak_get_remote_for_source (GsFlatpak *self,
				  GsApp *app,
				  gboolean *same_name_remote_exists,
				  GCancellable *cancellable)
{
	FlatpakRemote *remote = NULL;
	g_autoptr(GPtrArray) remotes = NULL;

	/* check an eventual remote with the same name */
	remote = flatpak_installation_get_remote_by_name (self->installation,
							  gs_app_get_id (app),
							  cancellable, NULL);

	if (remote != NULL) {
		if (gs_flatpak_app_source_equals_remote (app, remote))
			return remote;

		g_object_unref (remote);
		*same_name_remote_exists = TRUE;
		return NULL;
	}

	remotes = flatpak_installation_list_remotes (self->installation,
						     cancellable,
						     NULL);
	if (remotes == NULL) {
		return NULL;
	}

	for (guint i = 0; i < remotes->len; ++i) {
		remote = g_ptr_array_index (remotes, i);
		if (gs_flatpak_app_source_equals_remote (app, remote))
			return g_object_ref (remote);
	}

	return NULL;
}

static void
gs_flatpak_setup_source_from_remote (GsApp *source,
				     FlatpakRemote *remote)
{
	g_autofree gchar *remote_title = flatpak_remote_get_title (remote);
	g_autofree gchar *remote_url = flatpak_remote_get_url (remote);

	gs_app_set_id (source, remote_title);
	gs_app_set_metadata (source, "flatpak::url", remote_url);
}

static void
gs_flatpak_set_unique_name_for_source (GsFlatpak *self,
				       GsApp *source,
				       GCancellable *cancellable)
{
	const gchar *base_name = gs_app_get_id (source);

	for (guint i = 1; i < G_MAXUINT; ++i) {
		g_autoptr(FlatpakRemote) remote = NULL;
		g_autofree gchar *new_name = g_strdup_printf ("%s_%u", base_name, i);

		remote = flatpak_installation_get_remote_by_name (self->installation,
								  new_name, cancellable,
								  NULL);

		if (remote == NULL) {
			gs_app_set_id (source, new_name);
			return;
		}
	}
}

static GsApp *
gs_flatpak_create_app_from_repo_file (GsFlatpak *self,
				      GFile *file,
				      GCancellable *cancellable,
				      GError **error)
{
	gchar *tmp;
	g_autofree gchar *filename = NULL;
	g_autofree gchar *repo_comment = NULL;
	g_autofree gchar *repo_default_branch = NULL;
	g_autofree gchar *repo_description = NULL;
	g_autofree gchar *repo_gpgkey = NULL;
	g_autofree gchar *repo_homepage = NULL;
	g_autofree gchar *repo_icon = NULL;
	g_autofree gchar *repo_id = NULL;
	g_autofree gchar *repo_title = NULL;
	g_autofree gchar *repo_url = NULL;
	g_autoptr(GError) error_local = NULL;
	g_autoptr(GKeyFile) kf = NULL;
	g_autoptr(GsApp) app = NULL;
	g_autoptr(FlatpakRemote) remote = NULL;
	gboolean same_name_remote_exists = FALSE;

	/* read the file */
	kf = g_key_file_new ();
	filename = g_file_get_path (file);
	if (!g_key_file_load_from_file (kf, filename,
					G_KEY_FILE_NONE,
					&error_local)) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_NOT_SUPPORTED,
			     "failed to load flatpakrepo: %s",
			     error_local->message);
		return NULL;
	}

	/* get the ID from the basename */
	repo_id = g_file_get_basename (file);
	tmp = g_strrstr (repo_id, ".");
	if (tmp != NULL)
		*tmp = '\0';

	/* create source */
	repo_title = g_key_file_get_string (kf, "Flatpak Repo", "Title", NULL);
	repo_url = g_key_file_get_string (kf, "Flatpak Repo", "Url", NULL);
	repo_gpgkey = g_key_file_get_string (kf, "Flatpak Repo", "GPGKey", NULL);
	if (repo_title == NULL || repo_url == NULL || repo_gpgkey == NULL ||
	    repo_title[0] == '\0' || repo_url[0] == '\0' || repo_gpgkey[0] == '\0') {
		g_set_error_literal (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_NOT_SUPPORTED,
				     "not enough data in file, "
				     "expected Title, Url, GPGKey");
		return NULL;
	}

	/* check version */
	if (g_key_file_has_key (kf, "Flatpak Repo", "Version", NULL)) {
		guint64 ver = g_key_file_get_uint64 (kf, "Flatpak Repo", "Version", NULL);
		if (ver != 1) {
			g_set_error (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_NOT_SUPPORTED,
				     "unsupported version %" G_GUINT64_FORMAT, ver);
			return NULL;
		}
	}

	/* user specified a URL */
	if (g_str_has_prefix (repo_gpgkey, "http://") ||
	    g_str_has_prefix (repo_gpgkey, "https://")) {
		g_set_error_literal (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_NOT_SUPPORTED,
				     "Base64 encoded GPGKey required, not URL");
		return NULL;
	}

	/* create source */
	app = gs_app_new (repo_id);
	gs_app_set_flatpak_file_type (app, "flatpakrepo");
	gs_app_set_kind (app, AS_APP_KIND_SOURCE);
	gs_app_set_state (app, AS_APP_STATE_AVAILABLE);
	gs_app_add_quirk (app, AS_APP_QUIRK_NOT_LAUNCHABLE);
	gs_app_set_name (app, GS_APP_QUALITY_NORMAL, repo_title);
	gs_app_set_metadata (app, "flatpak::gpg-key", repo_gpgkey);
	gs_app_set_metadata (app, "flatpak::url", repo_url);
	gs_app_set_origin_hostname (app, repo_url);
	gs_app_set_management_plugin (app, gs_plugin_get_name (self->plugin));

	/* optional data */
	repo_homepage = g_key_file_get_string (kf, "Flatpak Repo", "Homepage", NULL);
	if (repo_homepage != NULL)
		gs_app_set_url (app, AS_URL_KIND_HOMEPAGE, repo_homepage);
	repo_comment = g_key_file_get_string (kf, "Flatpak Repo", "Comment", NULL);
	if (repo_comment != NULL)
		gs_app_set_summary (app, GS_APP_QUALITY_NORMAL, repo_comment);
	repo_description = g_key_file_get_string (kf, "Flatpak Repo", "Description", NULL);
	if (repo_description != NULL)
		gs_app_set_description (app, GS_APP_QUALITY_NORMAL, repo_description);
	repo_default_branch = g_key_file_get_string (kf, "Flatpak Repo", "DefaultBranch", NULL);
	if (repo_default_branch != NULL)
		gs_app_set_branch (app, repo_default_branch);
	repo_icon = g_key_file_get_string (kf, "Flatpak Repo", "Icon", NULL);
	if (repo_icon != NULL) {
		g_autoptr(AsIcon) ic = as_icon_new ();
		as_icon_set_kind (ic, AS_ICON_KIND_REMOTE);
		as_icon_set_url (ic, repo_icon);
		gs_app_add_icon (app, ic);
	}

	remote = gs_flatpak_get_remote_for_source (self, app, &same_name_remote_exists,
						   cancellable);
	if (remote != NULL) {
		gs_flatpak_setup_source_from_remote (app, remote);
		gs_app_set_state (app, AS_APP_STATE_UNKNOWN);
		gs_app_set_state (app, AS_APP_STATE_INSTALLED);
	} else if (same_name_remote_exists)
		gs_flatpak_set_unique_name_for_source (self, app, cancellable);

	/* success */
	return g_steal_pointer (&app);
}

gboolean
gs_flatpak_app_install (GsFlatpak *self,
			GsApp *app,
			GCancellable *cancellable,
			GError **error)
{
	GError *local_error = NULL;
	g_autoptr(FlatpakInstalledRef) xref = NULL;

	/* ensure we have metadata and state */
	if (!gs_flatpak_refine_app (self, app,
				    GS_PLUGIN_REFINE_FLAGS_REQUIRE_RUNTIME,
				    cancellable, error))
		return FALSE;

	/* install */
	gs_app_set_state (app, AS_APP_STATE_INSTALLING);

	/* add a source */
	if (gs_app_get_kind (app) == AS_APP_KIND_SOURCE) {
		return gs_flatpak_app_install_source (self,
						      app,
						      cancellable,
						      error);
	}

	/* flatpakref has to be done in two phases */
	if (g_strcmp0 (gs_app_get_flatpak_file_type (app), "flatpakref") == 0) {
		GsApp *runtime;
		g_autoptr(FlatpakRemoteRef) xref2 = NULL;
		gsize len = 0;
		g_autofree gchar *contents = NULL;
		g_autoptr(GBytes) data = NULL;
		if (gs_app_get_local_file (app) == NULL) {
			g_set_error (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_NOT_SUPPORTED,
				     "no local file set for flatpakref %s",
				     gs_app_get_unique_id (app));
			gs_app_set_state_recover (app);
			return FALSE;
		}
		g_debug ("installing flatpakref %s", gs_app_get_unique_id (app));
		if (!g_file_load_contents (gs_app_get_local_file (app),
					   cancellable, &contents, &len,
					   NULL, error)) {
			gs_utils_error_convert_gio (error);
			gs_app_set_state_recover (app);
			return FALSE;
		}

		/* we have a missing remote and a RuntimeRef */
		runtime = gs_app_get_runtime (app);
		if (runtime != NULL && gs_app_get_state (runtime) == AS_APP_STATE_UNKNOWN) {
			g_autofree gchar *cache_basename = NULL;
			g_autofree gchar *cache_fn = NULL;
			g_autoptr(GFile) file = NULL;
			g_autoptr(GsApp) app_src = NULL;
			const gchar *tmp = gs_app_get_metadata_item (app, "flatpak::runtime-repo");
			if (tmp == NULL) {
				g_set_error (error,
					     GS_PLUGIN_ERROR,
					     GS_PLUGIN_ERROR_NOT_SUPPORTED,
					     "no runtime available for %s",
					     gs_app_get_unique_id (app));
				gs_utils_error_add_unique_id (error, runtime);
				return FALSE;
			}
			g_debug ("runtime not available, so using %s", tmp);

			/* download file */
			cache_basename = g_path_get_basename (tmp);
			cache_fn = gs_utils_get_cache_filename ("flatpak",
								cache_basename,
								GS_UTILS_CACHE_FLAG_WRITEABLE,
								error);
			if (cache_fn == NULL)
				return FALSE;
			if (!gs_plugin_download_file (self->plugin,
						      runtime,
						      tmp,
						      cache_fn,
						      cancellable,
						      error))
				return FALSE;

			/* get GsApp for local file */
			file = g_file_new_for_path (cache_fn);
			app_src = gs_flatpak_create_app_from_repo_file (self,
									file,
									cancellable,
									error);
			if (app_src == NULL) {
				g_prefix_error (error,
						"cannot create source from %s: ",
						cache_fn);
				return FALSE;
			}

			/* install the flatpakrepo */
			if (!gs_flatpak_app_install_source (self,
							    app_src,
							    cancellable,
							    error)) {
				g_prefix_error (error,
						"cannot install source from %s: ",
						cache_fn);
				return FALSE;
			}

			/* get the new state */
			if (!gs_plugin_refine_item_state (self, runtime, cancellable, error)) {
				g_prefix_error (error,
						"cannot refine runtime using %s: ",
						cache_fn);
				return FALSE;
			}

			/* still not found */
			if (gs_app_get_state (runtime) == AS_APP_STATE_UNKNOWN) {
				g_set_error (error,
					     GS_PLUGIN_ERROR,
					     GS_PLUGIN_ERROR_NOT_SUPPORTED,
					     "no runtime available for %s",
					     gs_app_get_unique_id (app));
				gs_utils_error_add_unique_id (error, runtime);
				return FALSE;
			}
		}

		/* now install actual app */
		data = g_bytes_new (contents, len);
		xref2 = flatpak_installation_install_ref_file (self->installation,
							      data,
							      cancellable,
							      error);
		if (xref2 == NULL) {
			gs_plugin_flatpak_error_convert (error);
			gs_app_set_state_recover (app);
			return FALSE;
		}

		/* update search tokens for new remote */
		if (!gs_flatpak_refresh_appstream (self, G_MAXUINT, 0, cancellable, error))
			return FALSE;
	}

	/* install required runtime if not already installed */
	if (gs_app_get_kind (app) == AS_APP_KIND_DESKTOP &&
	    !install_runtime_for_app (self, app, cancellable, error)) {
		gs_app_set_state_recover (app);
		return FALSE;
	}

	if (g_strcmp0 (gs_app_get_flatpak_file_type (app), "flatpak") == 0) {
		if (gs_app_get_local_file (app) == NULL) {
			g_set_error (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_NOT_SUPPORTED,
				     "no local file set for bundle %s",
				     gs_app_get_unique_id (app));
			return FALSE;
		}
		g_debug ("installing bundle %s", gs_app_get_unique_id (app));
		xref = flatpak_installation_install_bundle (self->installation,
							    gs_app_get_local_file (app),
							    gs_flatpak_progress_cb,
							    app,
							    cancellable,
							    &local_error);
	} else {
		/* no origin set */
		if (gs_app_get_origin (app) == NULL) {
			g_set_error (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_NOT_SUPPORTED,
				     "no origin set for remote %s",
				     gs_app_get_unique_id (app));
			return FALSE;
		}
		g_debug ("installing %s", gs_app_get_id (app));
		xref = flatpak_installation_install (self->installation,
						     gs_app_get_origin (app),
						     gs_app_get_flatpak_kind (app),
						     gs_app_get_flatpak_name (app),
						     gs_app_get_flatpak_arch (app),
						     gs_app_get_flatpak_branch (app),
						     gs_flatpak_progress_cb, app,
						     cancellable,
						     &local_error);
	}
	if (xref == NULL) {
		gboolean already_installed =
			g_error_matches (local_error,
					 FLATPAK_ERROR,
					 FLATPAK_ERROR_ALREADY_INSTALLED);
		if (!already_installed) {
			g_propagate_error (error, local_error);
			gs_plugin_flatpak_error_convert (error);
			gs_app_set_state_recover (app);
			return FALSE;
		}

		g_clear_error (&local_error);
		g_debug ("Error: %s is already installed; "
			 "marking the GsApp as installed and continuing...",
			 gs_app_get_unique_id (app));
	}

	/* state is known */
	gs_app_set_state (app, AS_APP_STATE_INSTALLED);

	/* set new version */
	if (!gs_flatpak_refine_appstream (self, app, error))
		return FALSE;

	return TRUE;
}

gboolean
gs_flatpak_update_app (GsFlatpak *self,
		       GsApp *app,
		       GCancellable *cancellable,
		       GError **error)
{
	GError *local_error = NULL;
	g_autoptr(FlatpakInstalledRef) xref = NULL;

	/* install */
	gs_app_set_state (app, AS_APP_STATE_INSTALLING);

	/* install required runtime if not already installed */
	if (gs_app_get_kind (app) == AS_APP_KIND_DESKTOP &&
	    !install_runtime_for_app (self, app, cancellable, error)) {
		gs_app_set_state_recover (app);
		return FALSE;
	}

	xref = flatpak_installation_update (self->installation,
					    FLATPAK_UPDATE_FLAGS_NONE,
					    gs_app_get_flatpak_kind (app),
					    gs_app_get_flatpak_name (app),
					    gs_app_get_flatpak_arch (app),
					    gs_app_get_flatpak_branch (app),
					    gs_flatpak_progress_cb, app,
					    cancellable, &local_error);
	/* workaround for when file objects are missing and a static delta
	 * cannot be used */
	if (xref == NULL && g_error_matches (local_error, G_IO_ERROR,
					     G_IO_ERROR_NOT_FOUND)) {
		g_debug ("Error due to missing file when updating '%s': %s. "
			 "Retrying the update without static deltas...",
			 gs_app_get_unique_id (app),
			 local_error->message);
		g_clear_error (&local_error);

		/* pull without static deltas */
		xref = flatpak_installation_update (self->installation,
						    FLATPAK_UPDATE_FLAGS_NO_STATIC_DELTAS,
						    gs_app_get_flatpak_kind (app),
						    gs_app_get_flatpak_name (app),
						    gs_app_get_flatpak_arch (app),
						    gs_app_get_flatpak_branch (app),
						    gs_flatpak_progress_cb, app,
						    cancellable, &local_error);
	}
	if (xref == NULL) {
		gboolean already_installed =
			g_error_matches (local_error,
					 FLATPAK_ERROR,
					 FLATPAK_ERROR_ALREADY_INSTALLED);
		if (!already_installed) {
			g_propagate_error (error, local_error);
			gs_plugin_flatpak_error_convert (error);
			gs_app_set_state_recover (app);
			return FALSE;
		}

		g_clear_error (&local_error);
		g_debug ("Error: update for %s is already installed; "
			 "marking the GsApp as installed and continuing...",
			 gs_app_get_unique_id (app));
	}

	/* update UI */
	gs_plugin_updates_changed (self->plugin);

	/* state is known */
	gs_app_set_state (app, AS_APP_STATE_INSTALLED);
	gs_app_set_update_version (app, NULL);
	gs_app_set_update_details (app, NULL);
	gs_app_set_update_urgency (app, AS_URGENCY_KIND_UNKNOWN);

	/* set new version */
	if (!gs_flatpak_refine_appstream (self, app, error))
		return FALSE;

	return TRUE;
}

static gboolean
gs_flatpak_file_to_app_bundle (GsFlatpak *self,
			       GsAppList *list,
			       GFile *file,
			       GCancellable *cancellable,
			       GError **error)
{
	gint size;
	g_autofree gchar *content_type = NULL;
	g_autoptr(GBytes) appstream_gz = NULL;
	g_autoptr(GBytes) icon_data = NULL;
	g_autoptr(GBytes) metadata = NULL;
	g_autoptr(GsApp) app = NULL;
	g_autoptr(FlatpakBundleRef) xref_bundle = NULL;

	/* load bundle */
	xref_bundle = flatpak_bundle_ref_new (file, error);
	if (xref_bundle == NULL) {
		gs_plugin_flatpak_error_convert (error);
		g_prefix_error (error, "error loading bundle: ");
		return FALSE;
	}

	/* load metadata */
	app = gs_flatpak_create_app (self, FLATPAK_REF (xref_bundle));
	gs_app_set_flatpak_file_type (app, "flatpak");
	gs_app_set_kind (app, AS_APP_KIND_DESKTOP);
	gs_app_set_state (app, AS_APP_STATE_AVAILABLE_LOCAL);
	gs_app_set_size_installed (app, flatpak_bundle_ref_get_installed_size (xref_bundle));
	gs_flatpak_set_metadata (self, app, FLATPAK_REF (xref_bundle));
	metadata = flatpak_bundle_ref_get_metadata (xref_bundle);
	if (!gs_flatpak_set_app_metadata (self, app,
					  g_bytes_get_data (metadata, NULL),
					  g_bytes_get_size (metadata),
					  error))
		return FALSE;

	/* load AppStream */
	appstream_gz = flatpak_bundle_ref_get_appstream (xref_bundle);
	if (appstream_gz != NULL) {
		g_autoptr(GZlibDecompressor) decompressor = NULL;
		g_autoptr(GInputStream) stream_gz = NULL;
		g_autoptr(GInputStream) stream_data = NULL;
		g_autoptr(GBytes) appstream = NULL;
		g_autoptr(AsStore) store = NULL;
		g_autofree gchar *id = NULL;
		AsApp *item;

		/* decompress data */
		decompressor = g_zlib_decompressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP);
		stream_gz = g_memory_input_stream_new_from_bytes (appstream_gz);
		if (stream_gz == NULL)
			return FALSE;
		stream_data = g_converter_input_stream_new (stream_gz,
							    G_CONVERTER (decompressor));

		appstream = g_input_stream_read_bytes (stream_data,
						       0x100000, /* 1Mb */
						       cancellable,
						       error);
		if (appstream == NULL) {
			gs_plugin_flatpak_error_convert (error);
			return FALSE;
		}
		store = as_store_new ();
		if (!as_store_from_bytes (store, appstream, cancellable, error)) {
			gs_plugin_flatpak_error_convert (error);
			return FALSE;
		}

		/* allow peeking into this for debugging */
		if (g_getenv ("GS_FLATPAK_DEBUG_APPSTREAM") != NULL) {
			g_autoptr(GString) str = NULL;
			str = as_store_to_xml (store,
					       AS_NODE_TO_XML_FLAG_FORMAT_MULTILINE |
					       AS_NODE_TO_XML_FLAG_FORMAT_INDENT);
			g_debug ("showing AppStream data: %s", str->str);
		}

		/* check for sanity */
		if (as_store_get_size (store) == 0) {
			g_set_error_literal (error,
					     GS_PLUGIN_ERROR,
					     GS_PLUGIN_ERROR_NOT_SUPPORTED,
					     "no apps found in AppStream data");
			return FALSE;
		}
		g_debug ("%u applications found in AppStream data",
			 as_store_get_size (store));

		/* find app */
		id = g_strdup_printf ("%s.desktop", gs_app_get_flatpak_name (app));
		item = as_store_get_app_by_id (store, id);
		if (item == NULL) {
			g_set_error (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_INVALID_FORMAT,
				     "application %s not found",
				     id);
			return FALSE;
		}

		/* copy details from AppStream to app */
		if (!gs_appstream_refine_app (self->plugin, app, item, error))
			return FALSE;
	} else {
		g_warning ("no appstream metadata in file");
		gs_app_set_name (app, GS_APP_QUALITY_LOWEST,
				 gs_app_get_flatpak_name (app));
		gs_app_set_summary (app, GS_APP_QUALITY_LOWEST,
				    "A flatpak application");
	}

	/* load icon */
	size = 64 * (gint) gs_plugin_get_scale (self->plugin);
	icon_data = flatpak_bundle_ref_get_icon (xref_bundle, size);
	if (icon_data == NULL)
		icon_data = flatpak_bundle_ref_get_icon (xref_bundle, 64);
	if (icon_data != NULL) {
		g_autoptr(GInputStream) stream_icon = NULL;
		g_autoptr(GdkPixbuf) pixbuf = NULL;
		stream_icon = g_memory_input_stream_new_from_bytes (icon_data);
		pixbuf = gdk_pixbuf_new_from_stream (stream_icon, cancellable, error);
		if (pixbuf == NULL) {
			gs_utils_error_convert_gdk_pixbuf (error);
			return FALSE;
		}
		gs_app_set_pixbuf (app, pixbuf);
	} else {
		g_autoptr(AsIcon) icon = NULL;
		icon = as_icon_new ();
		as_icon_set_kind (icon, AS_ICON_KIND_STOCK);
		as_icon_set_name (icon, "application-x-executable");
		gs_app_add_icon (app, icon);
	}

	/* not quite true: this just means we can update this specific app */
	if (flatpak_bundle_ref_get_origin (xref_bundle))
		gs_app_add_quirk (app, AS_APP_QUIRK_HAS_SOURCE);

	/* success */
	gs_app_list_add (list, app);
	return TRUE;
}

static gboolean
gs_flatpak_file_to_app_repo (GsFlatpak *self,
			     GsAppList *list,
			     GFile *file,
			     GCancellable *cancellable,
			     GError **error)
{
	g_autoptr(GsApp) app = NULL;
	g_autoptr(FlatpakRemote) xremote = NULL;

	/* create app */
	app = gs_flatpak_create_app_from_repo_file (self, file, cancellable, error);
	if (app == NULL)
		return FALSE;

	/* check to see if the repo ID already exists */
	xremote = flatpak_installation_get_remote_by_name (self->installation,
							   gs_app_get_id (app),
							   cancellable, NULL);
	if (xremote != NULL) {
		g_debug ("repo %s already exists", gs_app_get_id (app));
		gs_app_set_state (app, AS_APP_STATE_UNKNOWN);
		gs_app_set_state (app, AS_APP_STATE_INSTALLED);
	} else {
		gs_app_set_state (app, AS_APP_STATE_AVAILABLE);
	}

	/* save to the cache */
	gs_plugin_cache_add (self->plugin, NULL, app);

	/* success */
	gs_app_list_add (list, app);
	return TRUE;
}

static gboolean
gs_flatpak_file_to_app_ref (GsFlatpak *self,
			    GsAppList *list,
			    GFile *file,
			    GCancellable *cancellable,
			    GError **error)
{
	const gchar *remote_name;
	gsize len = 0;
	g_autofree gchar *contents = NULL;
	g_autoptr(FlatpakRemoteRef) xref = NULL;
	g_autoptr(GBytes) ref_file_data = NULL;
	g_autoptr(GError) error_local = NULL;
	g_autoptr(GsApp) app = NULL;
	g_autoptr(FlatpakRemote) xremote = NULL;
	g_autoptr(GKeyFile) kf = NULL;
	g_autofree gchar *origin_url = NULL;
	g_autofree gchar *origin_title = NULL;
	g_autofree gchar *ref_comment = NULL;
	g_autofree gchar *ref_description = NULL;
	g_autofree gchar *ref_homepage = NULL;
	g_autofree gchar *ref_icon = NULL;
	g_autofree gchar *ref_title = NULL;
	g_autofree gchar *ref_name = NULL;
	g_autofree gchar *ref_runtime_repo = NULL;

	/* get file data */
	if (!g_file_load_contents (file,
				   cancellable,
				   &contents,
				   &len,
				   NULL,
				   error)) {
		gs_utils_error_convert_gio (error);
		return FALSE;
	}

	/* load the file */
	kf = g_key_file_new ();
	if (!g_key_file_load_from_data (kf, contents, len, G_KEY_FILE_NONE, error)) {
		gs_utils_error_convert_gio (error);
		return FALSE;
	}

	/* check version */
	if (g_key_file_has_key (kf, "Flatpak Ref", "Version", NULL)) {
		guint64 ver = g_key_file_get_uint64 (kf, "Flatpak Ref", "Version", NULL);
		if (ver != 1) {
			g_set_error (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_NOT_SUPPORTED,
				     "unsupported version %" G_GUINT64_FORMAT, ver);
			return FALSE;
		}
	}

	/* get name */
	ref_name = g_key_file_get_string (kf, "Flatpak Ref", "Name", error);
	if (ref_name == NULL) {
		gs_utils_error_convert_gio (error);
		return FALSE;
	}

	/* remove old version from the remote config */
	if (self->flags & GS_FLATPAK_FLAG_IS_TEMPORARY) {
		g_autofree gchar *remote_id_tmp = NULL;
		g_autofree gchar *remote_name_tmp = NULL;
		g_autoptr(FlatpakRemote) xremote_tmp = NULL;
		remote_name_tmp = g_strdup_printf ("%s-origin", ref_name);
		xremote_tmp = flatpak_installation_get_remote_by_name (self->installation,
								       remote_name_tmp,
								       cancellable,
								       NULL);
		if (xremote_tmp != NULL) {
			g_debug ("removing previous remote %s", remote_name_tmp);
			if (!flatpak_installation_remove_remote (self->installation,
								 remote_name_tmp,
								 cancellable,
								 error)) {
				gs_plugin_flatpak_error_convert (error);
				return FALSE;
			}
		} else {
			g_debug ("no previous %s remote to remove", remote_name_tmp);
		}

		/* remove from the store */
		remote_id_tmp = g_strdup_printf ("%s.desktop", ref_name);
		as_store_remove_app_by_id (self->store, remote_id_tmp);
	}

	/* install the remote, but not the app */
	ref_file_data = g_bytes_new (contents, len);
	xref = flatpak_installation_install_ref_file (self->installation,
						      ref_file_data,
						      cancellable,
						      error);
	if (xref == NULL) {
		gs_plugin_flatpak_error_convert (error);
		return FALSE;
	}

	/* load metadata */
	app = gs_flatpak_create_app (self, FLATPAK_REF (xref));
	gs_app_add_quirk (app, AS_APP_QUIRK_HAS_SOURCE);
	gs_app_set_flatpak_file_type (app, "flatpakref");
	gs_app_set_kind (app, AS_APP_KIND_DESKTOP);
	gs_app_set_state (app, AS_APP_STATE_AVAILABLE_LOCAL);
	gs_flatpak_set_metadata (self, app, FLATPAK_REF (xref));

	/* use the data from the flatpakref file as a fallback */
	ref_title = g_key_file_get_string (kf, "Flatpak Ref", "Title", NULL);
	if (ref_title != NULL)
		gs_app_set_name (app, GS_APP_QUALITY_NORMAL, ref_title);
	ref_comment = g_key_file_get_string (kf, "Flatpak Ref", "Comment", NULL);
	if (ref_comment != NULL)
		gs_app_set_summary (app, GS_APP_QUALITY_NORMAL, ref_comment);
	ref_description = g_key_file_get_string (kf, "Flatpak Ref", "Description", NULL);
	if (ref_description != NULL)
		gs_app_set_description (app, GS_APP_QUALITY_NORMAL, ref_description);
	ref_homepage = g_key_file_get_string (kf, "Flatpak Ref", "Homepage", NULL);
	if (ref_homepage != NULL)
		gs_app_set_url (app, AS_URL_KIND_HOMEPAGE, ref_homepage);
	ref_icon = g_key_file_get_string (kf, "Flatpak Ref", "Icon", NULL);
	if (ref_icon != NULL) {
		g_autoptr(AsIcon) ic = as_icon_new ();
		as_icon_set_kind (ic, AS_ICON_KIND_REMOTE);
		as_icon_set_url (ic, ref_icon);
		gs_app_add_icon (app, ic);
	}
	ref_runtime_repo = g_key_file_get_string (kf, "Flatpak Ref", "RuntimeRepo", NULL);
	if (ref_runtime_repo != NULL)
		gs_app_set_metadata (app, "flatpak::runtime-repo", ref_runtime_repo);

	/* set the origin data */
	remote_name = flatpak_remote_ref_get_remote_name (xref);
	g_debug ("auto-created remote name: %s", remote_name);
	xremote = flatpak_installation_get_remote_by_name (self->installation,
							   remote_name,
							   cancellable,
							   error);
	if (xremote == NULL) {
		gs_plugin_flatpak_error_convert (error);
		return FALSE;
	}
	origin_title = flatpak_remote_get_title (xremote);
	origin_url = flatpak_remote_get_url (xremote);
	if (origin_url == NULL) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_INVALID_FORMAT,
			     "no URL for remote %s",
			     flatpak_remote_get_name (xremote));
		return FALSE;
	}
	gs_app_set_origin (app, remote_name);
	gs_app_set_origin_hostname (app, origin_url);
	gs_app_set_origin_ui (app, origin_title);

	/* get the new appstream data (nonfatal for failure) */
	if (!gs_flatpak_refresh_appstream_remote (self, remote_name,
						  cancellable, &error_local)) {
		g_autoptr(GsPluginEvent) event = gs_plugin_event_new ();
		gs_plugin_flatpak_error_convert (&error_local);
		gs_plugin_event_set_app (event, app);
		gs_plugin_event_set_error (event, error_local);
		gs_plugin_event_add_flag (event, GS_PLUGIN_EVENT_FLAG_WARNING);
		gs_plugin_report_event (self->plugin, event);
	}

	/* get this now, as it's not going to be available at install time */
	if (!gs_plugin_refine_item_metadata (self, app, cancellable, error))
		return FALSE;

	/* parse it */
	if (!gs_flatpak_add_apps_from_xremote (self, xremote, cancellable, error))
		return FALSE;

	/* success */
	gs_app_list_add (list, app);
	return TRUE;
}

gboolean
gs_flatpak_file_to_app (GsFlatpak *self,
			GsAppList *list,
			GFile *file,
			GCancellable *cancellable,
			GError **error)
{
	g_autofree gchar *content_type = NULL;
	const gchar *mimetypes_bundle[] = {
		"application/vnd.flatpak",
		NULL };
	const gchar *mimetypes_repo[] = {
		"application/vnd.flatpak.repo",
		NULL };
	const gchar *mimetypes_ref[] = {
		"application/vnd.flatpak.ref",
		NULL };

	/* does this match any of the mimetypes_bundle we support */
	content_type = gs_utils_get_content_type (file, cancellable, error);
	if (content_type == NULL)
		return FALSE;
	if (g_strv_contains (mimetypes_bundle, content_type)) {
		return gs_flatpak_file_to_app_bundle (self,
						      list,
						      file,
						      cancellable,
						      error);
	}
	if (g_strv_contains (mimetypes_repo, content_type)) {
		return gs_flatpak_file_to_app_repo (self,
						    list,
						    file,
						    cancellable,
						    error);
	}
	if (self->flags & GS_FLATPAK_FLAG_IS_TEMPORARY &&
	    g_strv_contains (mimetypes_ref, content_type)) {
		return gs_flatpak_file_to_app_ref (self,
						   list,
						   file,
						   cancellable,
						   error);
	}
	return TRUE;
}

gboolean
gs_flatpak_search (GsFlatpak *self,
		   gchar **values,
		   GsAppList *list,
		   GCancellable *cancellable,
		   GError **error)
{
	return gs_appstream_store_search (self->plugin, self->store,
					  values, list,
					  cancellable, error);
}

gboolean
gs_flatpak_add_category_apps (GsFlatpak *self,
			      GsCategory *category,
			      GsAppList *list,
			      GCancellable *cancellable,
			      GError **error)
{
	return gs_appstream_store_add_category_apps (self->plugin, self->store,
						     category, list,
						     cancellable, error);
}

gboolean
gs_flatpak_add_categories (GsFlatpak *self,
			   GPtrArray *list,
			   GCancellable *cancellable,
			   GError **error)
{
	return gs_appstream_store_add_categories (self->plugin, self->store,
						  list, cancellable, error);
}

gboolean
gs_flatpak_add_popular (GsFlatpak *self,
			GsAppList *list,
			GCancellable *cancellable,
			GError **error)
{
	return gs_appstream_add_popular (self->plugin, self->store, list,
					 cancellable, error);
}

gboolean
gs_flatpak_add_featured (GsFlatpak *self,
			 GsAppList *list,
			 GCancellable *cancellable,
			 GError **error)
{
	return gs_appstream_add_featured (self->plugin, self->store, list,
					  cancellable, error);
}

static void
gs_flatpak_store_app_added_cb (AsStore *store, AsApp *app, GsFlatpak *self)
{
	gs_appstream_add_extra_info (self->plugin, app);
}

static void
gs_flatpak_store_app_removed_cb (AsStore *store, AsApp *app, GsFlatpak *self)
{
	g_debug ("AppStream app was removed, doing delete from global cache");
	gs_plugin_cache_remove (self->plugin, as_app_get_unique_id (app));
}

const gchar *
gs_flatpak_get_id (GsFlatpak *self)
{
	if (self->id == NULL) {
		GString *str = g_string_new ("GsFlatpak");
		g_string_append_printf (str, "-%s",
					as_app_scope_to_string (self->scope));
		if (flatpak_installation_get_id (self->installation) != NULL) {
			g_string_append_printf (str, "-%s",
						flatpak_installation_get_id (self->installation));
		}
		if (self->flags & GS_FLATPAK_FLAG_IS_TEMPORARY)
			g_string_append (str, "-temp");
		self->id = g_string_free (str, FALSE);
	}
	return self->id;
}

AsAppScope
gs_flatpak_get_scope (GsFlatpak *self)
{
	return self->scope;
}

void
gs_flatpak_set_flags (GsFlatpak *self, GsFlatpakFlags flags)
{
	self->flags = flags;
}

GsFlatpakFlags
gs_flatpak_get_flags (GsFlatpak *self)
{
	return self->flags;
}

static void
gs_flatpak_finalize (GObject *object)
{
	GsFlatpak *self;
	g_return_if_fail (GS_IS_FLATPAK (object));
	self = GS_FLATPAK (object);

	if (self->changed_id > 0) {
		g_signal_handler_disconnect (self->monitor, self->changed_id);
		self->changed_id = 0;
	}

	g_free (self->id);
	g_object_unref (self->installation);
	g_object_unref (self->plugin);
	g_object_unref (self->store);
	g_hash_table_unref (self->broken_remotes);

	G_OBJECT_CLASS (gs_flatpak_parent_class)->finalize (object);
}

static void
gs_flatpak_class_init (GsFlatpakClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = gs_flatpak_finalize;
}

static void
gs_flatpak_init (GsFlatpak *self)
{
	self->broken_remotes = g_hash_table_new_full (g_str_hash, g_str_equal,
						      g_free, NULL);
	self->store = as_store_new ();
	g_signal_connect (self->store, "app-added",
			  G_CALLBACK (gs_flatpak_store_app_added_cb),
			  self);
	g_signal_connect (self->store, "app-removed",
			  G_CALLBACK (gs_flatpak_store_app_removed_cb),
			  self);
	as_store_set_add_flags (self->store, AS_STORE_ADD_FLAG_USE_UNIQUE_ID);
	as_store_set_watch_flags (self->store, AS_STORE_WATCH_FLAG_REMOVED);
	as_store_set_search_match (self->store,
				   AS_APP_SEARCH_MATCH_MIMETYPE |
				   AS_APP_SEARCH_MATCH_PKGNAME |
				   AS_APP_SEARCH_MATCH_COMMENT |
				   AS_APP_SEARCH_MATCH_NAME |
				   AS_APP_SEARCH_MATCH_KEYWORD |
				   AS_APP_SEARCH_MATCH_ID);
}

GsFlatpak *
gs_flatpak_new (GsPlugin *plugin, FlatpakInstallation *installation)
{
	GsFlatpak *self;
	self = g_object_new (GS_TYPE_FLATPAK, NULL);
	self->installation = g_object_ref (installation);
	self->scope = flatpak_installation_get_is_user (installation)
				? AS_APP_SCOPE_USER : AS_APP_SCOPE_SYSTEM;
	self->plugin = g_object_ref (plugin);
	return GS_FLATPAK (self);
}
