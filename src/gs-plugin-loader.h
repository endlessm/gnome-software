/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007-2015 Richard Hughes <richard@hughsie.com>
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

#ifndef __GS_PLUGIN_LOADER_H
#define __GS_PLUGIN_LOADER_H

#include <glib-object.h>

#include "gs-app.h"
#include "gs-auth.h"
#include "gs-category.h"
#include "gs-plugin-event.h"
#include "gs-plugin-private.h"

G_BEGIN_DECLS

#define GS_TYPE_PLUGIN_LOADER		(gs_plugin_loader_get_type ())

G_DECLARE_DERIVABLE_TYPE (GsPluginLoader, gs_plugin_loader, GS, PLUGIN_LOADER, GObject)

struct _GsPluginLoaderClass
{
	GObjectClass		 parent_class;
	void			(*status_changed)	(GsPluginLoader	*plugin_loader,
							 GsApp		*app,
							 GsPluginStatus	 status);
	void			(*pending_apps_changed)	(GsPluginLoader	*plugin_loader);
	void			(*updates_changed)	(GsPluginLoader	*plugin_loader);
	void			(*reload)		(GsPluginLoader	*plugin_loader);
};

typedef void	 (*GsPluginLoaderFinishedFunc)		(GsPluginLoader	*plugin_loader,
							 GsApp		*app,
							 gpointer	 user_data);

GsPluginLoader	*gs_plugin_loader_new			(void);
void		 gs_plugin_loader_get_installed_async	(GsPluginLoader	*plugin_loader,
							 GsPluginRefineFlags refine_flags,
							 GsPluginFailureFlags failure_flags,
							 GCancellable	*cancellable,
							 GAsyncReadyCallback callback,
							 gpointer	 user_data);
GsAppList	*gs_plugin_loader_get_installed_finish	(GsPluginLoader	*plugin_loader,
							 GAsyncResult	*res,
							 GError		**error);
void		 gs_plugin_loader_get_updates_async	(GsPluginLoader	*plugin_loader,
							 GsPluginRefineFlags refine_flags,
							 GsPluginFailureFlags failure_flags,
							 GCancellable	*cancellable,
							 GAsyncReadyCallback callback,
							 gpointer	 user_data);
GsAppList	*gs_plugin_loader_get_updates_finish	(GsPluginLoader	*plugin_loader,
							 GAsyncResult	*res,
							 GError		**error);
void		 gs_plugin_loader_get_distro_upgrades_async (GsPluginLoader	*plugin_loader,
							 GsPluginRefineFlags refine_flags,
							 GsPluginFailureFlags failure_flags,
							 GCancellable	*cancellable,
							 GAsyncReadyCallback callback,
							 gpointer	 user_data);
GsAppList	*gs_plugin_loader_get_distro_upgrades_finish (GsPluginLoader	*plugin_loader,
							 GAsyncResult	*res,
							 GError		**error);
void		 gs_plugin_loader_get_unvoted_reviews_async (GsPluginLoader	*plugin_loader,
							 GsPluginRefineFlags refine_flags,
							 GsPluginFailureFlags failure_flags,
							 GCancellable	*cancellable,
							 GAsyncReadyCallback callback,
							 gpointer	 user_data);
GsAppList	*gs_plugin_loader_get_unvoted_reviews_finish (GsPluginLoader	*plugin_loader,
							 GAsyncResult	*res,
							 GError		**error);
void		 gs_plugin_loader_get_sources_async	(GsPluginLoader	*plugin_loader,
							 GsPluginRefineFlags refine_flags,
							 GsPluginFailureFlags failure_flags,
							 GCancellable	*cancellable,
							 GAsyncReadyCallback callback,
							 gpointer	 user_data);
GsAppList	*gs_plugin_loader_get_sources_finish	(GsPluginLoader	*plugin_loader,
							 GAsyncResult	*res,
							 GError		**error);
void		 gs_plugin_loader_get_popular_async	(GsPluginLoader	*plugin_loader,
							 GsPluginRefineFlags refine_flags,
							 GsPluginFailureFlags failure_flags,
							 GCancellable	*cancellable,
							 GAsyncReadyCallback callback,
							 gpointer	 user_data);
GsAppList	*gs_plugin_loader_get_popular_finish	(GsPluginLoader	*plugin_loader,
							 GAsyncResult	*res,
							 GError		**error);
