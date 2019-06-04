/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2016-2018 Endless Mobile, Inc
 *
 * Authors:
 *   Joaquim Rocha <jrocha@endlessm.com>
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

#include <config.h>

#include <flatpak.h>
#include <libeos-parental-controls/app-filter.h>
#include <ostree.h>
#include <gnome-software.h>
#include <glib/gi18n.h>
#include <gs-plugin.h>
#include <gs-utils.h>
#include <libsoup/soup.h>
#include <math.h>
#include <sys/types.h>
#include <sys/xattr.h>

#include "eos-updater-generated.h"
#include "gs-flatpak.h"
#include "gs-flatpak-app.h"

#define ENDLESS_ID_PREFIX "com.endlessm."

#define EOS_IMAGE_VERSION_XATTR "user.eos-image-version"
#define EOS_IMAGE_VERSION_PATH "/sysroot"
#define EOS_IMAGE_VERSION_ALT_PATH "/"

#define METADATA_SYS_DESKTOP_FILE "EndlessOS::system-desktop-file"
#define METADATA_REPLACED_BY_DESKTOP_FILE "EndlessOS::replaced-by-desktop-file"
#define EOS_PROXY_APP_PREFIX ENDLESS_ID_PREFIX "proxy"

/*
 * SECTION:
 * Plugin to improve GNOME Software integration in the EOS desktop.
 */

typedef enum {
	EOS_UPDATER_STATE_NONE = 0,
	EOS_UPDATER_STATE_READY,
	EOS_UPDATER_STATE_ERROR,
	EOS_UPDATER_STATE_POLLING,
	EOS_UPDATER_STATE_UPDATE_AVAILABLE,
	EOS_UPDATER_STATE_FETCHING,
	EOS_UPDATER_STATE_UPDATE_READY,
	EOS_UPDATER_STATE_APPLYING_UPDATE,
	EOS_UPDATER_STATE_UPDATE_APPLIED,
	EOS_UPDATER_N_STATES,
} EosUpdaterState;

static const gchar *eos_updater_state_str[] = {"None",
					       "Ready",
					       "Error",
					       "Polling",
					       "UpdateAvailable",
					       "Fetching",
					       "UpdateReady",
					       "ApplyingUpdate",
					       "UpdateApplied"};

#define EOS_UPGRADE_ID "com.endlessm.EOS.upgrade"

/* the percentage of the progress bar to use for applying the OS upgrade;
 * we need to fake the progress in this percentage because applying the OS upgrade
 * can take a long time and we don't want the user to think that the upgrade has
 * stalled */
#define EOS_UPGRADE_APPLY_PROGRESS_RANGE 25 /* percentage */
#define EOS_UPGRADE_APPLY_MAX_TIME 600.0 /* sec */
#define EOS_UPGRADE_APPLY_STEP_TIME 0.250 /* sec */

static void setup_os_upgrade (GsPlugin *plugin);
static EosUpdaterState sync_state_from_updater (GsPlugin *plugin);
static gboolean app_is_flatpak (GsApp *app);

struct GsPluginData
{
	GDBusConnection *session_bus;
	GHashTable *desktop_apps;

	/* This hash table is for "replacement apps" for placeholders
	 * on the desktop. We are shipping systems with icons like
	 * "Get VLC" or "Get Spotify", which, when launched, open
	 * the app center. In any case where the user could install
	 * those apps, we want to ensure that we replace the icon
	 * on the desktop with the application's icon, in the same
	 * place. */
	GHashTable *replacement_app_lookup;
	int applications_changed_id;
	SoupSession *soup_session;
	char *personality;
	char *product_name;
	char *os_version_id;
	gboolean eos_arch_is_arm;
	EosUpdater *updater_proxy;
	GsApp *os_upgrade;
	GCancellable *os_upgrade_cancellable;
	gfloat upgrade_fake_progress;
	guint upgrade_fake_progress_handler;
};

static gboolean
get_applications_with_shortcuts (GsPlugin	*plugin,
				 GHashTable    **apps_table,
				 GCancellable	*cancellable,
				 GError		**error) {
	g_autoptr (GVariantIter) iter = NULL;
	g_autoptr (GVariant) apps = NULL;
	gchar *application;
	GsPluginData *priv = gs_plugin_get_data (plugin);

	*apps_table = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
					     NULL);

	apps = g_dbus_connection_call_sync (priv->session_bus,
					    "org.gnome.Shell",
					    "/org/gnome/Shell",
					    "org.gnome.Shell.AppStore",
					    "ListApplications",
					    NULL, NULL,
					    G_DBUS_CALL_FLAGS_NONE,
					    -1,
					    cancellable,
					    error);
	if (apps == NULL)
		return FALSE;

	g_variant_get (apps, "(as)", &iter);
	while (g_variant_iter_loop (iter, "s", &application))
		g_hash_table_add (*apps_table, g_strdup (application));

	return TRUE;
}

static void
on_desktop_apps_changed (GDBusConnection *connection,
			 const gchar	 *sender_name,
			 const gchar	 *object_path,
			 const gchar	 *interface_name,
			 const gchar	 *signal_name,
			 GVariant	 *parameters,
			 GsPlugin	 *plugin)
{
	g_autoptr(GHashTable) apps = NULL;
	GHashTableIter iter;
	gpointer key, value;
	GsPluginData *priv = gs_plugin_get_data (plugin);
	g_autoptr(GError) error = NULL;

	if (!get_applications_with_shortcuts (plugin, &apps, NULL, &error)) {
		g_warning ("Error getting apps with shortcuts: %s",
			   error->message);
		return;
	}

	/* remove any apps that no longer have shortcuts */
	g_hash_table_iter_init (&iter, priv->desktop_apps);
	while (g_hash_table_iter_next (&iter, &key, &value)) {
		GsApp *app = NULL;

		/* remove the key (if it exists) so we don't have to deal with
		 * it again in the next loop */
		if (g_hash_table_remove (apps, key))
			continue;

		app = gs_plugin_cache_lookup (plugin, key);
		if (app)
			gs_app_remove_quirk (app, GS_APP_QUIRK_HAS_SHORTCUT);

		g_hash_table_iter_remove (&iter);
	}

	/* add any apps that have shortcuts now */
	g_hash_table_iter_init (&iter, apps);
	while (g_hash_table_iter_next (&iter, &key, &value)) {
		GsApp *app = gs_plugin_cache_lookup (plugin, key);

		if (app)
			gs_app_add_quirk (app, GS_APP_QUIRK_HAS_SHORTCUT);

		g_hash_table_add (priv->desktop_apps, g_strdup (key));
	}
}

static char *
get_image_version_for_path (const char *path)
{
	ssize_t xattr_size = 0;
	char *image_version = NULL;

	xattr_size = getxattr (path, EOS_IMAGE_VERSION_XATTR, NULL, 0);

	if (xattr_size == -1)
		return NULL;

	image_version = g_malloc0 (xattr_size + 1);

	xattr_size = getxattr (path, EOS_IMAGE_VERSION_XATTR,
			       image_version, xattr_size);

	/* this check is just in case the xattr has changed in between the
	 * size checks */
	if (xattr_size == -1) {
		g_warning ("Error when getting the 'eos-image-version' from %s",
			   path);
		return NULL;
	}

	return image_version;
}

static char *
get_image_version (void)
{
	char *image_version = get_image_version_for_path (EOS_IMAGE_VERSION_PATH);

	if (!image_version)
		image_version = get_image_version_for_path (EOS_IMAGE_VERSION_ALT_PATH);

	return image_version;
}

static char *
get_personality (const char *image_version)
{
	g_auto(GStrv) tokens = NULL;
	guint num_tokens = 0;
	char *personality = NULL;

	if (!image_version)
		return NULL;

	tokens = g_strsplit (image_version, ".", 0);
	num_tokens = g_strv_length (tokens);
	personality = tokens[num_tokens - 1];

	return g_strdup (personality);
}

static char *
get_product_name (const char *image_version)
{
	char *hyphen_index = NULL;

	if (image_version == NULL)
		return NULL;

	hyphen_index = strchr (image_version, '-');
	if (hyphen_index == NULL)
		return NULL;

	return g_strndup (image_version, hyphen_index - image_version);
}

static char *
get_os_version_id (GError **error)
{
	g_autoptr(GsOsRelease) os_release = gs_os_release_new (error);

	if (!os_release)
		return NULL;

	return g_strdup (gs_os_release_get_version_id (os_release));
}

static void
read_icon_replacement_overrides (GHashTable *replacement_app_lookup)
{
	const gchar * const *datadirs = g_get_system_data_dirs ();
	g_autoptr(GError) error = NULL;

	for (; *datadirs; ++datadirs) {
		g_autofree gchar *candidate_path = g_build_filename (*datadirs,
                                                                     "eos-application-tools",
                                                                     "icon-overrides",
                                                                     "eos-icon-overrides.ini",
                                                                     NULL);
		g_autoptr(GKeyFile) config = g_key_file_new ();
		g_auto(GStrv) keys = NULL;
		gsize n_keys = 0;
		gsize key_iterator = 0;

		if (!g_key_file_load_from_file (config, candidate_path, G_KEY_FILE_NONE, &error)) {
			if (!g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
				g_warning ("Could not load icon overrides file %s: %s", candidate_path, error->message);
			g_clear_error (&error);
			continue;
		}

		if (!(keys = g_key_file_get_keys (config, "Overrides", &n_keys, &error))) {
			g_warning ("Could not read keys from icon overrides file %s: %s", candidate_path, error->message);
			g_clear_error (&error);
			continue;
		}

		/* Now add all the key-value pairs to the replacement app lookup table */
		for (; key_iterator != n_keys; ++key_iterator) {
			g_hash_table_replace (replacement_app_lookup,
					      g_strdup (keys[key_iterator]),
					      g_key_file_get_string (config,
								     "Overrides",
								     keys[key_iterator],
								     NULL));
		}

		/* First one takes priority, ignore the others */
		break;
	}
}

static void
os_upgrade_cancelled_cb (GCancellable *cancellable,
			 GsPlugin *plugin)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	eos_updater_call_cancel (priv->updater_proxy, NULL, NULL, NULL);
}

static void
setup_os_upgrade_cancellable (GsPlugin *plugin)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	GCancellable *cancellable = gs_app_get_cancellable (priv->os_upgrade);

	if (g_set_object (&priv->os_upgrade_cancellable, cancellable))
		g_cancellable_connect (priv->os_upgrade_cancellable,
				       G_CALLBACK (os_upgrade_cancelled_cb),
				       plugin, NULL);
}

static void
app_ensure_set_metadata_variant (GsApp *app, const gchar *key, GVariant *var)
{
	/* we need to assign it to NULL in order to be able to override it
	 * (safeguard mechanism in GsApp...) */
	gs_app_set_metadata_variant (app, key, NULL);
	gs_app_set_metadata_variant (app, key, var);
}

static void
os_upgrade_set_download_by_user (GsApp *app, gboolean value)
{
	g_autoptr(GVariant) var = g_variant_new_boolean (value);
	app_ensure_set_metadata_variant (app, "eos::DownloadByUser", var);
}

static gboolean
os_upgrade_get_download_by_user (GsApp *app)
{
	GVariant *value = gs_app_get_metadata_variant (app, "eos::DownloadByUser");
	if (value == NULL)
		return FALSE;
	return g_variant_get_boolean (value);
}

static void
os_upgrade_set_restart_on_error (GsApp *app, gboolean value)
{
	g_autoptr(GVariant) var = g_variant_new_boolean (value);
	app_ensure_set_metadata_variant (app, "eos::RestartOnError", var);
}

