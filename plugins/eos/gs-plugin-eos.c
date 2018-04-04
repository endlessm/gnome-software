/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2016-2017 Endless Mobile, Inc
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
#include <gnome-software.h>
#include <glib/gi18n.h>
#include <gs-plugin.h>
#include <gs-utils.h>
#include <libsoup/soup.h>
#include <sys/types.h>
#include <sys/xattr.h>

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
	gboolean is_coding_enabled;
	char *os_version_id;
	gboolean eos_arch_is_arm;
};

static GHashTable *
get_applications_with_shortcuts (GsPlugin	*plugin,
				 GCancellable	*cancellable,
				 GError		**error) {
	g_autoptr (GVariantIter) iter = NULL;
	g_autoptr (GVariant) apps = NULL;
	gchar *application;
	GHashTable *apps_table;
	GsPluginData *priv = gs_plugin_get_data (plugin);

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
		return NULL;

	apps_table = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
					    NULL);

	g_variant_get (apps, "(as)", &iter);
	while (g_variant_iter_loop (iter, "s", &application))
		g_hash_table_add (apps_table, g_strdup (application));

	return apps_table;
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

	apps = get_applications_with_shortcuts (plugin, NULL, &error);
	if (apps == NULL) {
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
			gs_app_remove_quirk (app, AS_APP_QUIRK_HAS_SHORTCUT);

		g_hash_table_iter_remove (&iter);
	}

	/* add any apps that have shortcuts now */
	g_hash_table_iter_init (&iter, apps);
	while (g_hash_table_iter_next (&iter, &key, &value)) {
		GsApp *app = gs_plugin_cache_lookup (plugin, key);

		if (app)
			gs_app_add_quirk (app, AS_APP_QUIRK_HAS_SHORTCUT);

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
	char *image_version =
		get_image_version_for_path (EOS_IMAGE_VERSION_PATH);

	if (!image_version)
		image_version =
			get_image_version_for_path (EOS_IMAGE_VERSION_ALT_PATH);

	return image_version;
}

