/*
 * gs-discovery-feed-content-provider.c - Implementation of an EOS
 * Discovery Feed Content Provider
 *
 * Copyright (c) 2017 Endless Mobile Inc.
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

#include <gio/gio.h>
#include <string.h>
#include <glib/gi18n.h>

#include "gs-app-list-private.h"
#include "gs-plugin-loader-sync.h"
#include "gs-utils.h"

#include "gs-discovery-feed-content-provider-generated.h"
#include "gs-discovery-feed-content-provider.h"

#define MAX_RESULTS	20
#define MAX_INSTALLABLE_APPS 3

typedef struct {
	GsDiscoveryFeedContentProvider *provider;
	GDBusMethodInvocation *invocation;
} PendingSearch;

struct _GsDiscoveryFeedContentProvider {
	GObject parent;

	GsDiscoveryFeedInstallableApps *skeleton;
	GsPluginLoader *plugin_loader;
	GCancellable *cancellable;
};

G_DEFINE_TYPE (GsDiscoveryFeedContentProvider, gs_discovery_feed_content_provider, G_TYPE_OBJECT)

static void
pending_search_free (PendingSearch *search)
{
	g_object_unref (search->invocation);
	g_slice_free (PendingSearch, search);
}

static gchar *
get_app_thumbnail_cached_filename (GsApp *app)
{
	const gchar *popular_background_url = gs_app_get_metadata_item (app, "GnomeSoftware::popular-background");
	g_autofree char *tile_cache_hash = NULL;
	g_autofree char *cache_identifier = NULL;
	g_autofree char *url_basename = NULL;
	g_autofree char *cache_filename = NULL;

	url_basename = g_path_get_basename (popular_background_url);
	tile_cache_hash = g_compute_checksum_for_string (G_CHECKSUM_SHA256,
	                                                 popular_background_url,
	                                                 -1);
	cache_identifier = g_strdup_printf ("%s-%s", tile_cache_hash, url_basename);
	cache_filename = gs_utils_get_cache_filename ("eos-popular-app-thumbnails",
	                                              cache_identifier,
	                                              GS_UTILS_CACHE_FLAG_NONE,
	                                              NULL);

	/* Check to see if the file exists in the cache at the time we called this
	 * function. If it does, then we can use this app in the results
	 * since it will have a thumbnail */
	if (g_file_test (cache_filename, G_FILE_TEST_EXISTS)) {
		g_debug ("Hit cache for thumbnail when loading discovery feed %s: %s", popular_background_url, cache_filename);
		return g_steal_pointer (&cache_filename);
	}

	return NULL;
}

static gboolean
filter_for_discovery_feed_apps (GsApp *app, gpointer user_data)
{
	g_autofree gchar *app_thumbnail_filename = NULL;

	if (gs_app_get_state (app) != AS_APP_STATE_AVAILABLE)
		return FALSE;

	/* Check to make sure that the app has an available thumbnail
	 * otherwise, we need to skip it completely */
	app_thumbnail_filename = get_app_thumbnail_cached_filename (app);

	if (!app_thumbnail_filename)
		return FALSE;

	return TRUE;
}

static void
search_done_cb (GObject *source,
		GAsyncResult *res,
		gpointer user_data)
{
	PendingSearch *search = user_data;
	GsDiscoveryFeedContentProvider *self = search->provider;
	GVariantBuilder builder;
	guint app_list_length;
	g_autoptr(GError) error = NULL;
	g_autoptr(GsAppList) list = NULL;

	list = gs_plugin_loader_job_process_finish (self->plugin_loader, res, &error);
	if (list == NULL) {
		g_warning ("Error searching for Discovery Feed Apps: %s", error->message);
		g_dbus_method_invocation_take_error (search->invocation, g_steal_pointer (&error));
		pending_search_free (search);
		g_application_release (g_application_get_default ());
		return;
	}

	/* First, filter out any irrelevant apps. Then randomize the list -
	 * this gives us a different order for each day as the random seed
	 * for gs_app_list_randomize changes on a daily basis. Then, sort
	 * the list, but only use the kudos as the sorting factor. This will
	 * put "more relevant" apps closer to the top, but still with an
	 * internally randomized order. */
	gs_app_list_filter (list, filter_for_discovery_feed_apps, NULL);
	gs_app_list_randomize (list);

	app_list_length = MIN (gs_app_list_length (list), MAX_INSTALLABLE_APPS);

	/* Now that we have our indices, start building up a variant based
	 * on those indices */
	g_variant_builder_init (&builder, G_VARIANT_TYPE ("aa{sv}"));
	for (guint i = 0; i < app_list_length; ++i) {
		GsApp *app = GS_APP (gs_app_list_index (list, i));
		const gchar *app_id = gs_app_get_id (app);
		g_autofree gchar *app_thumbnail_filename = get_app_thumbnail_cached_filename (app);
		GdkPixbuf *pixbuf = gs_app_get_pixbuf (app);
		g_autoptr(GVariant) icon_serialized = NULL;

		if (pixbuf == NULL) {
			g_warning ("App %s should have an icon pixbuf, but does not", app_id);
			continue;
		}

		icon_serialized = g_icon_serialize (G_ICON (pixbuf));

		if (icon_serialized == NULL) {
			g_warning ("App %s should have a serializable icon, but does not", app_id);
			continue;
		}

		g_variant_builder_open (&builder, G_VARIANT_TYPE_VARDICT);
		g_variant_builder_add (&builder, "{sv}", "app_id", g_variant_new_string (app_id));
		g_variant_builder_add (&builder, "{sv}", "id", g_variant_new_string (gs_app_get_unique_id (app)));
		g_variant_builder_add (&builder, "{sv}", "name", g_variant_new_string (gs_app_get_name (app)));
		g_variant_builder_add (&builder, "{sv}", "synopsis", g_variant_new_string (gs_app_get_summary (app)));
		g_variant_builder_add (&builder, "{sv}", "thumbnail_uri", g_variant_new_string (app_thumbnail_filename));
		g_variant_builder_add (&builder, "{sv}", "icon", icon_serialized);

		g_variant_builder_close (&builder);
	}

	gs_discovery_feed_installable_apps_complete_get_installable_apps (self->skeleton,
									  search->invocation,
									  g_variant_builder_end (&builder));

	pending_search_free (search);
	g_application_release (g_application_get_default ());
}