static gboolean
os_upgrade_get_restart_on_error (GsApp *app)
{
	GVariant *value = gs_app_get_metadata_variant (app, "eos::RestartOnError");
	if (value == NULL)
		return FALSE;
	return g_variant_get_boolean (value);
}

static void
os_upgrade_force_fetch (EosUpdater *updater)
{
	g_auto(GVariantDict) options_dict = G_VARIANT_DICT_INIT (NULL);
	g_variant_dict_insert (&options_dict, "force", "b", TRUE);

	eos_updater_call_fetch_full (updater,
				     g_variant_dict_end (&options_dict),
				     NULL, NULL, NULL);
}

static void
app_ensure_installing_state (GsApp *app)
{
	/* ensure the state transition to 'installing' is allowed */
	if (gs_app_get_state (app) != AS_APP_STATE_INSTALLING)
		gs_app_set_state (app, AS_APP_STATE_AVAILABLE);

	gs_app_set_state (app, AS_APP_STATE_INSTALLING);
}

static gboolean
eos_updater_error_is_cancelled (const gchar *error_name)
{
	return (g_strcmp0 (error_name, "com.endlessm.Updater.Error.Cancelled") == 0);
}

static void
updater_state_changed (GsPlugin *plugin)
{
	sync_state_from_updater (plugin);
}

static void
updater_downloaded_bytes_changed (GsPlugin *plugin)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);

	app_ensure_installing_state (priv->os_upgrade);
	sync_state_from_updater (plugin);
}

static void
updater_version_changed (GsPlugin *plugin)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	const gchar *version = eos_updater_get_version (priv->updater_proxy);

	gs_app_set_version (priv->os_upgrade, version);
}

static void
disable_os_updater (GsPlugin *plugin)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);

	if (priv->upgrade_fake_progress_handler != 0) {
		g_source_remove (priv->upgrade_fake_progress_handler);
		priv->upgrade_fake_progress_handler = 0;
	}

	if (priv->updater_proxy == NULL)
		return;

	g_signal_handlers_disconnect_by_func (priv->updater_proxy,
					      G_CALLBACK (updater_state_changed),
					      plugin);
	g_signal_handlers_disconnect_by_func (priv->updater_proxy,
					      G_CALLBACK (updater_version_changed),
					      plugin);

	g_cancellable_cancel (priv->os_upgrade_cancellable);
	g_clear_object (&priv->os_upgrade_cancellable);

	g_clear_object (&priv->updater_proxy);
}

static gboolean
fake_os_upgrade_progress (GsPlugin *plugin)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	gfloat normal_step;
	guint new_progress;
	const gfloat fake_progress_max = 99.0;

	if (eos_updater_get_state (priv->updater_proxy) != EOS_UPDATER_STATE_APPLYING_UPDATE ||
	    priv->upgrade_fake_progress > fake_progress_max) {
		priv->upgrade_fake_progress = 0;
		priv->upgrade_fake_progress_handler = 0;
		return G_SOURCE_REMOVE;
	}

	normal_step = (gfloat) EOS_UPGRADE_APPLY_PROGRESS_RANGE /
		      (EOS_UPGRADE_APPLY_MAX_TIME / EOS_UPGRADE_APPLY_STEP_TIME);

	priv->upgrade_fake_progress += normal_step;

	new_progress = (100 - EOS_UPGRADE_APPLY_PROGRESS_RANGE) +
		       (guint) round (priv->upgrade_fake_progress);
	gs_app_set_progress (priv->os_upgrade,
			     MIN (new_progress, (guint) fake_progress_max));

	g_debug ("OS upgrade fake progress: %f", priv->upgrade_fake_progress);

	return G_SOURCE_CONTINUE;
}

static gboolean
updater_is_stalled (GsPlugin *plugin)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	GsApp *app = priv->os_upgrade;

	/* in case the OS upgrade has been disabled */
	if (priv->updater_proxy == NULL)
		return FALSE;

	return eos_updater_get_state (priv->updater_proxy) == EOS_UPDATER_STATE_FETCHING &&
	       gs_app_get_state (app) != AS_APP_STATE_INSTALLING;
}

/* This method deals with the synchronization between the EOS updater's states
 * (DBus service) and the OS upgrade's states (GsApp), in order to show the user
 * what is happening and what they can do. */
static EosUpdaterState
sync_state_from_updater (GsPlugin *plugin)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	GsApp *app = priv->os_upgrade;
	EosUpdaterState state;
	AsAppState previous_app_state = gs_app_get_state (app);
	AsAppState current_app_state;
	const guint max_progress_for_update = 75;

	/* in case the OS upgrade has been disabled */
	if (priv->updater_proxy == NULL)
		return EOS_UPDATER_STATE_NONE;

	state = eos_updater_get_state (priv->updater_proxy);
	g_debug ("EOS Updater state changed: %s", eos_updater_state_str [state]);

	switch (state) {
	case EOS_UPDATER_STATE_NONE:
	case EOS_UPDATER_STATE_READY: {
		if (os_upgrade_get_download_by_user (app)) {
			app_ensure_installing_state (app);
			eos_updater_call_poll (priv->updater_proxy, NULL, NULL,
					       NULL);
		} else {
			gs_app_set_state (app, AS_APP_STATE_UNKNOWN);
		}
		break;
	} case EOS_UPDATER_STATE_POLLING: {
		if (os_upgrade_get_download_by_user (app))
			app_ensure_installing_state (app);
		break;
	} case EOS_UPDATER_STATE_UPDATE_AVAILABLE: {
		if (os_upgrade_get_download_by_user (app)) {
			app_ensure_installing_state (app);
			/* when the OS upgrade was started by the user and the
			 * updater reports an available update, (meaning we were
			 * polling before), we should readily call fetch */
			os_upgrade_force_fetch (priv->updater_proxy);
		} else {
			guint64 total_size =
				eos_updater_get_download_size (priv->updater_proxy);
			gs_app_set_size_download (app, total_size);

			gs_app_set_state (app, AS_APP_STATE_AVAILABLE);
		}

		break;
	}
	case EOS_UPDATER_STATE_ERROR: {
		const gchar *error_name;
		const gchar *error_message;
		g_autoptr(GError) local_error = NULL;

		error_name = eos_updater_get_error_name (priv->updater_proxy);
		error_message = eos_updater_get_error_message (priv->updater_proxy);
		local_error = g_dbus_error_new_for_dbus_error (error_name, error_message);

		/* unless the error is because the user cancelled the upgrade,
		 * we should make sure it get in the journal */
		if (!(os_upgrade_get_download_by_user (app) &&
		      eos_updater_error_is_cancelled (error_name)))
		    g_warning ("Got OS upgrade error state with name '%s': %s",
			       error_name, error_message);

		gs_app_set_state_recover (app);

		if ((g_strcmp0 (error_name, "com.endlessm.Updater.Error.LiveBoot") == 0) ||
		    (g_strcmp0 (error_name, "com.endlessm.Updater.Error.NotOstreeSystem") == 0)) {
			g_debug ("Disabling OS upgrades: %s", error_message);
			disable_os_updater (plugin);
			return state;
		}

		/* if we need to restart when an error occurred, just call poll
		 * since it will perform the full upgrade as the
		 * eos::DownloadByUser is true */
		if (os_upgrade_get_restart_on_error (app)) {
			g_debug ("Restarting OS upgrade on error");
			os_upgrade_set_restart_on_error (app, FALSE);
			app_ensure_installing_state (app);
			eos_updater_call_poll (priv->updater_proxy, NULL, NULL,
					       NULL);
			break;
		}

		/* only set up an error to be shown to the user if the user had
		 * manually started the upgrade, and if the error in question is not
		 * originated by the user canceling the upgrade */
		if (os_upgrade_get_download_by_user (app) &&
		    !eos_updater_error_is_cancelled (error_name)) {
			g_autoptr(GsPluginEvent) event = gs_plugin_event_new ();
			gs_utils_error_convert_gdbus (&local_error);
			gs_plugin_event_set_app (event, app);
			gs_plugin_event_set_error (event, local_error);
			gs_plugin_event_add_flag (event, GS_PLUGIN_EVENT_FLAG_WARNING);
			gs_plugin_report_event (plugin, event);
		}

		break;
	}
	case EOS_UPDATER_STATE_FETCHING: {
		guint64 total_size = 0;
		guint64 downloaded = 0;
		gfloat progress = 0;

		if (!updater_is_stalled (plugin))
			app_ensure_installing_state (app);
		else
			gs_app_set_state (app, AS_APP_STATE_AVAILABLE);

		downloaded = eos_updater_get_downloaded_bytes (priv->updater_proxy);
		total_size = eos_updater_get_download_size (priv->updater_proxy);

		if (total_size == 0) {
			g_debug ("OS upgrade %s total size is 0!",
				 gs_app_get_unique_id (app));
		} else {
			/* set progress only up to a max percentage, leaving the
			 * remaining for applying the update */
			progress = (gfloat) downloaded / (gfloat) total_size *
				   (gfloat) max_progress_for_update;
		}
		gs_app_set_progress (app, (guint) progress);

		break;
	}
	case EOS_UPDATER_STATE_UPDATE_READY: {
		/* if there's an update ready to deployed, and it was started by
		 * the user, we should proceed to applying the upgrade */
		if (os_upgrade_get_download_by_user (app)) {
			app_ensure_installing_state (app);
			gs_app_set_progress (app, max_progress_for_update);
			eos_updater_call_apply (priv->updater_proxy, NULL, NULL,
						NULL);
		} else {
			/* otherwise just show it as available so the user has a
			 * chance to click 'download' which deploys the update */
			gs_app_set_state (app, AS_APP_STATE_AVAILABLE);
		}

		break;
	}
	case EOS_UPDATER_STATE_APPLYING_UPDATE: {
		/* set as 'installing' because if it is applying the update, we
		 * want to show the progress bar */
		app_ensure_installing_state (app);

		/* set up the fake progress to inform the user that something
		 * is still being done (we don't get progress reports from
		 * deploying updates) */
		if (priv->upgrade_fake_progress_handler != 0)
			g_source_remove (priv->upgrade_fake_progress_handler);
		priv->upgrade_fake_progress = 0;
		priv->upgrade_fake_progress_handler =
			g_timeout_add ((guint) (1000.0 * EOS_UPGRADE_APPLY_STEP_TIME),
				       (GSourceFunc) fake_os_upgrade_progress,
				       plugin);

		break;
	}
	case EOS_UPDATER_STATE_UPDATE_APPLIED: {
		/* ensure we can transition to state updatable */
		if (gs_app_get_state (app) == AS_APP_STATE_AVAILABLE)
			gs_app_set_state (app, AS_APP_STATE_INSTALLING);
		gs_app_set_state (app, AS_APP_STATE_UPDATABLE);

		break;
	}
	default:
		break;
	}

	current_app_state = gs_app_get_state (app);

	/* reset the 'download-by-user' state if the the app is no longer
	 * shown as downloading */
	if (current_app_state != AS_APP_STATE_INSTALLING) {
		os_upgrade_set_download_by_user (app, FALSE);
	} else {
		/* otherwise, ensure we have the right cancellable */
		setup_os_upgrade_cancellable (plugin);
	}

	/* if the state changed from or to 'unknown', we need to notify that a
	 * new update should be shown */
	if ((previous_app_state == AS_APP_STATE_UNKNOWN ||
	     current_app_state == AS_APP_STATE_UNKNOWN) &&
	    previous_app_state != current_app_state)
		gs_plugin_updates_changed (plugin);

	return state;
}

