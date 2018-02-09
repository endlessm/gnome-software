/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2016-2017 Endless Mobile, Inc.
 * Copyright (C) 2015-2016 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#include "config.h"

#include <glib/gi18n.h>

#include "gs-desktop-common.h"

/* Multimedia: AudioVideo + Graphics */
static const GsDesktopMap map_multimedia[] = {
	{ "all",		NC_("Menu of Audio & Video", "All"),
					{ "AudioVideo",
					  "Graphics",
					  NULL } },
	{ "featured",		NC_("Menu of Audio & Video", "Featured"),
					{ "AudioVideo::Featured",
					  "Graphics::Featured",
					  NULL} },
	{ "creation-editing",	NC_("Menu of Audio & Video", "Audio Creation & Editing"),
					{ "AudioVideo::AudioVideoEditing",
					  "AudioVideo::Midi",
					  "AudioVideo::DiscBurning",
					  "AudioVideo::Sequencer",
					  NULL} },
	{ "music-players",	NC_("Menu of Audio & Video", "Music Players"),
					{ "AudioVideo::Music",
					  "AudioVideo::Player",
					  NULL} },
	{ "3d",			NC_("Menu of Graphics", "3D Graphics"),
					{ "Graphics::3DGraphics",
					  NULL} },
	{ "photography",	NC_("Menu of Graphics", "Photography"),
					{ "Graphics::Photography",
					  NULL} },
	{ "scanning",		NC_("Menu of Graphics", "Scanning"),
					{ "Graphics::Scanning",
					  NULL} },
	{ "vector",		NC_("Menu of Graphics", "Vector Graphics"),
					{ "Graphics::VectorGraphics",
					  NULL} },
	{ "viewers",		NC_("Menu of Graphics", "Viewers"),
					{ "Graphics::Viewer",
					  NULL} },
	{ NULL }
};

/* Development */
static const GsDesktopMap map_developertools[] = {
	{ "all",		NC_("Menu of Developer Tools", "All"),
					{ "Development",
					  NULL } },
	{ "featured",		NC_("Menu of Developer Tools", "Featured"),
					{ "Development::Featured",
					  NULL} },
	{ "debuggers",		NC_("Menu of Developer Tools", "Debuggers"),
					{ "Development::Debugger",
					  NULL} },
	{ "ide",		NC_("Menu of Developer Tools", "IDEs"),
					{ "Development::IDE",
					  "Development::GUIDesigner",
					  NULL} },
	{ NULL }
};

/* Education & Science */
static const GsDesktopMap map_education_science[] = {
	{ "all",		NC_("Menu of Education & Science", "All"),
					{ "Education",
					  "Science",
					  NULL } },
	{ "featured",		NC_("Menu of Education & Science", "Featured"),
					{ "Education::Featured",
					  "Science::Featured",
					  NULL} },
	{ "artificial-intelligence", NC_("Menu of Education & Science", "Artificial Intelligence"),
					{ "Science::ArtificialIntelligence",
					  NULL} },
	{ "astronomy",		NC_("Menu of Education & Science", "Astronomy"),
					{ "Education::Astronomy",
					  "Science::Astronomy",
					  NULL} },
	{ "chemistry",		NC_("Menu of Education & Science", "Chemistry"),
					{ "Education::Chemistry",
					  "Science::Chemistry",
					  NULL} },
	{ "languages",		NC_("Menu of Education & Science", "Languages"),
					{ "Education::Languages",
					  "Education::Literature",
					  NULL} },
	{ "math",		NC_("Menu of Education & Science", "Math"),
					{ "Education::Math",
					  "Education::NumericalAnalysis",
					  "Science::Math",
					  "Science::Physics",
					  "Science::NumericalAnalysis",
					  NULL} },
	{ "robotics",		NC_("Menu of Education & Science", "Robotics"),
					{ "Science::Robotics",
					  NULL} },

	{ NULL }
};

