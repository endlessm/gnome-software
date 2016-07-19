/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2016 Endless Mobile, Inc.
 * Copyright (C) 2015-2016 Richard Hughes <richard@hughsie.com>
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

#include "config.h"

#include <glib/gi18n.h>

#include "gs-desktop-common.h"

/* Multimedia: AudioVideo + Graphics */
static const GsDesktopMap map_multimedia[] = {
	{ "all",		NC_("Menu of AudioVideo", "All"),
					{ "AudioVideo",
					  "Graphics",
					  NULL } },
	{ "featured",		NC_("Menu of AudioVideo", "Featured"),
					{ "AudioVideo::Featured",
					  "Graphics::Featured",
					  NULL} },
	{ "creation-editing",	NC_("Menu of AudioVideo", "Audio Creation & Editing"),
					{ "AudioVideo::AudioVideoEditing",
					  "AudioVideo::Midi",
					  "AudioVideo::DiscBurning",
					  "AudioVideo::Sequencer",
					  NULL} },
	{ "music-players",	NC_("Menu of AudioVideo", "Music Players"),
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

/* Education & Science */
static const GsDesktopMap map_education_science[] = {
	{ "all",		NC_("Menu of Education and Science", "All"),
					{ "Education",
					  "Science",
					  NULL } },
	{ "featured",		NC_("Menu of Education and Science", "Featured"),
					{ "Education::Featured",
					  "Science::Featured",
					  NULL} },
	{ "artificial-intelligence", NC_("Menu of Education and Science", "Artificial Intelligence"),
					{ "Science::ArtificialIntelligence",
					  NULL} },
	{ "astronomy",		NC_("Menu of Education and Science", "Astronomy"),
					{ "Education::Astronomy",
					  "Science::Astronomy",
					  NULL} },
	{ "chemistry",		NC_("Menu of Education and Science", "Chemistry"),
					{ "Education::Chemistry",
					  "Science::Chemistry",
					  NULL} },
	{ "languages",		NC_("Menu of Education and Science", "Languages"),
					{ "Education::Languages",
					  "Education::Literature",
					  NULL} },
	{ "math",		NC_("Menu of Education and Science", "Math"),
					{ "Education::Math",
					  "Education::NumericalAnalysis",
					  "Science::Math",
					  "Science::Physics",
					  "Science::NumericalAnalysis",
					  NULL} },
	{ "robotics",		NC_("Menu of Education and Science", "Robotics"),
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
	{ "all",		NC_("Menu of Office", "All"),
					{ "Office",
					  NULL } },
	{ "featured",		NC_("Menu of Office", "Featured"),
					{ "Office::Featured",
					  NULL} },
	{ "calendar",		NC_("Menu of Office", "Calendar"),
					{ "Office::Calendar",
					  "Office::ProjectManagement",
					  NULL} },
	{ "database",		NC_("Menu of Office", "Database"),
					{ "Office::Database",
					  NULL} },
	{ "finance",		NC_("Menu of Office", "Finance"),
					{ "Office::Finance",
					  "Office::Spreadsheet",
					  NULL} },
	{ "word-processor",	NC_("Menu of Office", "Word Processor"),
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
					  "Development",
					  NULL } },
	{ "featured",		NC_("Menu of Utility", "Featured"),
					{ "Utility::Featured",
					  "Network::Featured",
					  "Development::Featured",
					  NULL} },
	{ "chat",		NC_("Menu of Communication", "Chat"),
					{ "Network::Chat",
					  "Network::IRCClient",
					  "Network::Telephony",
					  "Network::VideoConference",
					  "Network::Email",
					  NULL} },
	{ "codecs",		NC_("Menu of Addons", "Codecs"),
					{ "Addons::Codecs",
					  NULL} },
	{ "debuggers",		NC_("Menu of Development", "Debuggers"),
					{ "Development:Debugger",
					  NULL} },
	{ "drivers",		NC_("Menu of Addons", "Hardware Drivers"),
					{ "Addons::Drivers",
					  NULL} },
	{ "fonts",		NC_("Menu of Addons", "Fonts"),
					{ "Addons::Fonts",
					  NULL} },
	{ "ide",		NC_("Menu of Development", "IDEs"),
					{ "Development::IDE",
					  "Development::GUIDesigner",
					  NULL} },
	{ "input-sources",	NC_("Menu of Addons", "Input Sources"),
					{ "Addons::InputSources",
					  NULL} },
	{ "language-packs",	NC_("Menu of Addons", "Language Packs"),
					{ "Addons::LanguagePacks",
					  NULL} },
	{ "localization",	NC_("Menu of Addons", "Localization"),
					{ "Addons::Localization",
					  NULL} },
	{ "shell-extensions",	NC_("Menu of Addons", "Shell Extensions"),
					{ "Addons::ShellExtensions",
					  NULL} },
	{ "text-editors",	NC_("Menu of Utility", "Text Editors"),
					{ "Utility::TextEditor",
					  NULL} },
	{ "web-browsers",	NC_("Menu of Communication", "Web Browsers"),
					{ "Network::WebBrowser",
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
	{ "art",		NC_("Menu of Art", "Art"),
					{ "Reference::Art",
					  NULL} },
	{ "biography",		NC_("Menu of Reference", "Biography"),
					{ "Reference::Biography",
					  NULL} },
	{ "comics",		NC_("Menu of Reference", "Comics"),
					{ "Reference::Comics",
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
	{ "news",		NC_("Menu of Communication", "News"),
					{ "Network::Feed",
					  "Network::News",
					  NULL} },
	{ "politics",		NC_("Menu of Reference", "Politics"),
					{ "Reference::Politics",
					  NULL} },
	{ "sports",		NC_("Menu of Reference", "Sports"),
					{ "Reference::Sports",
					  NULL} },
	{ NULL }
};

/* main categories */
static const GsDesktopData msdata[] = {
	/* TRANSLATORS: this is the menu spec main category for Learning */
	{ "education-science",		map_education_science,	N_("Learning"),
				"system-help-symbolic", "#29cc5d", 30 },
	/* TRANSLATORS: this is the menu spec main category for Game */
	{ "games",		map_games,		N_("Games"),
				"applications-games-symbolic", "#c4a000", 70 },
	/* TRANSLATORS: this is the menu spec main category for Multimedia */
	{ "multimedia",		map_multimedia,		N_("Multimedia"),
				"applications-graphics-symbolic", "#75507b", 60 },
	/* TRANSLATORS: this is the menu spec main category for Work */
	{ "productivity",	map_productivity,	N_("Work"),
				"x-office-document-symbolic", "#0098d2", 80 },
	/* TRANSLATORS: this is the menu spec main category for Reference */
	{ "reference",		map_reference,		N_("Reference & News"),
				"view-dual-symbolic", "#ac5500", 0 },
	/* TRANSLATORS: this is the menu spec main category for Utilities */
	{ "utilities",		map_utilities,		N_("Utilities"),
				"applications-utilities-symbolic", "#d3d7c7", 10 },
	{ NULL }
};

const GsDesktopData *
gs_desktop_get_data (void)
{
	return msdata;
}