gboolean
gs_plugin_setup (GsPlugin *plugin,
		 GCancellable *cancellable,
		 GError **error)
{
	GApplication *app = g_application_get_default ();
	GsPluginData *priv = gs_plugin_get_data (plugin);
	g_autofree char *image_version = NULL;

	priv->session_bus = g_application_get_dbus_connection (app);

	{
		g_autoptr(GError) local_error = NULL;
		if (!get_applications_with_shortcuts (plugin, &priv->desktop_apps,
						      cancellable, &local_error))
			g_warning ("Couldn't get the apps with shortcuts: %s",
				   local_error->message);
	}

	priv->applications_changed_id =
		g_dbus_connection_signal_subscribe (priv->session_bus,
						    "org.gnome.Shell",
						    "org.gnome.Shell.AppStore",
						    "ApplicationsChanged",
						    "/org/gnome/Shell",
						    NULL,
						    G_DBUS_SIGNAL_FLAGS_NONE,
						    (GDBusSignalCallback) on_desktop_apps_changed,
						    plugin, NULL);
	priv->soup_session = gs_plugin_get_soup_session (plugin);

	{
		g_autoptr(GError) local_error = NULL;
		priv->os_version_id = get_os_version_id (&local_error);
		if (!priv->os_version_id)
			g_warning ("No OS version ID could be set: %s",
				   local_error->message);
	}

	priv->replacement_app_lookup = g_hash_table_new_full (g_str_hash, g_str_equal,
							      g_free, g_free);

	priv->eos_arch_is_arm = g_strcmp0 (flatpak_get_default_arch (), "arm") == 0;

	image_version = get_image_version ();
	priv->personality = get_personality (image_version);
	priv->product_name = get_product_name (image_version);

	if (!priv->personality)
		g_warning ("No system personality could be retrieved!");

	g_assert (G_N_ELEMENTS (eos_updater_state_str) == EOS_UPDATER_N_STATES);

	/* Synchronous, but this guarantees that the lookup table will be
	 * there when we call ReplaceApplication later on */
	read_icon_replacement_overrides (priv->replacement_app_lookup);

	{
		g_autoptr(GError) local_error = NULL;
		priv->updater_proxy = eos_updater_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
									  G_DBUS_PROXY_FLAGS_NONE,
									  "com.endlessm.Updater",
									  "/com/endlessm/Updater",
									  NULL,
									  &local_error);
		if (priv->updater_proxy == NULL)
			g_warning ("Couldn't create EOS Updater proxy: %s",
				   local_error->message);
	}

	/* prepare EOS upgrade app + sync initial state */
	setup_os_upgrade (plugin);

	return TRUE;
}

void
gs_plugin_initialize (GsPlugin *plugin)
{
	gs_plugin_alloc_data (plugin, sizeof(GsPluginData));

	/* let the flatpak plugin run first so we deal with the apps
	 * in a more complete/refined state */
	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_AFTER, "flatpak");

	/* we already deal with apps that need to be proxied, so let's impede
	 * the other plugin from running */
	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_CONFLICTS, "generic-updates");
}

void
gs_plugin_destroy (GsPlugin *plugin)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);

	if (priv->applications_changed_id != 0) {
		g_dbus_connection_signal_unsubscribe (priv->session_bus,
						      priv->applications_changed_id);
		priv->applications_changed_id = 0;
	}

	g_hash_table_destroy (priv->desktop_apps);
	g_hash_table_destroy (priv->replacement_app_lookup);
	g_free (priv->personality);
	g_free (priv->product_name);
	g_free (priv->os_version_id);

	disable_os_updater (plugin);
	g_clear_object (&priv->os_upgrade);
}

static gboolean
app_is_renamed (GsApp *app)
{
	/* Apps renamed by eos-desktop get the desktop attribute of
	 * X-Endless-CreatedBy assigned to the desktop's name;
	 * Starting with EOS 3.2 apps can no longer be renamed so
	 * we keep it for legacy reasons */
	return g_strcmp0 (gs_app_get_metadata_item (app, "X-Endless-CreatedBy"),
			  "eos-desktop") == 0;
}

static gboolean
gs_plugin_locale_is_compatible (GsPlugin *plugin,
				const char *locale)
{
	g_auto(GStrv) locale_variants;
	const char *plugin_locale = gs_plugin_get_locale (plugin);
	int idx;

	locale_variants = g_get_locale_variants (plugin_locale);
	for (idx = 0; locale_variants[idx] != NULL; idx++) {
		if (g_strcmp0 (locale_variants[idx], locale) == 0)
			return TRUE;
	}

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
	return g_str_has_suffix (gs_flatpak_app_get_ref_name (app),
				 gs_plugin_get_locale (plugin));
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

	/* avoid blacklisting the same app that's already cached */
	if (is_same_app (cached_app, app))
		return;

	if (cached_app && !gs_app_is_installed (cached_app) &&
	    !gs_app_has_category (cached_app, "USB")) {
		const char *app_id = gs_app_get_unique_id (app);
		const char *cached_app_id = gs_app_get_unique_id (cached_app);

		g_debug ("Blacklisting '%s': using '%s' due to its locale",
			 cached_app_id, app_id);
		gs_app_add_category (cached_app, "Blacklisted");
	}

	gs_plugin_cache_add (plugin, locale_cache_key, app);
}

static gboolean
gs_plugin_eos_blacklist_kapp_if_needed (GsPlugin *plugin, GsApp *app)
{
	guint endless_prefix_len = strlen (ENDLESS_ID_PREFIX);
	g_autofree char *locale_cache_key = NULL;
	g_auto(GStrv) tokens = NULL;
	const char *last_token = NULL;
	guint num_tokens = 0;
	/* getting the app name, besides skipping the '.desktop' part of the id
	 * also makes sure we're dealing with a Flatpak app */
	const char *app_name = gs_flatpak_app_get_ref_name (app);
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
	    !gs_app_has_category (app, "USB")) {
		if (gs_app_is_installed (app))
			return FALSE;

		g_debug ("Blacklisting '%s': incompatible with the current "
			 "locale", gs_app_get_unique_id (app));
		gs_app_add_category (app, "Blacklisted");

		return TRUE;
	}

	locale_cache_key = get_app_locale_cache_key (app_name);
	cached_app = gs_plugin_cache_lookup (plugin, locale_cache_key);

	if (is_same_app (cached_app, app))
		return FALSE;

	/* skip if the cached app is already our best */
	if (cached_app &&
	    gs_plugin_app_is_locale_best_match (plugin, cached_app) &&
	    !gs_app_has_category (cached_app, "USB")) {
		if (!gs_app_is_installed (app)) {
			g_debug ("Blacklisting '%s': cached app '%s' is best "
				 "match", gs_app_get_unique_id (app),
				 gs_app_get_unique_id (cached_app));
			gs_app_add_category (app, "Blacklisted");
		}

		return TRUE;
	}

	gs_plugin_update_locale_cache_app (plugin, locale_cache_key, app);
	return FALSE;
}

