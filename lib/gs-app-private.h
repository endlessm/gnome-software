/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2016 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#pragma once

#include "gs-app.h"
#include "gs-plugin-types.h"

G_BEGIN_DECLS

void		 gs_app_set_unique_id		(GsApp		*app,
						 const gchar	*unique_id);
void		 gs_app_remove_addon		(GsApp		*app,
						 GsApp		*addon);
GCancellable	*gs_app_get_cancellable		(GsApp		*app);
GsPluginAction	 gs_app_get_pending_action	(GsApp		*app);
void		 gs_app_set_pending_action	(GsApp		*app,
						 GsPluginAction	 action);
gint		 gs_app_compare_priority	(GsApp		*app1,
						 GsApp		*app2);

G_END_DECLS
