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
#include <math.h>
#include <sys/types.h>
#include <sys/xattr.h>

#define ENDLESS_ID_PREFIX "com.endlessm."

#define EOS_IMAGE_VERSION_XATTR "user.eos-image-version"
#define EOS_IMAGE_VERSION_PATH "/sysroot"
#define EOS_IMAGE_VERSION_ALT_PATH "/"

/*
 * SECTION:
 *
 * Plugin to blacklist certain apps on Endless OS, depending on the OS’s locale,
 * version, or architecture.
 */

struct GsPluginData
{
	char *personality;
	gboolean eos_arch_is_arm;
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

gboolean
gs_plugin_setup (GsPlugin *plugin,
		 GCancellable *cancellable,
		 GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);

	priv->eos_arch_is_arm = g_strcmp0 (flatpak_get_default_arch (), "arm") == 0;

	{
		g_autoptr(GError) local_error = NULL;
		priv->personality = get_personality (&local_error);

		if (local_error != NULL)
			g_warning ("No system personality could be retrieved! %s", local_error->message);
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
}

/* Copy of the implementation of gs_flatpak_app_get_ref_name(). */
static const gchar *
app_get_flatpak_ref_name (GsApp *app)
{
	return gs_app_get_metadata_item (app, "flatpak::RefName");
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
	return g_str_has_suffix (app_get_flatpak_ref_name (app),
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

	const char *hostname = NULL;
	const char *app_name = NULL;

	if (gs_app_get_scope (app) != AS_APP_SCOPE_SYSTEM ||
	    gs_app_is_installed (app))
		return FALSE;

	hostname = gs_app_get_origin_hostname (app);
	if (hostname == NULL)
		return FALSE;

	app_name = app_get_flatpak_ref_name (app);
	if (app_name == NULL)
		return FALSE;

	/* We need to check for the app's origin, otherwise we'd be
	 * blacklisting matching apps coming from any repo */
	if (g_strcmp0 (hostname, "sdk.gnome.org") == 0 ||
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
	const char *app_name = app_get_flatpak_ref_name (app);

	static const char *adult_apps[] = {
		"com.katawa_shoujo.KatawaShoujo",
		"com.scoutshonour.dtipbijays",
		NULL
	};

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
	        g_strv_contains (adult_apps, app_name) ||
	        g_strv_contains (violent_apps, app_name));
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
	} else if (app_is_banned_for_personality (plugin, app)) {
		g_debug ("Blacklisting '%s': app is banned for personality",
			 gs_app_get_unique_id (app));
		blacklist_app = TRUE;
	} else if (app_is_evergreen (app)) {
		g_debug ("Blacklisting '%s': it's an evergreen app",
			 gs_app_get_unique_id (app));
		blacklist_app = TRUE;
	}

	if (blacklist_app)
		gs_app_add_category (app, "Blacklisted");

	return blacklist_app;
}

gboolean
gs_plugin_refine_app (GsPlugin *plugin,
		      GsApp *app,
		      GsPluginRefineFlags flags,
		      GCancellable *cancellable,
		      GError **error)
{
	/* if we don't know yet the state of an app then we shouldn't
	 * do any further operations on it */
	if (gs_app_get_state (app) == AS_APP_STATE_UNKNOWN)
		return TRUE;

	if (gs_plugin_eos_blacklist_if_needed (plugin, app))
		return TRUE;

	if (gs_app_get_kind (app) != AS_APP_KIND_DESKTOP)
		return TRUE;

	if (gs_plugin_eos_blacklist_kapp_if_needed (plugin, app))
		return TRUE;

	if (gs_plugin_eos_blacklist_app_for_remote_if_needed (plugin, app))
		return TRUE;

	gs_plugin_eos_remove_blacklist_from_usb_if_needed (plugin, app);

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