static char *
get_personality (void)
{
	g_autofree char *image_version = get_image_version ();
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

gboolean
gs_plugin_setup (GsPlugin *plugin,
		 GCancellable *cancellable,
		 GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	priv->desktop_apps = get_applications_with_shortcuts (plugin, cancellable, error);
	return TRUE;
}

void
gs_plugin_initialize (GsPlugin *plugin)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GSettings) settings = NULL;
	GApplication *app = g_application_get_default ();
	GsPluginData *priv = gs_plugin_alloc_data (plugin,
						   sizeof(GsPluginData));

	/* let the flatpak plugin run first so we deal with the apps
	 * in a more complete/refined state */
	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_AFTER, "flatpak");

	/* we already deal with apps that need to be proxied, so let's impede
	 * the other plugin from running */
	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_CONFLICTS, "generic-updates");

	priv->eos_arch_is_arm = g_strcmp0 (flatpak_get_default_arch (), "arm") == 0;

	priv->session_bus = g_application_get_dbus_connection (app);

	priv->replacement_app_lookup = g_hash_table_new_full (g_str_hash, g_str_equal,
							      g_free, g_free);
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
	priv->personality = get_personality ();

	settings = g_settings_new ("org.gnome.shell");
	priv->is_coding_enabled = g_settings_get_boolean (settings, "enable-coding-game");

	/* Synchronous, but this guarantees that the lookup table will be
	 * there when we call ReplaceApplication later on */
	read_icon_replacement_overrides (priv->replacement_app_lookup);

	if (!priv->personality)
		g_warning ("No system personality could be retrieved!");

	priv->os_version_id = get_os_version_id (&error);
	if (!priv->os_version_id)
		g_warning ("No OS version ID could be set: %s",
			   error->message);
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
	g_free (priv->os_version_id);
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

	if (cached_app && !gs_app_is_installed (cached_app)) {
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

	if (!gs_plugin_locale_is_compatible (plugin, last_token)) {
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
	    gs_plugin_app_is_locale_best_match (plugin, cached_app)) {
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
		"com.arduino.App",
		"com.dropbox.Client",
		"com.github.Slingshot",
		"com.google.Chrome",
		"com.microsoft.Skype",
		"com.skype.Client",
		"com.mojang.Minecraft",
		"com.sparklinlabs.Superpowers",
		"com.stencyl.Game",
		"com.sublimetext.three",
		"com.visualstudio.code.oss",
		"de.billardgl.Billardgl",
		"io.github.Supertux",
		"org.supertuxproject.SuperTux",
		"net.blockout.Blockout2",
		"net.blockout.BlockOutII",
		"net.gcompris.Gcompris",
		"net.olofson.Kobodeluxe",
		"net.olofson.KoboDeluxe",
		"net.sourceforge.Atanks",
		"net.sourceforge.atanks",
		"net.sourceforge.Audacity",
		"org.audacityteam.Audacity",
		"net.sourceforge.Btanks",
		"net.sourceforge.btanks",
		"net.sourceforge.ChromiumBSU",
		"net.sourceforge.chromium-bsu",
		"net.sourceforge.Extremetuxracer",
		"net.sourceforge.ExtremeTuxRacer",
		"net.sourceforge.Frostwire",
		"net.sourceforge.Rili",
		"net.sourceforge.Ri-li",
		"net.sourceforge.Supertuxkart",
		"net.supertuxkart.SuperTuxKart",
		"net.sourceforge.Torcs",
		"net.sourceforge.torcs",
		"net.sourceforge.Tuxfootball",
		"net.sourceforge.TuxFootball",
		"net.sourceforge.Warmux",
		"org.gna.Warmux",
		"net.wz2100.Warzone2100",
		"org.armagetronad.Armagetronad",
		"org.armagetronad.ArmagetronAdvanced",
		"org.codeblocks.App",
		"org.debian.Tuxpuck",
		"org.debian.TuxPuck",
		"org.debian.alioth.tux4kids.Tuxmath",
		"com.tux4kids.tuxmath",
		"org.debian.alioth.tux4kids.Tuxtype",
		"com.tux4kids.tuxtype",
		"org.eclipse.Eclipse",
		"org.frozenbubble.FrozenBubble",
		"org.frozen_bubble.frozen-bubble",
		"org.gimp.Gimp",
		"org.gimp.GIMP",
		"org.gnome.Freecell",
		"org.gnome.Iagno",
		"org.gnome.iagno",
		"org.gnome.Quadrapassel",
		"org.gnome.quadrapassel",
		"org.gnome.Solitaire",
		"org.gnome.Aisleriot",
		"org.gnome.Tetravex",
		"org.gnome.tetravex",
		"org.gnome.people.dscorgie.Labyrinth",
		"org.kde.Kalzium",
		"org.kde.Kapman",
		"org.kde.Katomic",
		"org.kde.Kblocks",
		"org.kde.Kbounce",
		"org.kde.Kbruch",
		"org.kde.Kdiamond",
		"org.kde.Kgeography",
		"org.kde.Kgoldrunner",
		"org.kde.Khangman",
		"org.kde.Kigo",
		"org.kde.Killbots",
		"org.kde.Kjumpingcube",
		"org.kde.Klines",
		"org.kde.Knavalbattle",
		"org.kde.Knetwalk",
		"org.kde.Ksame",
		"org.kde.Ksquares",
		"org.kde.Ksudoku",
		"org.kde.Ktuberling",
		"org.kde.Kubrick",
		"org.kde.Kwordquiz",
		"org.kde.Palapeli",
		"org.learningequality.KALite",
		"org.maemo.Numptyphysics",
		"io.thp.numptyphysics",
		"org.marsshooter.Marsshooter",
		"net.sourceforge.mars-game",
		"org.mozilla.Firefox",
		"org.openarena.Openarena",
		"ws.openarena.OpenArena",
		"org.openscad.Openscad",
		"org.platformio.Ide",
		"org.processing.App",
		"org.seul.Pingus",
		"org.seul.pingus",
		"org.snap4arduino.App",
		"org.squeakland.Etoys",
		"org.squeakland.Scratch",
		"org.stellarium.Stellarium",
		"org.sugarlabs.Turtleblocks",
		"org.tuxfamily.Xmoto",
		"org.tuxfamily.XMoto",
		NULL
	};

	static const char *core_apps[] = {
		"org.gnome.Calculator",
		"org.gnome.Evince",
		"org.gnome.Nautilus",
		"org.gnome.Rhythmbox3",
		"org.gnome.Totem",
		"org.gnome.clocks",
		"org.gnome.eog",
		"org.gnome.gedit",
		"org.libreoffice.LibreOffice",
		NULL
	};

	/* Flatpak apps known not to be working properly */
	static const char *buggy_apps[] = {
		/* Missing lots of keys and defaults specified in eos-theme */
		"ca.desrt.dconf-editor",
		/* Can't open LibreOffice documents */
		"org.gnome.Documents",
		NULL
	};

	/* List of apps that are proven to work on ARM */
	static const char *arm_whitelist[] = {
		"ch.x29a.playitslowly",
		"com.bixense.PasswordCalculator",
		"com.dosbox.DOSBox",
		"com.frac_tion.teleport",
		"com.github.babluboy.bookworm",
		"com.github.bilelmoussaoui.Authenticator",
		"com.github.birros.WebArchives",
		"com.github.bitseater.weather",
		"com.github.cassidyjames.dippi",
		"com.github.dahenson.agenda",
		"com.github.donadigo.appeditor",
		"com.github.fabiocolacio.marker",
		"com.github.geigi.cozy",
		"com.github.gkarsay.parlatype",
		"com.github.gyunaev.spivak",
		"com.github.hluk.copyq",
		"com.github.lainsce.notejot",
		"com.github.needleandthread.vocal",
		"com.github.ojubaorg.Othman",
		"com.github.paolostivanin.OTPClient",
		"com.github.philip_scott.notes-up",
		"com.github.philip_scott.spice-up",
		"com.github.quaternion",
		"com.github.rssguard",
		"com.transmissionbt.Transmission",
		"com.uploadedlobster.peek",
		"cx.ring.Ring",
		"de.haeckerfelix.gradio",
		"de.manuel_kehl.go-for-it",
		"fr.free.Homebank",
		"im.srain.Srain",
		"io.elementary.code",
		"io.github.Cockatrice.cockatrice",
		"io.github.Hexchat",
		"io.github.Pithos",
		"io.github.cloose.CuteMarkEd",
		"io.github.jliljebl.Flowblade",
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
		"net.sf.VICE",
		"net.sf.nootka",
		"net.sourceforge.Klavaro",
		"nl.openoffice.bluefish",
		"org.baedert.corebird",
		"org.blender.Blender",
		"org.freeciv.Freeciv",
		"org.freefilesync.FreeFileSync",
		"org.gabmus.hydrapaper",
		"org.geany.Geany",
		"org.gnome.Books",
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
		"org.gnome.Hitori",
		"org.gnome.Lollypop",
		"org.gnome.Maps",
		"org.gnome.Polari",
		"org.gnome.Recipes",
		"org.gnome.Todo",
		"org.gnome.Weather",
		"org.gnome.bijiben",
		"org.gnome.frogr",
		"org.gnome.gbrainy",
		"org.gnome.ghex",
		"org.gnome.gitg",
		"org.gnome.glabels-3",
		"org.gnome.meld",
		"org.gnucash.GnuCash",
		"org.gottcode.FocusWriter",
		"org.inkscape.Inkscape",
		"org.keepassxc.KeePassXC",
		"org.kicad_pcb.KiCad",
		"org.mapeditor.Tiled",
		"org.musicbrainz.Picard",
		"org.mypaint.MyPaint",
		"org.nextcloud.Nextcloud",
		"org.pitivi.Pitivi",
		"org.qbittorrent.qBittorrent",
		"org.quassel_irc.QuasselClient",
		"org.telegram.desktop",
		"org.tordini.flavio.Minitube",
		"org.vim.Vim",
		"org.wesnoth.Wesnoth",
		"org.xiphos.Xiphos",
		"work.openpaper.Paperwork",
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

static gboolean
app_is_banned_for_personality (GsPlugin *plugin, GsApp *app)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	const char *app_name = gs_flatpak_app_get_ref_name (app);

	static const char *violent_apps[] = {
		"io.github.FreeDM",
		"io.github.Freedoom-Phase-1",
		"io.github.Freedoom-Phase-2",
		"org.openarena.Openarena",
		NULL
	};

	static const char *google_apps[] = {
		"com.google.Chrome",
		"com.endlessm.translation",
		NULL
	};

	/* only block apps based on personality if they are not installed */
	if (gs_app_is_installed (app))
		return FALSE;

	return ((g_strcmp0 (priv->personality, "es_GT") == 0) &&
	        g_strv_contains (violent_apps, app_name)) ||
	       ((g_strcmp0 (priv->personality, "zh_CN") == 0) &&
	        (g_strv_contains (google_apps, app_name) ||
	         g_str_has_prefix (app_name, "com.endlessm.encyclopedia")));
}

static gboolean
app_is_banned_coding_app (GsPlugin *plugin, GsApp *app)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);

	/* the coding chatbox is in the core ostree, so unlike the
	   personality-based blocking, we block even if it is installed */

	return (!priv->is_coding_enabled &&
	        (g_strcmp0 (gs_flatpak_app_get_ref_name (app), "com.endlessm.Coding.Chatbox") == 0));
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