static gboolean
gs_plugin_eos_blacklist_app_for_remote_if_needed (GsPlugin *plugin,
						  GsApp *app)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	gboolean do_blacklist = FALSE;

	static const char *duplicated_apps[] = {
		"com.google.Chrome",
		"com.sparklinlabs.Superpowers",
		"com.stencyl.Game",
		"de.billardgl.Billardgl",
		"net.sourceforge.Frostwire",
		"org.eclipse.Eclipse",
		"org.learningequality.KALite",
		"org.mozilla.Firefox",
		"org.platformio.Ide",
		"org.snap4arduino.App",
		"org.squeakland.Etoys",
		"org.squeakland.Scratch",
		NULL
	};

	static const char *core_apps[] = {
		"org.gnome.Calculator",
		"org.gnome.Contacts",
		"org.gnome.Evince",
		"org.gnome.Nautilus",
		"org.gnome.Rhythmbox3",
		"org.gnome.Totem",
		"org.gnome.clocks",
		"org.gnome.eog",
		"org.gnome.gedit",
		NULL
	};

	/* Flatpak apps known not to be working properly */
	static const char *buggy_apps[] = {
		/* Missing lots of keys and defaults specified in eos-theme */
		"ca.desrt.dconf-editor",
		/* Requires kdeconnect on the host, which is not supported on Endless */
		"com.github.bajoja.indicator-kdeconnect",
		NULL
	};

	/* List of apps that are proven to work on ARM */
	static const char *arm_whitelist[] = {
		"cc.arduino.arduinoide",
		"ch.x29a.playitslowly",
		"com.abisource.AbiWord",
		"com.bixense.PasswordCalculator",
		"com.chez.GrafX2",
		"com.dosbox.DOSBox",
		"com.endlessm.photos",
		"com.frac_tion.teleport",
		"com.github.JannikHv.Gydl",
		"com.github.alecaddd.sequeler",
		"com.github.babluboy.bookworm",
		"com.github.bilelmoussaoui.Authenticator",
		"com.github.birros.WebArchives",
		"com.github.bitseater.weather",
		"com.github.bleakgrey.tootle",
		"com.github.cassidyjames.dippi",
		"com.github.dahenson.agenda",
		"com.github.danrabbit.harvey",
		"com.github.donadigo.appeditor",
		"com.github.eudaldgr.elements",
		"com.github.fabiocolacio.marker",
		"com.github.geigi.cozy",
		"com.github.gijsgoudzwaard.image-optimizer",
		"com.github.gkarsay.parlatype",
		"com.github.gyunaev.spivak",
		"com.github.hluk.copyq",
		"com.github.labyrinth_team.labyrinth",
		"com.github.lainsce.coin",
		"com.github.lainsce.notejot",
		"com.github.lainsce.yishu",
		"com.github.libresprite.LibreSprite",
		"com.github.mdh34.hackup",
		"com.github.mdh34.quickdocs",
		"com.github.miguelmota.Cointop",
		"com.github.muriloventuroso.easyssh",
		"com.github.needleandthread.vocal",
		"com.github.ojubaorg.Othman",
		"com.github.paolostivanin.OTPClient",
		"com.github.philip_scott.notes-up",
		"com.github.philip_scott.spice-up",
		"com.github.quaternion",
		"com.github.robertsanseries.ciano",
		"com.github.rssguard",
		"com.github.ryanakca.slingshot",
		"com.github.themix_project.Oomox",
		"com.github.unrud.RemoteTouchpad",
		"com.github.utsushi.Utsushi",
		"com.github.wwmm.pulseeffects",
		"com.inventwithpython.flippy",
		"com.katawa_shoujo.KatawaShoujo",
		"com.moonlight_stream.Moonlight",
		"com.ozmartians.VidCutter",
		"com.szibele.e-juice-calc",
		"com.transmissionbt.Transmission",
		"com.tux4kids.tuxmath",
		"com.tux4kids.tuxtype",
		"com.uploadedlobster.peek",
		"com.visualstudio.code.oss",
		"cx.ring.Ring",
		"de.haeckerfelix.Fragments",
		"de.haeckerfelix.gradio",
		"de.manuel_kehl.go-for-it",
		"de.wolfvollprecht.UberWriter",
		"eu.scarpetta.PDFMixTool",
		"fr.free.Homebank",
		"id.sideka.App",
		"im.srain.Srain",
		"io.elementary.code",
		"io.github.Cockatrice.cockatrice",
		"io.github.Hexchat",
		"io.github.Pithos",
		"io.github.cges30901.hmtimer",
		"io.github.cloose.CuteMarkEd",
		"io.github.gillesdegottex.FMIT",
		"io.github.jkozera.ZevDocs",
		"io.github.jliljebl.Flowblade",
		"io.github.markummitchell.Engauge_Digitizer",
		"io.github.martinrotter.textosaurus",
		"io.github.mmstick.FontFinder",
		"io.github.mujx.Nheko",
		"io.github.qtox.qTox",
		"io.github.quodlibet.QuodLibet",
		"io.github.wereturtle.ghostwriter",
		"io.gitlab.construo.construo",
		"io.gitlab.evtest_qt.evtest_qt",
		"io.gitlab.jstest_gtk.jstest_gtk",
		"io.thp.numptyphysics",
		"me.kozec.syncthingtk",
		"net.ankiweb.Anki",
		"net.bartkessels.getit",
		"net.mediaarea.AVIMetaEdit",
		"net.mediaarea.BWFMetaEdit",
		"net.mediaarea.DVAnalyzer",
		"net.mediaarea.MOVMetaEdit",
		"net.mediaarea.MediaConch",
		"net.mediaarea.MediaInfo",
		"net.mediaarea.QCTools",
		"net.olofson.KoboDeluxe",
		"net.oz9aec.Gpredict",
		"net.scribus.Scribus",
		"net.sf.VICE",
		"net.sf.fuse_emulator",
		"net.sf.nootka",
		"net.sourceforge.Chessx",
		"net.sourceforge.Fillets",
		"net.sourceforge.Klavaro",
		"net.sourceforge.Ri-li",
		"net.sourceforge.Teo",
		"net.sourceforge.TuxFootball",
		"net.sourceforge.atanks",
		"net.sourceforge.xournal",
		"nl.openoffice.bluefish",
		"org.baedert.corebird",
		"org.blender.Blender",
		"org.bunkus.mkvtoolnix-gui",
		"org.codeblocks.codeblocks",
		"org.debian.TuxPuck",
		"org.equeim.Tremotesf",
		"org.filezillaproject.Filezilla",
		"org.flatpak.Builder",
		"org.flatpak.qtdemo",
		"org.freeciv.Freeciv",
		"org.freedesktop.GstDebugViewer",
		"org.freefilesync.FreeFileSync",
		"org.fritzing.Fritzing",
		"org.frozen_bubble.frozen-bubble",
		"org.gabmus.hydrapaper",
		"org.gahshomar.Gahshomar",
		"org.geany.Geany",
		"org.gimp.GIMP",
		"org.gna.Warmux",
		"org.gnome.Aisleriot",
		"org.gnome.Books",
		"org.gnome.Boxes",
		"org.gnome.Builder",
		"org.gnome.Calendar",
		"org.gnome.Characters",
		"org.gnome.Devhelp",
		"org.gnome.Dictionary",
		"org.gnome.Fractal",
		"org.gnome.Geary",
		"org.gnome.Genius",
		"org.gnome.Glade",
		"org.gnome.Gnote",
		"org.gnome.Gtranslator",
		"org.gnome.Hitori",
		"org.gnome.Keysign",
		"org.gnome.Lollypop",
		"org.gnome.Maps",
		"org.gnome.Music",
		"org.gnome.OfficeRunner",
		"org.gnome.Photos",
		"org.gnome.Podcasts",
		"org.gnome.Polari",
		"org.gnome.Recipes",
		"org.gnome.Todo",
		"org.gnome.Weather",
		"org.gnome.bijiben",
		"org.gnome.chess",
		"org.gnome.dfeet",
		"org.gnome.frogr",
		"org.gnome.gbrainy",
		"org.gnome.ghex",
		"org.gnome.gitg",
		"org.gnome.glabels-3",
		"org.gnome.iagno",
		"org.gnome.meld",
		"org.gnome.quadrapassel",
		"org.gnome.tetravex",
		"org.gnucash.GnuCash",
		"org.gottcode.Connectagram",
		"org.gottcode.CuteMaze",
		"org.gottcode.FocusWriter",
		"org.gottcode.Gottet",
		"org.gottcode.Hexalate",
		"org.gottcode.Kapow",
		"org.gottcode.NovProg",
		"org.gottcode.Peg-E",
		"org.gottcode.Simsu",
		"org.gottcode.Tanglet",
		"org.gottcode.Tetzle",
		"org.gpodder.gpodder",
		"org.inkscape.Inkscape",
		"org.jamovi.jamovi",
		"org.kde.gcompris",
		"org.kde.kapman",
		"org.kde.katomic",
		"org.kde.kblocks",
		"org.kde.kbounce",
		"org.kde.kbruch",
		"org.kde.kdiamond",
		"org.kde.kgeography",
		"org.kde.kgoldrunner",
		"org.kde.khangman",
		"org.kde.kigo",
		"org.kde.killbots",
		"org.kde.kjumpingcube",
		"org.kde.klickety",
		"org.kde.klines",
		"org.kde.knavalbattle",
		"org.kde.knetwalk",
		"org.kde.kolourpaint",
		"org.kde.ksquares",
		"org.kde.ksudoku",
		"org.kde.ktuberling",
		"org.kde.kwordquiz",
		"org.kde.okular",
		"org.kde.palapeli",
		"org.keepassxc.KeePassXC",
		"org.kicad_pcb.KiCad",
		"org.laptop.TurtleArtActivity",
		"org.libreoffice.LibreOffice",
		"org.mapeditor.Tiled",
		"org.musescore.MuseScore",
		"org.musicbrainz.Picard",
		"org.mypaint.MyPaint",
		"org.nextcloud.Nextcloud",
		"org.openshot.OpenShot",
		"org.openttd.OpenTTD",
		"org.pencil2d.Pencil2D",
		"org.pitivi.Pitivi",
		"org.processing.processingide",
		"org.pyzo.pyzo",
		"org.qbittorrent.qBittorrent",
		"org.qgis.qgis",
		"org.qownnotes.QOwnNotes",
		"org.quassel_irc.QuasselClient",
		"org.remmina.Remmina",
		"org.seul.pingus",
		"org.shotcut.Shotcut",
		"org.supertux.SuperTux-Milestone1",
		"org.synfig.SynfigStudio",
		"org.telegram.desktop",
		"org.tordini.flavio.Minitube",
		"org.tuxpaint.Tuxpaint",
		"org.vim.Vim",
		"org.wesnoth.Wesnoth",
		"org.xiphos.Xiphos",
		"space.fips.Fips",
		"uk.co.mangobrain.Infector",
		"work.openpaper.Paperwork",
		"xyz.z3ntu.razergenie",
		NULL
	};

	/* Legacy apps that have been replaced by other versions in Flathub */
	static const char *legacy_apps[] = {
		"com.spotify.Client",
		"org.videolan.VLC",
		NULL
	};

	const char *hostname = NULL;
	const char *app_name = NULL;

	if (gs_app_get_scope (app) != AS_APP_SCOPE_SYSTEM ||
	    gs_app_is_installed (app))
		return FALSE;

	hostname = gs_app_get_origin_hostname (app);
	if (hostname == NULL)
		return FALSE;

	app_name = gs_flatpak_app_get_ref_name (app);
	if (app_name == NULL)
		return FALSE;

	/* We need to check for the app's origin, otherwise we'd be
	 * blacklisting matching apps coming from any repo */
	if (g_str_has_suffix (hostname, ".endlessm.com")) {
		if (g_strv_contains (legacy_apps, app_name)) {
			g_debug ("Blacklisting '%s': it's a legacy app",
				 gs_app_get_unique_id (app));
			do_blacklist = TRUE;
		}
	} else if (g_strcmp0 (hostname, "sdk.gnome.org") == 0 ||
		   g_strcmp0 (hostname, "flathub.org") == 0 ||
		   g_str_has_suffix (hostname, ".flathub.org")) {

		/* If the arch is ARM then we simply use a whitelist and
		 * don't go through all the remaining lists */
		if (priv->eos_arch_is_arm) {
			if (g_strv_contains (arm_whitelist, app_name))
				return FALSE;
			g_debug ("Blacklisting '%s': it's not whitelisted for ARM",
				 gs_app_get_unique_id (app));
			do_blacklist = TRUE;
		} else if (g_strv_contains (duplicated_apps, app_name)) {
			g_debug ("Blacklisting '%s': app is in the duplicated list",
				 gs_app_get_unique_id (app));
			do_blacklist = TRUE;
		} else if (g_strv_contains (core_apps, app_name)) {
			g_debug ("Blacklisting '%s': app is in the core apps list",
				 gs_app_get_unique_id (app));
			do_blacklist = TRUE;
		} else if (g_strv_contains (buggy_apps, app_name)) {
			g_debug ("Blacklisting '%s': app is in the buggy list",
				 gs_app_get_unique_id (app));
			do_blacklist = TRUE;
		}
	}

	if (do_blacklist)
		gs_app_add_category (app, "Blacklisted");

	return do_blacklist;
}

static void
gs_plugin_eos_remove_blacklist_from_usb_if_needed (GsPlugin *plugin, GsApp *app)
{
	if (!gs_app_has_category (app, "Blacklisted") ||
	    !gs_app_has_category (app, "USB"))
		return;

	g_debug ("Removing blacklisting from '%s': app is from USB", gs_app_get_unique_id (app));
	gs_app_remove_category (app, "Blacklisted");
}

static gboolean
app_is_banned_for_personality (GsPlugin *plugin, GsApp *app)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	const char *app_name = gs_flatpak_app_get_ref_name (app);

	static const char *violent_apps[] = {
		"com.grangerhub.Tremulous",
		"com.moddb.TotalChaos",
		"com.realm667.WolfenDoom_Blade_of_Agony",
		"io.github.FreeDM",
		"io.github.Freedoom-Phase-1",
		"io.github.Freedoom-Phase-2",
		"net.redeclipse.RedEclipse",
		"org.sauerbraten.Sauerbraten",
		"org.xonotic.Xonotic",
		"ws.openarena.OpenArena",
		NULL
	};

	static const char *google_apps[] = {
		"com.google.Chrome",
		"com.endlessm.translation",
		"com.github.JannikHv.Gydl",
		"org.tordini.flavio.Minitube",
		NULL
	};

	/* do not ban apps based on personality if they are installed or
	 * if they don't have a ref name (i.e. are not Flatpak apps) */
	if (gs_app_is_installed (app) || app_name == NULL)
		return FALSE;

	return ((g_strcmp0 (priv->personality, "es_GT") == 0) &&
	        g_strv_contains (violent_apps, app_name)) ||
	       ((g_strcmp0 (priv->personality, "zh_CN") == 0) &&
	        (g_strv_contains (google_apps, app_name) ||
	         g_str_has_prefix (app_name, "com.endlessm.encyclopedia"))) ||
	       (g_str_has_prefix (priv->personality, "spark") &&
	        g_strv_contains (violent_apps, app_name));
}

static gboolean
app_is_compatible_with_os (GsPlugin *plugin, GsApp *app)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	const char *app_available_since;

	if (!priv->os_version_id)
		return TRUE;

	app_available_since =
		gs_app_get_metadata_item (app, "EndlessOS::available-since");
	if (!app_available_since)
		return TRUE;

	/* if the OS version is greater than or equal to the app
	 * "available-since" metadata item, it means it is compatible */
	return as_utils_vercmp (priv->os_version_id, app_available_since) >= 0;
}

static gboolean
app_is_evergreen (GsApp *app)
{
	const char *id = gs_app_get_id (app);

	return g_str_has_prefix (id, "com.endlessm.quote_of_the_day") ||
		g_str_has_prefix (id, "com.endlessm.word_of_the_day");
}

/* Convert an #EpcAppFilterOarsValue to an #AsContentRatingValue. This is
 * actually a trivial cast, since the types are defined the same; but throw in
 * a static assertion to be sure. */