/* Games */
static const GsDesktopMap map_games[] = {
	{ "all",		NC_("Menu of Games", "All"),
					{ "Game",
					  NULL } },
	{ "featured",		NC_("Menu of Games", "Featured"),
					{ "Game::Featured",
					  NULL} },
	{ "action",		NC_("Menu of Games", "Action"),
					{ "Game::ActionGame",
					  NULL} },
	{ "adventure",		NC_("Menu of Games", "Adventure"),
					{ "Game::AdventureGame",
					  NULL} },
	{ "arcade",		NC_("Menu of Games", "Arcade"),
					{ "Game::ArcadeGame",
					  NULL} },
	{ "blocks",		NC_("Menu of Games", "Blocks"),
					{ "Game::BlocksGame",
					  NULL} },
	{ "board",		NC_("Menu of Games", "Board"),
					{ "Game::BoardGame",
					  NULL} },
	{ "card",		NC_("Menu of Games", "Card"),
					{ "Game::CardGame",
					  NULL} },
	{ "emulator",		NC_("Menu of Games", "Emulators"),
					{ "Game::Emulator",
					  NULL} },
	{ "kids",		NC_("Menu of Games", "Kids"),
					{ "Game::KidsGame",
					  NULL} },
	{ "logic",		NC_("Menu of Games", "Logic"),
					{ "Game::LogicGame",
					  NULL} },
	{ "role-playing",	NC_("Menu of Games", "Role Playing"),
					{ "Game::RolePlaying",
					  NULL} },
	{ "sports",		NC_("Menu of Games", "Sports"),
					{ "Game::SportsGame",
					  "Game::Simulation",
					  NULL} },
	{ "strategy",		NC_("Menu of Games", "Strategy"),
					{ "Game::StrategyGame",
					  NULL} },
	{ NULL }
};

/* Office */
static const GsDesktopMap map_productivity[] = {
	{ "all",		NC_("Menu of Productivity", "All"),
					{ "Office",
					  NULL } },
	{ "featured",		NC_("Menu of Productivity", "Featured"),
					{ "Office::Featured",
					  NULL} },
	{ "calendar",		NC_("Menu of Productivity", "Calendar"),
					{ "Office::Calendar",
					  "Office::ProjectManagement",
					  NULL} },
	{ "database",		NC_("Menu of Productivity", "Database"),
					{ "Office::Database",
					  NULL} },
	{ "finance",		NC_("Menu of Productivity", "Finance"),
					{ "Office::Finance",
					  "Office::Spreadsheet",
					  NULL} },
	{ "word-processor",	NC_("Menu of Productivity", "Word Processor"),
					{ "Office::WordProcessor",
					  "Office::Dictionary",
					  NULL} },
	{ NULL }
};

/* Utility: Utility + Network + Development */
static const GsDesktopMap map_utilities[] = {
	{ "all",		NC_("Menu of Utility", "All"),
					{ "Utility",
					  "Network",
					  "Settings",
					  "System",
					  NULL } },
	{ "featured",		NC_("Menu of Utility", "Featured"),
					{ "Utility::Featured",
					  "Network::Featured",
					  NULL} },
	{ "fonts",		NC_("Menu of Add-ons", "Fonts"),
					{ "Addon::Font",
					  NULL} },
	{ "chat",		NC_("Menu of Communication", "Chat"),
					{ "Network::Chat",
					  "Network::IRCClient",
					  "Network::Telephony",
					  "Network::VideoConference",
					  "Network::Email",
					  NULL} },
	{ "codecs",		NC_("Menu of Add-ons", "Codecs"),
					{ "Addon::Codec",
					  NULL} },
	{ "input-sources",	NC_("Menu of Add-ons", "Input Sources"),
					{ "Addon::InputSource",
					  NULL} },
	{ "language-packs",	NC_("Menu of Add-ons", "Language Packs"),
					{ "Addon::LanguagePack",
					  NULL} },
	{ "shell-extensions",	NC_("Menu of Add-ons", "Shell Extensions"),
					{ "Addon::ShellExtension",
					  NULL} },
	{ "localization",	NC_("Menu of Add-ons", "Localization"),
					{ "Addon::Localization",
					  NULL} },
	{ "drivers",		NC_("Menu of Add-ons", "Hardware Drivers"),
					{ "Addon::Driver",
					  NULL} },
	{ "text-editors",	NC_("Menu of Utility", "Text Editors"),
					{ "Utility::TextEditor",
					  NULL} },
	{ "web-browsers",	NC_("Menu of Communication & News", "Web Browsers"),
					{ "Network::WebBrowser",
					  NULL} },
	{ "news",		NC_("Menu of Communication & News", "News"),
					{ "Network::Feed",
					  "Network::News",
					  NULL} },
	{ "settings",		NC_("Menu of Settings", "Settings"),
					{ "Settings::Accessibility",
					  "Settings::DesktopSettings",
					  "Settings::HardwareSettings",
					  "Settings::Printing",
					  "Settings::PackageManager",
					  "Settings::Printing",
					  "Settings::Security",
					  NULL} },
	{ "system",		NC_("Menu of System", "System"),
					{ "System::Emulator",
					  "System::FileManager",
					  "System::FileSystem",
					  "System::FileTools",
					  "System::TerminalEmulator",
					  "System::Security",
					  NULL} },
	{ NULL }
};