static gboolean
gs_plugin_eos_blacklist_if_needed (GsPlugin *plugin, GsApp *app)
{
	gboolean blacklist_app = FALSE;
	const char *id = gs_app_get_id (app);

	if (gs_app_get_kind (app) != AS_APP_KIND_DESKTOP &&
	    gs_app_has_quirk (app, AS_APP_QUIRK_COMPULSORY) &&
	    !gs_app_has_quirk (app, AS_APP_QUIRK_IS_PROXY)) {
		g_debug ("Blacklisting '%s': it's a compulsory, non-desktop app",
			 gs_app_get_unique_id (app));
		blacklist_app = TRUE;
	} else if (g_str_has_prefix (id, "eos-link-")) {
		g_debug ("Blacklisting '%s': app is an eos-link",
			 gs_app_get_unique_id (app));
		blacklist_app = TRUE;
	} else if (gs_app_has_quirk (app, AS_APP_QUIRK_COMPULSORY) &&
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
	} else if (app_is_banned_coding_app (plugin, app)) {
		g_debug ("Blacklisting '%s': it's a banned coding app",
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

static const char*
get_desktop_file_id (GsApp *app)
{
	const char *desktop_file_id =
		gs_app_get_metadata_item (app, METADATA_SYS_DESKTOP_FILE);

	if (!desktop_file_id)
		desktop_file_id = gs_app_get_id (app);

	g_assert (desktop_file_id != NULL);
	return desktop_file_id;
}

static void
gs_plugin_eos_update_app_shortcuts_info (GsPlugin *plugin,
					 GsApp *app)
{
	GsPluginData *priv = NULL;
	const char *desktop_file_id = NULL;
	g_autofree char *kde_desktop_file_id = NULL;

	if (!gs_app_is_installed (app)) {
		gs_app_remove_quirk (app, AS_APP_QUIRK_HAS_SHORTCUT);
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
		gs_app_add_quirk (app, AS_APP_QUIRK_HAS_SHORTCUT);
	else
		gs_app_remove_quirk (app, AS_APP_QUIRK_HAS_SHORTCUT);
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

	/* we only allow to remove flatpak apps */
	gs_app_add_quirk (app, AS_APP_QUIRK_COMPULSORY);

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

gboolean
gs_plugin_refine (GsPlugin		*plugin,
		  GsAppList		*list,
		  GsPluginRefineFlags	flags,
		  GCancellable		*cancellable,
		  GError		**error)
{
	for (guint i = 0; i < gs_app_list_length (list); ++i) {
		GsApp *app = gs_app_list_index (list, i);

		gs_plugin_eos_refine_core_app (app);

		/* if we don't know yet the state of an app then we shouldn't
		 * do any further operations on it */
		if (gs_app_get_state (app) == AS_APP_STATE_UNKNOWN)
			continue;

		if (gs_plugin_eos_blacklist_if_needed (plugin, app))
			continue;

		if (gs_app_get_kind (app) != AS_APP_KIND_DESKTOP)
			continue;

		gs_plugin_eos_update_app_shortcuts_info (plugin, app);

		if (gs_plugin_eos_blacklist_kapp_if_needed (plugin, app))
			continue;

		if (gs_plugin_eos_blacklist_app_for_remote_if_needed (plugin, app))
			continue;

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
	const char *desktop_file_id = get_desktop_file_id (app);
	g_autoptr(GDesktopAppInfo) app_info =
		gs_utils_get_desktop_app_info (desktop_file_id);
	const char *shortcut_id = g_app_info_get_id (G_APP_INFO (app_info));

	g_dbus_connection_call_sync (priv->session_bus,
				     "org.gnome.Shell",
				     "/org/gnome/Shell",
				     "org.gnome.Shell.AppStore",
				     "RemoveApplication",
				     g_variant_new ("(s)", shortcut_id),
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
	const char *desktop_file_id = get_desktop_file_id (app);
	g_autoptr(GDesktopAppInfo) app_info =
		gs_utils_get_desktop_app_info (desktop_file_id);
	const char *shortcut_id = g_app_info_get_id (G_APP_INFO (app_info));

	/* Look up the app in our replacement list to see if we
	 * can replace and existing shortcut, and if so, do that
	 * instead */
	const char *shortcut_id_to_replace = g_hash_table_lookup (priv->replacement_app_lookup,
								  desktop_file_id);


	if (shortcut_id_to_replace)
		shell_replace_app (priv->session_bus,
				   shortcut_id_to_replace,
				   shortcut_id,
				   cancellable,
				   &error);
	else
		shell_add_app_if_not_visible (priv->session_bus,
					      shortcut_id,
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
	gs_app_add_quirk (app, AS_APP_QUIRK_HAS_SHORTCUT);
	return add_app_to_shell (plugin, app, cancellable, error);
}

gboolean
gs_plugin_remove_shortcut (GsPlugin	*plugin,
			   GsApp	*app,
			   GCancellable	*cancellable,
			   GError	**error)
{
	gs_app_remove_quirk (app, AS_APP_QUIRK_HAS_SHORTCUT);
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
	const char *desktop_file_id = get_desktop_file_id (app);
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
	if (gs_app_has_quirk (app, AS_APP_QUIRK_COMPULSORY) &&
	    !app_is_flatpak (app)) {
		return gs_plugin_app_launch (plugin, app, error);
	}

	/* for apps that have a special desktop file (e.g. Google Chrome) */
	if (gs_app_get_metadata_item (app, METADATA_SYS_DESKTOP_FILE))
		return launch_with_sys_desktop_file (app, error);

	return TRUE;
}

static GsApp *
gs_plugin_eos_create_updates_proxy_app (GsPlugin *plugin)
{
	const char *id = EOS_PROXY_APP_PREFIX ".EOSUpdatesProxy";
	GsApp *proxy = gs_app_new (id);
	g_autoptr(AsIcon) icon;

	gs_app_set_scope (proxy, AS_APP_SCOPE_SYSTEM);
	gs_app_set_kind (proxy, AS_APP_KIND_RUNTIME);
	/* TRANSLATORS: this is the name of the Endless Platform app */
	gs_app_set_name (proxy, GS_APP_QUALITY_NORMAL,
			 _("Endless Platform"));
	/* TRANSLATORS: this is the summary of the Endless Platform app */
	gs_app_set_summary (proxy, GS_APP_QUALITY_NORMAL,
			    _("Framework for applications"));
	gs_app_set_state (proxy, AS_APP_STATE_UPDATABLE_LIVE);
	gs_app_add_quirk (proxy, AS_APP_QUIRK_IS_PROXY);
	gs_app_set_management_plugin (proxy, gs_plugin_get_name (plugin));

	icon = as_icon_new ();
	as_icon_set_kind (icon, AS_ICON_KIND_STOCK);
	as_icon_set_name (icon, "system-run-symbolic");
	gs_app_add_icon (proxy, icon);

	return proxy;
}

static gboolean
add_updates (GsPlugin *plugin,
	     GsAppList *list,
	     GCancellable *cancellable,
	     GError **error)
{
	g_autoptr(GsApp) updates_proxy_app = gs_plugin_eos_create_updates_proxy_app (plugin);
	g_autoptr(GSList) proxied_updates = NULL;
	const char *proxied_apps[] = {"com.endlessm.Platform",
				      "com.endlessm.apps.Platform",
				      "com.endlessm.EknServices.desktop",
				      "com.endlessm.EknServices2.desktop",
				      "com.endlessm.quote_of_the_day.en.desktop",
				      "com.endlessm.word_of_the_day.en.desktop",
				      NULL};

	for (guint i = 0; i < gs_app_list_length (list); ++i) {
		GsApp *app = gs_app_list_index (list, i);
		const char *id = gs_app_get_id (app);

		if (!g_strv_contains (proxied_apps, id) ||
		    gs_app_get_scope (updates_proxy_app) != gs_app_get_scope (app))
			continue;

		proxied_updates = g_slist_prepend (proxied_updates, app);
	}

	if (!proxied_updates)
		return TRUE;

	for (GSList *iter = proxied_updates; iter; iter = g_slist_next (iter)) {
		GsApp *app = GS_APP (iter->data);
		gs_app_add_related (updates_proxy_app, app);
		/* remove proxied apps from updates list since they will be
		 * updated from the proxy app */
		gs_app_list_remove (list, app);
	}
	gs_app_list_add (list, updates_proxy_app);

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
	 * but in case the user has changed the "download-updates" setting then
	 * this will still work correctly */
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
	const gchar *popular_apps[] = {"com.google.Chrome.desktop",
				       "com.spotify.Client.desktop",
				       "com.transmissionbt.Transmission.desktop",
				       "com.valvesoftware.Steam.desktop",
				       "libreoffice-calc.desktop",
				       "libreoffice-impress.desktop",
				       "libreoffice-writer.desktop",
				       "net.gcompris.Gcompris.desktop",
				       "net.minetest.Minetest.desktop",
				       "net.sourceforge.Audacity.desktop",
				       "org.debian.alioth.tux4kids.Tuxmath.desktop",
				       "org.gimp.Gimp.desktop",
				       "org.inkscape.Inkscape.desktop",
				       "org.mozilla.Firefox.desktop",
				       "org.tuxpaint.Tuxpaint.desktop",
				       "org.videolan.VLC.desktop",
				       "simple-scan.desktop",
				       NULL};

	ptask = as_profile_start_literal (gs_plugin_get_profile (plugin),
					  "eos::add-popular");

	new_list = gs_app_list_new ();

	/* add the hardcoded list of popular apps */
	for (guint i = 0; popular_apps[i] != NULL; ++i) {
		g_autoptr(GsApp) app = gs_app_new (popular_apps[i]);
		gs_app_add_quirk (app, AS_APP_QUIRK_MATCH_ANY_PREFIX);
		gs_app_list_add (new_list, app);
	}

	/* get all the popular apps that are Endless' ones */
	for (guint i = 0; i < gs_app_list_length (list); ++i) {
	        GsApp *app = gs_app_list_index (list, i);
		if (g_str_has_prefix (gs_app_get_id (app), "com.endlessm."))
			gs_app_list_add (new_list, app);
	}

	/* replace the list of popular apps so far by ours */
	gs_app_list_remove_all (list);
	gs_app_list_add_list (list, new_list);

	return TRUE;
}