static AsContentRatingValue
convert_app_filter_oars_value (EpcAppFilterOarsValue filter_value)
{
  G_STATIC_ASSERT (AS_CONTENT_RATING_VALUE_LAST == EPC_APP_FILTER_OARS_VALUE_INTENSE + 1);

  return (AsContentRatingValue) filter_value;
}

/* Check whether the OARS rating for @app is as, or less, extreme than the
 * user’s preferences in @app_filter. If so (i.e. if the app is suitable for
 * this user to use), return %TRUE; otherwise return %FALSE.
 *
 * The #AsContentRating in @app may be %NULL if no OARS ratings are provided for
 * the app. If so, we have to assume the most restrictive ratings.
 *
 * We don’t need to worry about updating the app list when the app filter value
 * is changed, as changing it requires logging out and back in as an
 * administrator. */
static gboolean
app_is_content_rating_appropriate (GsApp *app, EpcAppFilter *app_filter)
{
	AsContentRating *rating = gs_app_get_content_rating (app);  /* (nullable) */
	g_autofree const gchar **oars_sections = epc_app_filter_get_oars_sections (app_filter);

	if (rating == NULL)
		g_debug ("No OARS ratings provided for ‘%s’: assuming most extreme",
		         gs_app_get_unique_id (app));

	for (gsize i = 0; oars_sections[i] != NULL; i++) {
		AsContentRatingValue rating_value;
		EpcAppFilterOarsValue filter_value;

		filter_value = epc_app_filter_get_oars_value (app_filter, oars_sections[i]);

		if (rating != NULL)
			rating_value = as_content_rating_get_value (rating, oars_sections[i]);
		else
			rating_value = AS_CONTENT_RATING_VALUE_INTENSE;

		if (rating_value == AS_CONTENT_RATING_VALUE_UNKNOWN ||
		    filter_value == EPC_APP_FILTER_OARS_VALUE_UNKNOWN)
			continue;
		else if (convert_app_filter_oars_value (filter_value) < rating_value)
			return FALSE;
	}

	return TRUE;
}

static gboolean
app_is_parentally_blacklisted (GsApp *app, EpcAppFilter *app_filter)
{
	const gchar *desktop_id;
	g_autoptr(GAppInfo) appinfo = NULL;

	desktop_id = gs_app_get_id (app);
	if (desktop_id == NULL)
		return FALSE;
	appinfo = G_APP_INFO (gs_utils_get_desktop_app_info (desktop_id));
	if (appinfo == NULL)
		return FALSE;

	return !epc_app_filter_is_appinfo_allowed (app_filter, appinfo);
}

static gboolean
gs_plugin_eos_parental_filter_if_needed (GsPlugin *plugin, GsApp *app, EpcAppFilter *app_filter)
{
	gboolean filtered = FALSE;

	/* Check the OARS ratings to see if this app should be installable. */
	if (!app_is_content_rating_appropriate (app, app_filter)) {
		g_debug ("Filtering ‘%s’: app OARS rating is too extreme for this user",
		         gs_app_get_unique_id (app));
		gs_app_add_quirk (app, GS_APP_QUIRK_PARENTAL_FILTER);
		filtered = TRUE;
	}

	/* Check the app blacklist to see if this app should be launchable. */
	if (app_is_parentally_blacklisted (app, app_filter)) {
		g_debug ("Filtering ‘%s’: app is blacklisted for this user",
		         gs_app_get_unique_id (app));
		gs_app_add_quirk (app, GS_APP_QUIRK_PARENTAL_NOT_LAUNCHABLE);
		filtered = TRUE;
	}

	return filtered;
}

static gboolean
gs_plugin_eos_blacklist_if_needed (GsPlugin *plugin, GsApp *app)
{
	gboolean blacklist_app = FALSE;
	const char *id = gs_app_get_id (app);

	if (gs_app_get_kind (app) != AS_APP_KIND_DESKTOP &&
	    gs_app_has_quirk (app, GS_APP_QUIRK_COMPULSORY) &&
	    !gs_app_has_quirk (app, GS_APP_QUIRK_IS_PROXY)) {
		g_debug ("Blacklisting '%s': it's a compulsory, non-desktop app",
			 gs_app_get_unique_id (app));
		blacklist_app = TRUE;
	} else if (g_str_has_prefix (id, "eos-link-")) {
		g_debug ("Blacklisting '%s': app is an eos-link",
			 gs_app_get_unique_id (app));
		blacklist_app = TRUE;
	} else if (gs_app_has_quirk (app, GS_APP_QUIRK_COMPULSORY) &&
		   g_strcmp0 (id, "org.gnome.Software.desktop") == 0) {
		g_debug ("Blacklisting '%s': app is GNOME Software itself",
			 gs_app_get_unique_id (app));
		blacklist_app = TRUE;
	} else if (app_is_renamed (app)) {
		g_debug ("Blacklisting '%s': app is renamed",
			 gs_app_get_unique_id (app));
		blacklist_app = TRUE;
	} else if (app_is_banned_for_personality (plugin, app)) {
		g_debug ("Blacklisting '%s': app is banned for personality",
			 gs_app_get_unique_id (app));
		blacklist_app = TRUE;
	} else if (app_is_evergreen (app)) {
		g_debug ("Blacklisting '%s': it's an evergreen app",
			 gs_app_get_unique_id (app));
		blacklist_app = TRUE;
	} else if (!gs_app_is_installed (app) &&
		   !app_is_compatible_with_os (plugin, app)) {
		g_debug ("Blacklisting '%s': it's incompatible with the OS "
			 "version", gs_app_get_unique_id (app));
		blacklist_app = TRUE;
	}

	if (blacklist_app)
		gs_app_add_category (app, "Blacklisted");

	return blacklist_app;
}

static char *
get_desktop_file_id (GsApp *app)
{
	const char *desktop_file_id =
		gs_app_get_metadata_item (app, METADATA_SYS_DESKTOP_FILE);

	if (!desktop_file_id) {
		if (app_is_flatpak (app)) {
			/* ensure we add the .desktop suffix to the app's ref name
			 * since in Flatpak the app ID can have that suffix already
			 * or not, depending on how the appdata has been generated */
			const char *ref_name = gs_flatpak_app_get_ref_name (app);
			return g_strconcat (ref_name, ".desktop", NULL);
		}

		/* just use the app ID if this is not a Flatpak app */
		desktop_file_id = gs_app_get_id (app);
	}

	g_assert (desktop_file_id != NULL);
	return g_strdup (desktop_file_id);
}

static void
gs_plugin_eos_update_app_shortcuts_info (GsPlugin *plugin,
					 GsApp *app)
{
	GsPluginData *priv = NULL;
	g_autofree char *desktop_file_id = NULL;
	g_autofree char *kde_desktop_file_id = NULL;

	if (!gs_app_is_installed (app)) {
		gs_app_remove_quirk (app, GS_APP_QUIRK_HAS_SHORTCUT);
		return;
	}

	priv = gs_plugin_get_data (plugin);
	desktop_file_id = get_desktop_file_id (app);
	kde_desktop_file_id =
		g_strdup_printf ("%s-%s", "kde4", desktop_file_id);

	/* Cache both keys, since we may see either variant in the desktop
	 * grid; see on_desktop_apps_changed().
	 */
	gs_plugin_cache_add (plugin, desktop_file_id, app);
	gs_plugin_cache_add (plugin, kde_desktop_file_id, app);

	if (g_hash_table_lookup (priv->desktop_apps, desktop_file_id) ||
	    g_hash_table_lookup (priv->desktop_apps, kde_desktop_file_id))
		gs_app_add_quirk (app, GS_APP_QUIRK_HAS_SHORTCUT);
	else
		gs_app_remove_quirk (app, GS_APP_QUIRK_HAS_SHORTCUT);
}

static gboolean
app_is_flatpak (GsApp *app)
{
	return gs_app_get_bundle_kind (app) == AS_BUNDLE_KIND_FLATPAK;
}

void
gs_plugin_adopt_app (GsPlugin *plugin, GsApp *app)
{
	if (app_is_flatpak (app))
		return;

	gs_app_set_management_plugin (app, gs_plugin_get_name (plugin));
}

static void
gs_plugin_eos_refine_core_app (GsApp *app)
{
	if (app_is_flatpak (app) ||
	    (gs_app_get_scope (app) == AS_APP_SCOPE_UNKNOWN))
		return;

	if (gs_app_get_kind (app) == AS_APP_KIND_OS_UPGRADE)
		return;

	/* blacklist the KDE desktop file of the GNOME System Monitor since
	 * it's a core app but should not be shown */
	if (g_strcmp0 (gs_app_get_id (app), "gnome-system-monitor-kde.desktop") == 0) {
		g_debug ("Blacklisting %s because it will show as a duplicate "
			 "of the real gnome-system-monitor one.",
			 gs_app_get_unique_id (app));
		gs_app_add_category (app, "Blacklisted");
		return;
	}

	/* we only allow to remove flatpak apps */
	gs_app_add_quirk (app, GS_APP_QUIRK_COMPULSORY);

	if (!gs_app_is_installed (app)) {
		/* forcibly set the installed state */
		gs_app_set_state (app, AS_APP_STATE_UNKNOWN);
		gs_app_set_state (app, AS_APP_STATE_INSTALLED);
	}
}

typedef struct
{
	GsApp *app;
	GsPlugin *plugin;
	char *cache_filename;
} PopularBackgroundRequestData;

