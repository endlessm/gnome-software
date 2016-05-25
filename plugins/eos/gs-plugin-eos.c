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

#include <gnome-software.h>
#include <glib/gi18n.h>
#include <gs-plugin.h>

#include "gs-plugin-eos.h"

/*
 * SECTION:
 * Plugin to improve GNOME Software integration in the EOS desktop.
 *
 * This plugin runs entirely in the main thread.
 */

struct _GsPluginEos
{
	GsPlugin parent;
};

G_DEFINE_TYPE (GsPluginEos, gs_plugin_eos, GS_TYPE_PLUGIN)

static void
gs_plugin_eos_init (GsPluginEos *self)
{
	/* let the flatpak plugin run first so we deal with the apps
	 * in a more complete/refined state */
	gs_plugin_add_rule (GS_PLUGIN (self), GS_PLUGIN_RULE_RUN_AFTER, "flatpak");
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

	gs_app_set_management_plugin (app, plugin);
}

static void
refine_core_app (GsApp *app)
{
	if (app_is_flatpak (app) ||
	    (gs_app_get_scope (app) == AS_COMPONENT_SCOPE_UNKNOWN))
		return;

	if (gs_app_get_kind (app) == AS_COMPONENT_KIND_OPERATING_SYSTEM)
		return;

	/* Hide non-installed apt packages, as they can’t actually be installed.
	 * The installed ones are pre-installed in the image, and can’t be
	 * removed. We only allow flatpaks to be removed. */
	if (!gs_app_is_installed (app)) {
		gs_app_add_quirk (app, GS_APP_QUIRK_HIDE_EVERYWHERE);
	} else {
		gs_app_add_quirk (app, GS_APP_QUIRK_COMPULSORY);
	}
}

static void
gs_plugin_eos_refine_async (GsPlugin            *plugin,
                            GsAppList           *list,
                            GsPluginRefineFlags  flags,
                            GCancellable        *cancellable,
                            GAsyncReadyCallback  callback,
                            gpointer             user_data)
{
	g_autoptr(GTask) task = NULL;

	task = g_task_new (plugin, cancellable, callback, user_data);
	g_task_set_source_tag (task, gs_plugin_eos_refine_async);

	for (guint i = 0; i < gs_app_list_length (list); i++) {
		GsApp *app = gs_app_list_index (list, i);

		refine_core_app (app);
	}

	g_task_return_boolean (task, TRUE);
}

static gboolean
gs_plugin_eos_refine_finish (GsPlugin      *plugin,
                             GAsyncResult  *result,
                             GError       **error)
{
	return g_task_propagate_boolean (G_TASK (result), error);
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

	return TRUE;
}

static void
gs_plugin_eos_class_init (GsPluginEosClass *klass)
{
	GsPluginClass *plugin_class = GS_PLUGIN_CLASS (klass);

	plugin_class->refine_async = gs_plugin_eos_refine_async;
	plugin_class->refine_finish = gs_plugin_eos_refine_finish;
}

GType
gs_plugin_query_type (void)
{
	return GS_TYPE_PLUGIN_EOS;
}