static gboolean
handle_get_discovery_feed_apps (GsDiscoveryFeedInstallableApps *skeleton,
				GDBusMethodInvocation *invocation,
				gpointer user_data)
{
	PendingSearch *pending_search;
	g_autoptr(GsPluginJob) plugin_job = NULL;
	GsDiscoveryFeedContentProvider *self = user_data;

	g_cancellable_cancel (self->cancellable);
	g_clear_object (&self->cancellable);

	pending_search = g_slice_new0 (PendingSearch);
	pending_search->provider = self;
	pending_search->invocation = g_object_ref (invocation);

	g_application_hold (g_application_get_default ());
	self->cancellable = g_cancellable_new ();

	plugin_job = gs_plugin_job_newv (GS_PLUGIN_ACTION_SEARCH,
					 "search", "Endless::HasDiscoveryFeedContent",
					 "failure-flags", GS_PLUGIN_FAILURE_FLAGS_NONE,
					 "refine-flags", GS_PLUGIN_REFINE_FLAGS_REQUIRE_ICON,
					 "max-results", MAX_RESULTS,
					 NULL);
	gs_plugin_loader_job_process_async (self->plugin_loader, plugin_job,
					    self->cancellable,
					    search_done_cb,
					    pending_search);

	return TRUE;
}

gboolean
gs_discovery_feed_content_provider_register (GsDiscoveryFeedContentProvider *self,
                                             GDBusConnection       *connection,
                                             GError               **error)
{
	return g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (self->skeleton),
	                                         connection,
	                                         "/org/gnome/Software/DiscoveryFeedContentProvider", error);
}

void
gs_discovery_feed_content_provider_unregister (GsDiscoveryFeedContentProvider *self)
{
	g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (self->skeleton));
}

static void
discovery_feed_content_provider_dispose (GObject *obj)
{
	GsDiscoveryFeedContentProvider *self = GS_DISCOVERY_FEED_CONTENT_PROVIDER (obj);

	g_cancellable_cancel (self->cancellable);
	g_clear_object (&self->cancellable);
	g_clear_object (&self->plugin_loader);
	g_clear_object (&self->skeleton);

	G_OBJECT_CLASS (gs_discovery_feed_content_provider_parent_class)->dispose (obj);
}

static void
gs_discovery_feed_content_provider_init (GsDiscoveryFeedContentProvider *self)
{
	self->skeleton = gs_discovery_feed_installable_apps_skeleton_new ();

	g_signal_connect (self->skeleton, "handle-get-installable-apps",
			  G_CALLBACK (handle_get_discovery_feed_apps), self);
}

static void
gs_discovery_feed_content_provider_class_init (GsDiscoveryFeedContentProviderClass *klass)
{
	GObjectClass *oclass = G_OBJECT_CLASS (klass);

	oclass->dispose = discovery_feed_content_provider_dispose;
}

GsDiscoveryFeedContentProvider *
gs_discovery_feed_content_provider_new (void)
{
	return g_object_new (gs_discovery_feed_content_provider_get_type (), NULL);
}

void
gs_discovery_feed_content_provider_setup (GsDiscoveryFeedContentProvider *provider,
				          GsPluginLoader *loader)
{
	provider->plugin_loader = g_object_ref (loader);
}