static void
popular_background_image_tile_request_data_destroy (PopularBackgroundRequestData *data)
{
	g_free (data->cache_filename);
	g_free (data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(PopularBackgroundRequestData,
                              popular_background_image_tile_request_data_destroy)

static void
gs_plugin_eos_update_tile_image_from_filename (GsApp      *app,
                                               const char *filename)
{
	g_autofree char *css = g_strdup_printf ("background-image: url('%s')",
	                                       filename);
	gs_app_set_metadata (app, "GnomeSoftware::BackgroundTile-css", css);
}

static void
gs_plugin_eos_tile_image_downloaded_cb (SoupSession *session,
                                        SoupMessage *msg,
                                        gpointer user_data)
{
	g_autoptr(PopularBackgroundRequestData) data = user_data;
	g_autoptr(GError) error = NULL;

	if (msg->status_code == SOUP_STATUS_CANCELLED)
		return;

	if (msg->status_code != SOUP_STATUS_OK) {
		g_debug ("Failed to download tile image corresponding to cache entry %s: %s",
		         data->cache_filename,
		         msg->reason_phrase);
		return;
	}

	/* Write out the cache image to disk */
	if (!g_file_set_contents (data->cache_filename,
	                          msg->response_body->data,
	                          msg->response_body->length,
	                          &error)) {
		g_debug ("Failed to write cache image %s, %s",
		         data->cache_filename,
		         error->message);
		return;
	}

	gs_plugin_eos_update_tile_image_from_filename (data->app, data->cache_filename);
}

static void
gs_plugin_eos_refine_popular_app (GsPlugin *plugin,
				  GsApp *app)
{
	const char *popular_bg = NULL;
	g_autofree char *tile_cache_hash = NULL;
	g_autofree char *cache_filename = NULL;
	g_autofree char *writable_cache_filename = NULL;
	g_autofree char *url_basename = NULL;
	g_autofree char *cache_identifier = NULL;
	GsPluginData *priv = gs_plugin_get_data (plugin);
	PopularBackgroundRequestData *request_data = NULL;
	g_autoptr(SoupURI) soup_uri = NULL;
	g_autoptr(SoupMessage) message = NULL;

	if (gs_app_get_metadata_item (app, "GnomeSoftware::BackgroundTile-css"))
		return;

	popular_bg =
	   gs_app_get_metadata_item (app, "GnomeSoftware::popular-background");

	if (!popular_bg)
		return;

	url_basename = g_path_get_basename (popular_bg);

	/* First take a hash of this URL and see if it is in our cache */
	tile_cache_hash = g_compute_checksum_for_string (G_CHECKSUM_SHA256,
	                                                 popular_bg,
	                                                 -1);
	cache_identifier = g_strdup_printf ("%s-%s", tile_cache_hash, url_basename);
	cache_filename = gs_utils_get_cache_filename ("eos-popular-app-thumbnails",
	                                              cache_identifier,
	                                              GS_UTILS_CACHE_FLAG_NONE,
	                                              NULL);

	/* Check to see if the file exists in the cache at the time we called this
	 * function. If it does, then change the css so that the tile loads. Otherwise,
	 * we'll need to asynchronously fetch the image from the server and write it
	 * to the cache */
	if (g_file_test (cache_filename, G_FILE_TEST_EXISTS)) {
		g_debug ("Hit cache for thumbnail %s: %s", popular_bg, cache_filename);
		gs_plugin_eos_update_tile_image_from_filename (app, cache_filename);
		return;
	}

	writable_cache_filename = gs_utils_get_cache_filename ("eos-popular-app-thumbnails",
	                                                       cache_identifier,
	                                                       GS_UTILS_CACHE_FLAG_WRITEABLE,
	                                                       NULL);

	soup_uri = soup_uri_new (popular_bg);
	g_debug ("Downloading thumbnail %s to %s", popular_bg, writable_cache_filename);
	if (!soup_uri || !SOUP_URI_VALID_FOR_HTTP (soup_uri)) {
		g_debug ("Couldn't download %s, URL is not valid", popular_bg);
		return;
	}

	/* XXX: Note that we might have multiple downloads in progress here. We
	 * don't make any attempt to keep track of this. */
	message = soup_message_new_from_uri (SOUP_METHOD_GET, soup_uri);
	if (!message) {
		g_debug ("Couldn't download %s, network not available", popular_bg);
		return;
	}

	request_data = g_new0 (PopularBackgroundRequestData, 1);
	request_data->app = app;
	request_data->plugin = plugin;
	request_data->cache_filename = g_steal_pointer (&writable_cache_filename);

	soup_session_queue_message (priv->soup_session,
	                            g_steal_pointer (&message),
	                            gs_plugin_eos_tile_image_downloaded_cb,
	                            request_data);
}

static void
gs_plugin_refine_proxy_app (GsPlugin	*plugin,
			    GsApp	*app)
{
	g_autoptr(GsAppList) related_filtered = gs_app_list_new ();
	GPtrArray *proxied_apps = gs_app_get_related (app);

	for (guint i = 0; i < proxied_apps->len; ++i) {
		GsApp *proxied_app = g_ptr_array_index (proxied_apps, i);

		if (gs_app_get_state (proxied_app) == AS_APP_STATE_AVAILABLE ||
		    gs_app_is_updatable (proxied_app)) {
			g_debug ("App %s has an update or needs to be installed/updated "
				 "by the proxy app %s",
				 gs_app_get_unique_id (proxied_app),
				 gs_app_get_unique_id (app));
			gs_app_list_add (related_filtered, proxied_app);
		}
	}

	gs_app_clear_related (app);

	for (guint i = 0; i < gs_app_list_length (related_filtered); ++i) {
		GsApp *related_app = gs_app_list_index (related_filtered, i);
		gs_app_add_related (app, related_app);
	}

	/* mark the state as unknown so 1) we're always allowed to change the state below
	 * if needed; and 2) the app will not be shown at all (unless the state is changed
	 * below), thus avoiding eventually showing a proxy app without updates */
	gs_app_set_state (app, AS_APP_STATE_UNKNOWN);

	/* only let the proxy app show in the updates list if it has anything to update */
	if (gs_app_list_length (related_filtered) > 0)
		gs_app_set_state (app, AS_APP_STATE_UPDATABLE_LIVE);
}

gboolean
gs_plugin_refine (GsPlugin		*plugin,
		  GsAppList		*list,
		  GsPluginRefineFlags	flags,
		  GCancellable		*cancellable,
		  GError		**error)
{
	/* Query the app filter once, rather than one per app. */
	g_autoptr(EpcAppFilter) app_filter = NULL;
	app_filter = epc_get_app_filter (NULL, getuid (), TRUE, cancellable, error);
	if (app_filter == NULL)
		return FALSE;

	/* Refine each app. */
	for (guint i = 0; i < gs_app_list_length (list); ++i) {
		GsApp *app = gs_app_list_index (list, i);

		if (g_str_has_prefix (gs_app_get_id (app), EOS_PROXY_APP_PREFIX)) {
			gs_plugin_refine_proxy_app (plugin, app);
			continue;
		}

		gs_plugin_eos_refine_core_app (app);

		/* if we don't know yet the state of an app then we shouldn't
		 * do any further operations on it */
		if (gs_app_get_state (app) == AS_APP_STATE_UNKNOWN)
			continue;

		if (gs_plugin_eos_blacklist_if_needed (plugin, app))
			continue;

		if (gs_app_get_kind (app) != AS_APP_KIND_DESKTOP)
			continue;

		if (gs_plugin_eos_parental_filter_if_needed (plugin, app, app_filter))
			continue;

		gs_plugin_eos_update_app_shortcuts_info (plugin, app);

		if (gs_plugin_eos_blacklist_kapp_if_needed (plugin, app))
			continue;

		if (gs_plugin_eos_blacklist_app_for_remote_if_needed (plugin, app))
			continue;

		gs_plugin_eos_remove_blacklist_from_usb_if_needed (plugin, app);

		gs_plugin_eos_refine_popular_app (plugin, app);
	}

	return TRUE;
}

static gboolean
remove_app_from_shell (GsPlugin		*plugin,
		       GsApp		*app,
		       GCancellable	*cancellable,
		       GError		**error_out)
{
	GError *error = NULL;
	GsPluginData *priv = gs_plugin_get_data (plugin);
	g_autofree char *desktop_file_id = get_desktop_file_id (app);

	g_dbus_connection_call_sync (priv->session_bus,
				     "org.gnome.Shell",
				     "/org/gnome/Shell",
				     "org.gnome.Shell.AppStore",
				     "RemoveApplication",
				     g_variant_new ("(s)", desktop_file_id),
				     NULL,
				     G_DBUS_CALL_FLAGS_NONE,
				     -1,
				     cancellable,
				     &error);

	if (error != NULL) {
		g_debug ("Error removing app from shell: %s", error->message);
		g_propagate_error (error_out, error);
		return FALSE;
	}

	return TRUE;
}

static GVariant *
shell_add_app_if_not_visible (GDBusConnection *session_bus,
			      const gchar *shortcut_id,
			      GCancellable *cancellable,
			      GError **error)
{
	return g_dbus_connection_call_sync (session_bus,
					    "org.gnome.Shell",
					    "/org/gnome/Shell",
					    "org.gnome.Shell.AppStore",
					    "AddAppIfNotVisible",
					    g_variant_new ("(s)", shortcut_id),
					    NULL,
					    G_DBUS_CALL_FLAGS_NONE,
					    -1,
					    cancellable,
					    error);
}

static GVariant *
shell_replace_app (GDBusConnection *session_bus,
		   const char *original_shortcut_id,
		   const char *replacement_shortcut_id,
		   GCancellable *cancellable,
		   GError **error)
{
	return g_dbus_connection_call_sync (session_bus,
					    "org.gnome.Shell",
					    "/org/gnome/Shell",
					    "org.gnome.Shell.AppStore",
					    "ReplaceApplication",
					    g_variant_new ("(ss)",
					                   original_shortcut_id,
					                   replacement_shortcut_id),
					    NULL,
					    G_DBUS_CALL_FLAGS_NONE,
					    -1,
					    cancellable,
					    error);
}

static gboolean
add_app_to_shell (GsPlugin	*plugin,
		  GsApp		*app,
		  GCancellable	*cancellable,
		  GError	**error_out)
{
	GError *error = NULL;
	GsPluginData *priv = gs_plugin_get_data (plugin);
	g_autofree char *desktop_file_id = get_desktop_file_id (app);

	/* Look up the app in our replacement list to see if we
	 * can replace an existing shortcut, and if so, do that
	 * instead */
	const char *shortcut_id_to_replace = g_hash_table_lookup (priv->replacement_app_lookup,
								  desktop_file_id);


	if (shortcut_id_to_replace)
		shell_replace_app (priv->session_bus,
				   shortcut_id_to_replace,
				   desktop_file_id,
				   cancellable,
				   &error);
	else
		shell_add_app_if_not_visible (priv->session_bus,
					      desktop_file_id,
					      cancellable,
					      &error);

	if (error != NULL) {
		g_debug ("Error adding app to shell: %s", error->message);
		g_propagate_error (error_out, error);
		return FALSE;
	}

	return TRUE;
}

gboolean
gs_plugin_add_shortcut (GsPlugin	*plugin,
			GsApp		*app,
			GCancellable	*cancellable,
			GError		**error)
{
	gs_app_add_quirk (app, GS_APP_QUIRK_HAS_SHORTCUT);
	return add_app_to_shell (plugin, app, cancellable, error);
}

gboolean
gs_plugin_remove_shortcut (GsPlugin	*plugin,
			   GsApp	*app,
			   GCancellable	*cancellable,
			   GError	**error)
{
	gs_app_remove_quirk (app, GS_APP_QUIRK_HAS_SHORTCUT);
	return remove_app_from_shell (plugin, app, cancellable, error);
}

gboolean
gs_plugin_app_install (GsPlugin *plugin,
		       GsApp *app,
		       GCancellable *cancellable,
		       GError **error)
{
	g_autoptr(GError) local_error = NULL;
	if (!app_is_flatpak (app))
		return TRUE;

	/* We're only interested in already installed flatpak apps so we can
	 * add them to the desktop */
	if (gs_app_get_state (app) != AS_APP_STATE_INSTALLED)
		return TRUE;

	if (!add_app_to_shell (plugin, app, cancellable, &local_error)) {
		g_warning ("Failed to add shortcut: %s",
			   local_error->message);
	}
	return TRUE;
}

static gboolean
launch_with_sys_desktop_file (GsApp *app,
                              GError **error)
{
	GdkDisplay *display;
	g_autoptr(GAppLaunchContext) context = NULL;
	g_autofree char *desktop_file_id = get_desktop_file_id (app);
	g_autoptr(GDesktopAppInfo) app_info =
		gs_utils_get_desktop_app_info (desktop_file_id);
	g_autoptr(GError) local_error = NULL;
	gboolean ret;

	display = gdk_display_get_default ();
	context = G_APP_LAUNCH_CONTEXT (gdk_display_get_app_launch_context (display));
	ret = g_app_info_launch (G_APP_INFO (app_info), NULL, context, &local_error);

	if (!ret) {
		g_warning ("Could not launch %s: %s", gs_app_get_unique_id (app),
			   local_error->message);
		g_set_error (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_FAILED,
			     _("Could not launch this application."));
	}

	return ret;
}

gboolean
gs_plugin_app_remove (GsPlugin *plugin,
		      GsApp *app,
		      GCancellable *cancellable,
		      GError **error)
{
	g_autoptr(GError) local_error = NULL;
	if (!app_is_flatpak (app))
		return TRUE;

	/* We're only interested in apps that have been successfully uninstalled */
	if (gs_app_is_installed (app))
		return TRUE;

	if (!remove_app_from_shell (plugin, app, cancellable, &local_error)) {
		g_warning ("Failed to remove shortcut: %s",
			   local_error->message);
	}
	return TRUE;
}

gboolean
gs_plugin_launch (GsPlugin *plugin,
		  GsApp *app,
		  GCancellable *cancellable,
		  GError **error)
{
	/* if the app is one of the system ones, we simply launch it through the
	 * plugin's app launcher */
	if (gs_app_has_quirk (app, GS_APP_QUIRK_COMPULSORY) &&
	    !app_is_flatpak (app))
		return gs_plugin_app_launch (plugin, app, error);

	/* for apps that have a special desktop file (e.g. Google Chrome) */
	if (gs_app_get_metadata_item (app, METADATA_SYS_DESKTOP_FILE))
		return launch_with_sys_desktop_file (app, error);

	return TRUE;
}

static GsApp *
gs_plugin_eos_create_proxy_app (GsPlugin *plugin,
				const char *id,
				const char *name,
				const char *summary)
{
	GsApp *proxy = gs_app_new (id);
	g_autoptr(AsIcon) icon;

	gs_app_set_scope (proxy, AS_APP_SCOPE_SYSTEM);
	gs_app_set_kind (proxy, AS_APP_KIND_RUNTIME);
	gs_app_set_name (proxy, GS_APP_QUALITY_NORMAL, name);
	gs_app_set_summary (proxy, GS_APP_QUALITY_NORMAL, summary);
	gs_app_set_state (proxy, AS_APP_STATE_UPDATABLE_LIVE);
	gs_app_add_quirk (proxy, GS_APP_QUIRK_IS_PROXY);
	gs_app_set_management_plugin (proxy, gs_plugin_get_name (plugin));

	icon = as_icon_new ();
	as_icon_set_kind (icon, AS_ICON_KIND_STOCK);
	as_icon_set_name (icon, "system-run-symbolic");
	gs_app_add_icon (proxy, icon);

	return proxy;
}

static void
process_proxy_updates (GsPlugin *plugin,
		       GsAppList *list,
		       GsApp *proxy_app,
		       gboolean install_missing,
		       const char **proxied_apps)
{
	g_autoptr(GsAppList) proxied_updates = gs_app_list_new ();

	GsApp *cached_proxy_app = gs_app_list_lookup (list,
						      gs_app_get_unique_id (proxy_app));
	if (cached_proxy_app != NULL)
		gs_app_list_remove (list, cached_proxy_app);

	/* add all the apps if we should install the missing ones; the real sorting for
	 * whether they're already installed is done in the refine method */
	if (install_missing) {
		for (guint i = 0; proxied_apps[i] != NULL; ++i) {
		        g_autoptr(GsApp) app = gs_app_new (proxied_apps[i]);
			gs_app_add_quirk (app, GS_APP_QUIRK_IS_WILDCARD);
			gs_app_set_bundle_kind (app, AS_BUNDLE_KIND_FLATPAK);

			gs_app_list_add (proxied_updates, app);
		}
	}

	for (guint i = 0; i < gs_app_list_length (list); ++i) {
		GsApp *app = gs_app_list_index (list, i);
		GsApp *added_app = NULL;
		const char *id = gs_app_get_id (app);

		if (!g_strv_contains (proxied_apps, id) ||
		    gs_app_get_scope (proxy_app) != gs_app_get_scope (app))
			continue;

		added_app = gs_app_list_lookup (proxied_updates, gs_app_get_unique_id (app));
		if (added_app == app)
			continue;

		/* remove any app matching the updatable one we're about to add as
		 * this makes sure that we're getting the right app (updatable) in
		 * the proxy app's related list */
		gs_app_list_remove (proxied_updates, added_app);

		/* ensure the app we're about to add really is updatable; this
		 * is mostly a safeguard, since in this plugin's refine of proxy
		 * apps we remove any apps that are not updatable */
		gs_app_set_state (app, AS_APP_STATE_UPDATABLE_LIVE);
		gs_app_list_add (proxied_updates, app);
	}

	if (gs_app_list_length (proxied_updates) == 0)
		return;

	for (guint i = 0; i < gs_app_list_length (proxied_updates); ++i) {
		GsApp *app = gs_app_list_index (proxied_updates, i);
		gs_app_add_related (proxy_app, app);

		/* remove proxied apps from updates list since they will be
		 * updated from the proxy app */
		gs_app_list_remove (list, app);
	}

	gs_app_list_add (list, proxy_app);
}

static gboolean
add_updates (GsPlugin *plugin,
	     GsAppList *list,
	     GCancellable *cancellable,
	     GError **error)
{
	g_autoptr(GsApp) framework_proxy_app =
		gs_plugin_eos_create_proxy_app (plugin,
						EOS_PROXY_APP_PREFIX ".EOSUpdatesProxy",
						/* TRANSLATORS: this is the name of the Endless Platform app */
						_("Endless Platform"),
						/* TRANSLATORS: this is the summary of the Endless Platform app */
						_("Framework for applications"));
	g_autoptr(GsApp) hack_proxy_app =
		gs_plugin_eos_create_proxy_app (plugin,
						EOS_PROXY_APP_PREFIX ".EOSHackUpdatesProxy",
						/* TRANSLATORS: this is the name of the Endless Hack Platform app */
						_("Endless Hack Platform"),
						/* TRANSLATORS: this is the summary of the Endless Hack Platform app */
						_("Endless Hack system components"));

	const char *framework_proxied_apps[] = {"com.endlessm.Platform",
						"com.endlessm.apps.Platform",
						"com.endlessm.CompanionAppService.desktop",
						"com.endlessm.EknServicesMultiplexer.desktop",
						"com.endlessm.quote_of_the_day.en.desktop",
						"com.endlessm.word_of_the_day.en.desktop",
						NULL};
	const char *hack_proxied_apps[] = {"com.endlessm.Clubhouse.desktop",
					   "com.endlessm.Fizzics.desktop",
					   "com.endlessm.GameStateService.desktop",
					   "com.endlessm.HackAccountService.desktop",
					   "com.endlessm.HackComponents.desktop",
					   "com.endlessm.HackSoundServer.desktop",
					   "com.endlessm.HackToolbox.desktop",
					   "com.endlessm.HackUnlock.desktop",
					   "com.endlessm.Hackdex_chapter_one.desktop",
					   "com.endlessm.Hackdex_chapter_two.desktop",
					   "com.endlessm.LightSpeed.desktop",
					   "com.endlessm.OperatingSystemApp.desktop",
					   "com.endlessm.Sidetrack.desktop",
					   NULL};

	GsPluginData *priv = gs_plugin_get_data (plugin);
	gboolean is_hack_product = g_strcmp0 (priv->product_name, "hack") == 0;

	process_proxy_updates (plugin, list,
			       framework_proxy_app,
			       FALSE,
			       framework_proxied_apps);
	process_proxy_updates (plugin, list,
			       hack_proxy_app,
			       is_hack_product,
			       hack_proxied_apps);

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

		gs_plugin_eos_remove_blacklist_from_usb_if_needed (plugin, app);
	}

	return TRUE;
}

gboolean
gs_plugin_add_updates_pending (GsPlugin *plugin,
			       GsAppList *list,
			       GCancellable *cancellable,
			       GError **error)
{
	return add_updates (plugin, list, cancellable, error);
}

gboolean
gs_plugin_add_updates (GsPlugin *plugin,
		       GsAppList *list,
		       GCancellable *cancellable,
		       GError **error)
{
	/* only the gs_plugin_add_updates_pending should be used in EOS
	 * but in case the user has downloaded updates (e.g. by having used the
	 * Flatpak CLI) this will still work correctly */
	return add_updates (plugin, list, cancellable, error);
}

gboolean
gs_plugin_add_popular (GsPlugin *plugin,
		       GsAppList *list,
		       GCancellable *cancellable,
		       GError **error)
{
	g_autoptr(AsProfileTask) ptask = NULL;
	GsAppList *new_list = NULL;
	/* These IDs must match the <component><id> element in the app's
	 * appdata. Note in particular that some apps have a .desktop suffix
	 * there, and some do not.
	 */
	const gchar *popular_apps[] = {
		"com.calibre_ebook.calibre.desktop",
		"com.endlessm.photos.desktop",
		"com.google.Chrome.desktop",
		"com.spotify.Client.desktop",
		"com.transmissionbt.Transmission",
		"com.valvesoftware.Steam.desktop",
		"io.github.mmstick.FontFinder.desktop",
		"net.minetest.Minetest",
		"net.scribus.Scribus",
		"org.audacityteam.Audacity",
		"org.gimp.GIMP",
		"org.gnome.Chess",
		"org.gnome.SwellFoop",
		"org.inkscape.Inkscape",
		"org.kde.gcompris.desktop",
		"org.kde.krita",
		"org.libreoffice.LibreOffice.desktop",
		"org.mozilla.Firefox.desktop",
		"org.telegram.desktop.desktop",
		"org.tuxpaint.Tuxpaint.desktop",
		"org.videolan.VLC.desktop",
		/* TODO: this doesn't end up in the list of apps used by the
		 * overview. Relevant log output:
		 *
		 *   As  run 0x562bb3977e30~GsPlugin::appstream(gs_plugin_add_popular;gs_plugin_refine_wildcard)
		 *   Gs  not using simple-scan.desktop for wildcard as no bundle or pkgname
		 *
		 * This app is built into the ostree. Its ID is, without spaces:
		 *
		 *   system / * / * / desktop / simple-scan.desktop / *
		 *            ^   ^ these are probably bundle and pkgname?
		 */
		"simple-scan.desktop",
		NULL
	};

	ptask = as_profile_start_literal (gs_plugin_get_profile (plugin),
					  "eos::add-popular");

	new_list = gs_app_list_new ();

	/* add the hardcoded list of popular apps */
	for (guint i = 0; popular_apps[i] != NULL; ++i) {
		g_autoptr(GsApp) app = gs_app_new (popular_apps[i]);
		gs_app_add_quirk (app, GS_APP_QUIRK_IS_WILDCARD);
		gs_app_list_add (new_list, app);
	}

	/* get all the popular apps that are Endless' ones */
	for (guint i = 0; i < gs_app_list_length (list); ++i) {
	        GsApp *app = gs_app_list_index (list, i);
		const gchar *app_id = gs_app_get_id (app);
		const gchar *origin = gs_app_get_origin (app);
		gboolean add = FALSE;

		if (!app_id || !origin)
			continue;

		/* com.endlessm. apps from eos-apps */
		if (!g_strcmp0 (origin, "eos-apps") &&
		    g_str_has_prefix (app_id, "com.endlessm."))
			add = TRUE;
		/* all com.endlessnetwork. apps */
		else if (g_str_has_prefix (app_id, "com.endlessnetwork."))
			add = TRUE;

		if (add)
			gs_app_list_add (new_list, app);
	}

	/* replace the list of popular apps so far by ours */
	gs_app_list_remove_all (list);
	gs_app_list_add_list (list, new_list);

	return TRUE;
}

static void
setup_os_upgrade (GsPlugin *plugin)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	GsApp *app = NULL;
	g_autoptr(AsIcon) ic = NULL;

	if (priv->os_upgrade != NULL)
		return;

	/* use stock icon */
	ic = as_icon_new ();
	as_icon_set_kind (ic, AS_ICON_KIND_STOCK);
	as_icon_set_name (ic, "application-x-addon");

	/* create the OS upgrade */
	app = gs_app_new (EOS_UPGRADE_ID);
	gs_app_add_icon (app, ic);
	gs_app_set_scope (app, AS_APP_SCOPE_SYSTEM);
	gs_app_set_kind (app, AS_APP_KIND_OS_UPGRADE);
	gs_app_set_name (app, GS_APP_QUALITY_LOWEST, "Endless OS");
	gs_app_set_summary (app, GS_APP_QUALITY_NORMAL,
			    _("An Endless update with new features and fixes."));
	/* ensure that the version doesn't appear as (NULL) in the banner, it
	 * should be changed to the right value when it changes in the eos-updater */
	gs_app_set_version (app, "");
	gs_app_set_name (app, GS_APP_QUALITY_LOWEST, "Endless OS");
	gs_app_add_quirk (app, GS_APP_QUIRK_NEEDS_REBOOT);
	gs_app_add_quirk (app, GS_APP_QUIRK_PROVENANCE);
	gs_app_add_quirk (app, GS_APP_QUIRK_NOT_REVIEWABLE);
	gs_app_set_management_plugin (app, gs_plugin_get_name (plugin));
	gs_app_set_metadata (app, "GnomeSoftware::UpgradeBanner-css",
			     "background: url('" DATADIR "/gnome-software/upgrade-bg.png');"
			     "background-size: 100% 100%;");

	priv->os_upgrade = app;

	/* for debug purposes we create the OS upgrade even if the EOS updater is NULL */
	if (priv->updater_proxy != NULL) {
		/* sync initial state */
		sync_state_from_updater (plugin);

		g_signal_connect_object (priv->updater_proxy, "notify::state",
					 G_CALLBACK (updater_state_changed),
					 plugin, G_CONNECT_SWAPPED);
		g_signal_connect_object (priv->updater_proxy,
					 "notify::downloaded-bytes",
					 G_CALLBACK (updater_downloaded_bytes_changed),
					 plugin, G_CONNECT_SWAPPED);
		g_signal_connect_object (priv->updater_proxy, "notify::version",
					 G_CALLBACK (updater_version_changed),
					 plugin, G_CONNECT_SWAPPED);
	}
}

