/*
 * gs-discovery-feed-content-provider.h - Implementation of an EOS
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

#ifndef __GS_DISCOVERY_FEED_CONTENT_PROVIDER_H
#define __GS_DISCOVERY_FEED_CONTENT_PROVIDER_H

#include "gs-plugin-loader.h"

#define GS_TYPE_DISCOVERY_FEED_CONTENT_PROVIDER gs_discovery_feed_content_provider_get_type()

G_DECLARE_FINAL_TYPE (GsDiscoveryFeedContentProvider, gs_discovery_feed_content_provider, GS, DISCOVERY_FEED_CONTENT_PROVIDER, GObject)

gboolean	gs_discovery_feed_content_provider_register	(GsDiscoveryFeedContentProvider	 *self,
								 GDBusConnection		 *connection,
								 GError				**error);
void		gs_discovery_feed_content_provider_unregister	(GsDiscoveryFeedContentProvider	 *self);
GsDiscoveryFeedContentProvider	*gs_discovery_feed_content_provider_new		(void);
void		gs_discovery_feed_content_provider_setup	(GsDiscoveryFeedContentProvider	 *provider,
								 GsPluginLoader			 *loader);

#endif /* __GS_DISCOVERY_FEED_CONTENT_PROVIDER_H */