void		 gs_plugin_loader_get_featured_async	(GsPluginLoader	*plugin_loader,
							 GsPluginRefineFlags refine_flags,
							 GsPluginFailureFlags failure_flags,
							 GCancellable	*cancellable,
							 GAsyncReadyCallback callback,
							 gpointer	 user_data);
GsAppList	*gs_plugin_loader_get_featured_finish	(GsPluginLoader	*plugin_loader,
							 GAsyncResult	*res,
							 GError		**error);
void		 gs_plugin_loader_get_categories_async	(GsPluginLoader	*plugin_loader,
							 GsPluginRefineFlags refine_flags,
							 GsPluginFailureFlags failure_flags,
							 GCancellable	*cancellable,
							 GAsyncReadyCallback callback,
							 gpointer	 user_data);
GPtrArray	*gs_plugin_loader_get_categories_finish	(GsPluginLoader	*plugin_loader,
							 GAsyncResult	*res,
							 GError		**error);
void		 gs_plugin_loader_get_category_apps_async (GsPluginLoader	*plugin_loader,
							 GsCategory	*category,
							 GsPluginRefineFlags refine_flags,
							 GsPluginFailureFlags failure_flags,
							 GCancellable	*cancellable,
							 GAsyncReadyCallback callback,
							 gpointer	 user_data);
GsAppList	*gs_plugin_loader_get_category_apps_finish (GsPluginLoader	*plugin_loader,
							 GAsyncResult	*res,
							 GError		**error);
void		 gs_plugin_loader_search_async		(GsPluginLoader	*plugin_loader,
							 const gchar	*value,
							 GsPluginRefineFlags refine_flags,
							 GsPluginFailureFlags failure_flags,
							 GCancellable	*cancellable,
							 GAsyncReadyCallback callback,
							 gpointer	 user_data);
GsAppList	*gs_plugin_loader_search_finish		(GsPluginLoader	*plugin_loader,
							 GAsyncResult	*res,
							 GError		**error);
void		 gs_plugin_loader_search_files_async	(GsPluginLoader	*plugin_loader,
							 const gchar	*value,
							 GsPluginRefineFlags refine_flags,
							 GsPluginFailureFlags failure_flags,
							 GCancellable	*cancellable,
							 GAsyncReadyCallback callback,
							 gpointer	 user_data);
GsAppList	*gs_plugin_loader_search_files_finish	(GsPluginLoader	*plugin_loader,
							 GAsyncResult	*res,
							 GError		**error);
void		 gs_plugin_loader_search_what_provides_async (GsPluginLoader	*plugin_loader,
							 const gchar	*value,
							 GsPluginRefineFlags refine_flags,
							 GsPluginFailureFlags failure_flags,
							 GCancellable	*cancellable,
							 GAsyncReadyCallback callback,
							 gpointer	 user_data);
GsAppList	*gs_plugin_loader_search_what_provides_finish (GsPluginLoader	*plugin_loader,
							 GAsyncResult	*res,
							 GError		**error);
void		 gs_plugin_loader_file_to_app_async	(GsPluginLoader	*plugin_loader,
							 GFile		*file,
							 GsPluginRefineFlags refine_flags,
							 GsPluginFailureFlags failure_flags,
							 GCancellable	*cancellable,
							 GAsyncReadyCallback callback,
							 gpointer	 user_data);
GsApp		*gs_plugin_loader_file_to_app_finish	(GsPluginLoader	*plugin_loader,
							 GAsyncResult	*res,
							 GError		**error);
void		 gs_plugin_loader_update_async		(GsPluginLoader	*plugin_loader,
							 GsAppList	*apps,
							 GsPluginFailureFlags failure_flags,
							 GCancellable	*cancellable,
							 GAsyncReadyCallback callback,
							 gpointer	 user_data);
gboolean	 gs_plugin_loader_update_finish		(GsPluginLoader	*plugin_loader,
							 GAsyncResult	*res,
							 GError		**error);
gboolean	 gs_plugin_loader_setup			(GsPluginLoader	*plugin_loader,
							 gchar		**whitelist,
							 gchar		**blacklist,
							 GsPluginFailureFlags failure_flags,
							 GCancellable	*cancellable,
							 GError		**error);
void		 gs_plugin_loader_dump_state		(GsPluginLoader	*plugin_loader);
gboolean	 gs_plugin_loader_get_enabled		(GsPluginLoader	*plugin_loader,
							 const gchar	*plugin_name);