static gboolean
should_add_os_upgrade (GsApp *os_upgrade)
{
	switch (gs_app_get_state (os_upgrade)) {
	case AS_APP_STATE_AVAILABLE:
	case AS_APP_STATE_INSTALLING:
	case AS_APP_STATE_UPDATABLE:
		return TRUE;
	default:
		break;
	}

	return FALSE;
}

static void
check_for_os_updates (GsPlugin *plugin)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	EosUpdaterState updater_state;

	/* check if the OS upgrade has been disabled */
	if (priv->updater_proxy == NULL)
		return;

	/* poll in the error/none/ready states to check if there's an
	 * update available */
	updater_state = eos_updater_get_state (priv->updater_proxy);
	switch (updater_state) {
	case EOS_UPDATER_STATE_ERROR:
	case EOS_UPDATER_STATE_NONE:
	case EOS_UPDATER_STATE_READY:
		eos_updater_call_poll (priv->updater_proxy, NULL, NULL, NULL);
	default:
		break;
	}
}

gboolean
gs_plugin_refresh (GsPlugin *plugin,
		   guint cache_age,
		   GsPluginRefreshFlags flags,
		   GCancellable *cancellable,
		   GError **error)
{
	check_for_os_updates (plugin);

	return TRUE;
}

gboolean
gs_plugin_add_distro_upgrades (GsPlugin *plugin,
			       GsAppList *list,
			       GCancellable *cancellable,
			       GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);

	/* if we are testing the plugin, then always add the OS upgrade */
	if (g_getenv ("GS_PLUGIN_EOS_TEST") != NULL) {
		if  (gs_app_get_state (priv->os_upgrade) == AS_APP_STATE_UNKNOWN)
			gs_app_set_state (priv->os_upgrade, AS_APP_STATE_AVAILABLE);
		gs_app_list_add (list, priv->os_upgrade);
		return TRUE;
	}

	/* check if the OS upgrade has been disabled */
	if (priv->updater_proxy == NULL)
		return TRUE;

	if (should_add_os_upgrade (priv->os_upgrade)) {
		g_debug ("Adding EOS upgrade: %s",
			 gs_app_get_unique_id (priv->os_upgrade));
		gs_app_list_add (list, priv->os_upgrade);
	}

	return TRUE;
}