/* Reference: Reference + Network::Feed + Network::News */
static const GsDesktopMap map_reference[] = {
	{ "all",		NC_("Menu of Reference", "All"),
					{ "Reference",
					  NULL } },
	{ "featured",		NC_("Menu of Reference", "Featured"),
					{ "Reference::Featured",
					  NULL} },
	{ "art",		NC_("Menu of Reference", "Art"),
					{ "Reference::Art",
					  NULL} },
	{ "biography",		NC_("Menu of Reference", "Biography"),
					{ "Reference::Biography",
					  NULL} },
	{ "comics",		NC_("Menu of Reference", "Comics"),
					{ "Reference::Comics",
					  NULL} },
	{ "feed",		NC_("Menu of Reference", "Feed"),
					{ "Reference::Feed",
					  NULL} },
	{ "fiction",		NC_("Menu of Reference", "Fiction"),
					{ "Reference::Fiction",
					  NULL} },
	{ "health",		NC_("Menu of Reference", "Health"),
					{ "Reference::Health",
					  NULL} },
	{ "history",		NC_("Menu of Reference", "History"),
					{ "Reference::History",
					  NULL} },
	{ "lifestyle",		NC_("Menu of Reference", "Lifestyle"),
					{ "Reference::Lifestyle",
					  NULL} },
	{ "news",		NC_("Menu of Reference", "News"),
					{ "Reference::News",
					  NULL} },
	{ "politics",		NC_("Menu of Reference", "Politics"),
					{ "Reference::Politics",
					  NULL} },
	{ "sports",		NC_("Menu of Reference", "Sports"),
					{ "Reference::Sports",
					  NULL} },
	{ NULL }
};

/* USB */
static const GsDesktopMap map_usb[] = {
	{ "all",		NC_("Menu of Reference", "All"),
					{ "USB",
					  NULL } },
	{ NULL }
};

/* main categories */
/* Please keep category name and subcategory context synchronized!!! */
static const GsDesktopData msdata[] = {
	/* TRANSLATORS: this is the menu spec main category for Learning */
	{ "education-science",		map_education_science,	N_("Learning"),
				"accessories-dictionary-symbolic", "#e34535", 100 },
	/* TRANSLATORS: this is the menu spec main category for Game */
	{ "games",		map_games,		N_("Games"),
				"applications-games-symbolic", "#5cae4b", 70 },
	/* TRANSLATORS: this is the menu spec main category for Multimedia */
	{ "multimedia",		map_multimedia,		N_("Multimedia"),
				"applications-multimedia-symbolic", "#07afa7", 60 },
	/* TRANSLATORS: this is the menu spec main category for Work */
	{ "productivity",	map_productivity,	N_("Work"),
				"x-office-document-symbolic", "#0098d2", 20 },
	/* TRANSLATORS: this is the menu spec main category for Reference */
	{ "reference",		map_reference,		N_("Reference & News"),
				"gs-category-newspaper-symbolic", "#ffcd34", 80 },
	/* TRANSLATORS: this is the menu spec main category for Utilities */
	{ "utilities",		map_utilities,		N_("Utilities"),
				"applications-utilities-symbolic", "#3841c3", 10 },
	/* TRANSLATORS: this is the menu spec main category for Dev Tools; it
	 * should be a relatively short label; as an example, in Portuguese and
	 * Spanish the direct translation of "Programming" (noun) is used */
	{ "developer-tools",	map_developertools,	N_("Dev Tools"),
				"preferences-other-symbolic", "#7b3eb5", 5 },
	{ "usb",		map_usb,		N_("USB"),
				"media-removable-symbolic", "#ccff00", 5 },
	{ NULL }
};

const GsDesktopData *
gs_desktop_get_data (void)
{
	return msdata;
}