void		 gs_plugin_loader_set_location		(GsPluginLoader	*plugin_loader,
							 const gchar	*location);
GsAuth		*gs_plugin_loader_get_auth_by_id	(GsPluginLoader	*plugin_loader,
							 const gchar	*provider_id);
guint		 gs_plugin_loader_get_scale		(GsPluginLoader	*plugin_loader);
void		 gs_plugin_loader_set_scale		(GsPluginLoader	*plugin_loader,
							 guint		 scale);
void		 gs_plugin_loader_app_refine_async	(GsPluginLoader	*plugin_loader,
							 GsApp		*app,
							 GsPluginRefineFlags refine_flags,
							 GsPluginFailureFlags failure_flags,
							 GCancellable	*cancellable,
							 GAsyncReadyCallback callback,
							 gpointer	 user_data);
gboolean	 gs_plugin_loader_app_refine_finish	(GsPluginLoader	*plugin_loader,
							 GAsyncResult	*res,
							 GError		**error);
void		 gs_plugin_loader_app_action_async	(GsPluginLoader	*plugin_loader,
							 GsApp		*app,
							 GsPluginAction	 a,
							 GsPluginFailureFlags failure_flags,
							 GCancellable	*cancellable,
							 GAsyncReadyCallback callback,
							 gpointer	 user_data);
gboolean	 gs_plugin_loader_app_action_finish	(GsPluginLoader	*plugin_loader,
							 GAsyncResult	*res,
							 GError		**error);
gboolean	 gs_plugin_loader_review_action_finish	(GsPluginLoader	*plugin_loader,
							 GAsyncResult	*res,
							 GError		**error);
void		 gs_plugin_loader_review_action_async	(GsPluginLoader	*plugin_loader,
							 GsApp		*app,
							 AsReview	*review,
							 GsPluginAction	 action,
							 GsPluginFailureFlags failure_flags,
							 GCancellable	*cancellable,
							 GAsyncReadyCallback callback,
							 gpointer	 user_data);
gboolean	 gs_plugin_loader_auth_action_finish	(GsPluginLoader	*plugin_loader,
							 GAsyncResult	*res,
							 GError		**error);
void		 gs_plugin_loader_auth_action_async	(GsPluginLoader	*plugin_loader,
							 GsAuth		*auth,
							 GsPluginAction	 action,
							 GsPluginFailureFlags failure_flags,
							 GCancellable	*cancellable,
							 GAsyncReadyCallback callback,
							 gpointer	 user_data);
gboolean	 gs_plugin_loader_refresh_finish	(GsPluginLoader	*plugin_loader,
							 GAsyncResult	*res,
							 GError		**error);
void		 gs_plugin_loader_refresh_async		(GsPluginLoader	*plugin_loader,
							 guint		 cache_age,
							 GsPluginRefreshFlags refresh_flags,
							 GsPluginFailureFlags failure_flags,
							 GCancellable	*cancellable,
							 GAsyncReadyCallback callback,
							 gpointer	 user_data);
GsAppList	*gs_plugin_loader_get_pending		(GsPluginLoader	*plugin_loader);
gboolean	 gs_plugin_loader_get_allow_updates	(GsPluginLoader	*plugin_loader);
gboolean	 gs_plugin_loader_get_network_available	(GsPluginLoader *plugin_loader);
gboolean	 gs_plugin_loader_get_network_metered	(GsPluginLoader *plugin_loader);
gboolean	 gs_plugin_loader_get_plugin_supported	(GsPluginLoader	*plugin_loader,
							 const gchar	*plugin_func);

GPtrArray	*gs_plugin_loader_get_events		(GsPluginLoader	*plugin_loader);
GsPluginEvent	*gs_plugin_loader_get_event_by_id	(GsPluginLoader	*plugin_loader,
							 const gchar	*unique_id);
GsPluginEvent	*gs_plugin_loader_get_event_default	(GsPluginLoader	*plugin_loader);
void		 gs_plugin_loader_remove_events		(GsPluginLoader	*plugin_loader);

AsProfile	*gs_plugin_loader_get_profile		(GsPluginLoader	*plugin_loader);
GsApp		*gs_plugin_loader_app_create		(GsPluginLoader	*plugin_loader,
							 const gchar	*unique_id);

G_END_DECLS

#endif /* __GS_PLUGIN_LOADER_H */

/* vim: set noexpandtab: */