gboolean
gs_plugin_app_upgrade_download (GsPlugin *plugin,
				GsApp *app,
			        GCancellable *cancellable,
				GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);

	/* only process this app if was created by this plugin */
	if (g_strcmp0 (gs_app_get_management_plugin (app),
		       gs_plugin_get_name (plugin)) != 0)
		return TRUE;

	if (app != priv->os_upgrade) {
		g_warning ("The OS upgrade to download (%s) differs from the "
			   "one in the EOS plugin, yet it's managed by it!",
			   gs_app_get_unique_id (app));
		return TRUE;
	}

	/* if the OS upgrade has been disabled */
	if (priv->updater_proxy == NULL) {
		g_set_error (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_FAILED,
			     "the OS upgrade has been disabled in the EOS plugin");
		return FALSE;
	}

	os_upgrade_set_download_by_user (app, TRUE);

	if (updater_is_stalled (plugin)) {
		os_upgrade_set_restart_on_error (app, TRUE);
		eos_updater_call_cancel (priv->updater_proxy, NULL, NULL, NULL);
		return TRUE;
	}

	/* we need to poll again if there has been an error; the state of the
	 * OS upgrade will then be dealt with from outside this function,
	 * according to the state changes of the update itself */
	if (eos_updater_get_state (priv->updater_proxy) == EOS_UPDATER_STATE_ERROR)
		eos_updater_call_poll (priv->updater_proxy, NULL, NULL, NULL);
	else
		sync_state_from_updater (plugin);

	return TRUE;
}

gboolean
gs_plugin_file_to_app (GsPlugin *plugin,
		       GsAppList *list,
		       GFile *file,
		       GCancellable *cancellable,
		       GError **error)
{
	g_autofree gchar *content_type = NULL;
	const gchar *mimetypes_repo[] = {
		"inode/directory",
		NULL };

	/* does this match any of the mimetypes we support */
	content_type = gs_utils_get_content_type (file, cancellable, error);
	if (content_type == NULL)
		return FALSE;
	if (g_strv_contains (mimetypes_repo, content_type)) {
		/* If it looks like an ostree repo that could be on a USB drive,
		 * have eos-updater check it for available OS updates */
		g_autoptr (GFile) repo_dir = NULL;

		repo_dir = g_file_get_child (file, ".ostree");
		if (g_file_query_exists (repo_dir, NULL))
			check_for_os_updates (plugin);
	}

	return TRUE;
}

static char *
get_os_collection_id (void)
{
	OstreeDeployment *booted_deployment;
	GKeyFile *origin;
	g_autofree char *refspec = NULL;
	g_autofree char *remote = NULL;
	g_autofree char *collection_id = NULL;
	g_autoptr(OstreeRepo) repo = NULL;
	g_autoptr(OstreeSysroot) sysroot = NULL;
	g_autoptr(GError) error = NULL;

	sysroot = ostree_sysroot_new_default ();
	if (!ostree_sysroot_load (sysroot, NULL, &error))
		goto err_out;

	booted_deployment = ostree_sysroot_get_booted_deployment (sysroot);
	if (booted_deployment == NULL)
		return NULL;

	origin = ostree_deployment_get_origin (booted_deployment);
	if (origin == NULL)
		return NULL;

	refspec = g_key_file_get_string (origin, "origin", "refspec", &error);
	if (refspec == NULL)
		goto err_out;

	ostree_parse_refspec (refspec, &remote, NULL, &error);
	if (remote == NULL)
		goto err_out;

	repo = ostree_repo_new_default ();
	if (!ostree_repo_open (repo, NULL, &error))
		goto err_out;

	if (!ostree_repo_get_remote_option (repo, remote, "collection-id", NULL, &collection_id, &error))
		goto err_out;

	return g_steal_pointer (&collection_id);

err_out:
	if (error != NULL)
		g_debug ("failed to get OSTree collection ID: %s", error->message);
	return NULL;
}

gboolean
gs_plugin_os_get_copyable (GsPlugin *plugin,
			   const gchar *copy_dest,
			   gboolean *copyable,
			   GCancellable *cancellable,
			   GError **error)
{
	g_autofree char *collection_id = get_os_collection_id ();

	*copyable = (collection_id != NULL);

	return TRUE;
}

typedef struct {
	GCancellable *cancellable;
	gulong cancelled_id;
	gboolean finished;
	GError *error;
} OsCopyProcessHelper;

static void
os_copy_process_helper_free (OsCopyProcessHelper *helper)
{
	g_clear_object (&helper->cancellable);
	g_clear_error (&helper->error);
	g_free (helper);
}

static OsCopyProcessHelper *
os_copy_process_helper_new (GCancellable *cancellable, gulong cancelled_id)
{
	OsCopyProcessHelper *helper = g_new0 (OsCopyProcessHelper, 1);
	helper->cancellable = g_object_ref (cancellable);
	helper->cancelled_id = cancelled_id;
	helper->finished = FALSE;
	helper->error = NULL;
	return helper;
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(OsCopyProcessHelper, os_copy_process_helper_free)

static void
os_copy_process_watch_cb (GPid pid, gint status, gpointer user_data)
{
	OsCopyProcessHelper *helper = user_data;
	g_autoptr(GError) error = NULL;

	if (!g_cancellable_is_cancelled (helper->cancellable) && status != 0)
		g_set_error (&helper->error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "failed to copy OS to removable media: command "
			     "failed with status %d", status);

	g_cancellable_disconnect (helper->cancellable, helper->cancelled_id);
	g_spawn_close_pid (pid);

	/* once the copy terminates (successfully or not), set plugin status to
	 * update UI accordingly */

	helper->finished = TRUE;
}

static void
os_copy_cancelled_cb (GCancellable *cancellable, gpointer user_data)
{
	GPid pid = GPOINTER_TO_INT (user_data);

	/* terminate the process which is copying the OS */
	kill (pid, SIGTERM);
}

gboolean
gs_plugin_os_copy (GsPlugin *plugin,
		   const gchar *copy_dest,
		   GCancellable *cancellable,
		   GError **error)
{
	/* this is used in an async function but we block here until that
	 * returns so we won't auto-free while other threads depend on this */
	g_autoptr (OsCopyProcessHelper) helper = NULL;
	gboolean spawn_retval;
	const gchar *argv[] = {"/usr/bin/pkexec",
			       "/usr/bin/eos-updater-prepare-volume",
			       copy_dest,
			       NULL};
	GPid child_pid;
	gulong cancelled_id;

	g_debug ("Copying OS to: %s", copy_dest);

	spawn_retval = g_spawn_async (".",
				      (gchar **) argv,
				      NULL,
				      G_SPAWN_DO_NOT_REAP_CHILD,
				      NULL,
				      NULL,
				      &child_pid,
				      error);

	if (spawn_retval) {
		cancelled_id = g_cancellable_connect (cancellable,
						      G_CALLBACK (os_copy_cancelled_cb),
						      GINT_TO_POINTER (child_pid),
						      NULL);

		helper = os_copy_process_helper_new (cancellable,
								cancelled_id);
		g_child_watch_add (child_pid, os_copy_process_watch_cb, helper);
	} else
		return FALSE;

	/* Iterate the main loop until either the copy process completes or the
	 * user cancels the copy. Without this, it is impossible to cancel the
	 * copy because we reach the end of this function, its parent GTask
	 * returns and we disconnect the handler that would kill the copy
	 * process. */
	while (!helper->finished)
		g_main_context_iteration (NULL, FALSE);

	if (helper->error) {
		g_propagate_error (error, g_steal_pointer (&helper->error));
		return FALSE;
	}

	return TRUE;
}
