/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2013 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2013 Matthias Clasen <mclasen@redhat.com>
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
 * GNU General Public License for more category.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#include <string.h>
#include <ostree.h>

#include "gs-common.h"
#include "gs-background-tile.h"
#include "gs-category-page.h"

#define MAX_PLACEHOLDER_TILES 30

struct _GsCategoryPage
{
	GsPage		 parent_instance;

	GsPluginLoader	*plugin_loader;
	GtkBuilder	*builder;
	GCancellable	*cancellable;
	GsShell		*shell;
	GsCategory	*category;
	GsCategory	*subcategory;
	GHashTable	*category_apps; /* (element-type GsCategory GsAppList) */
	gint		 num_placeholders_to_show;

	GtkWidget	*infobar_category_shell_extensions;
	GtkWidget	*button_category_shell_extensions;
	GtkWidget	*category_detail_box;
	GtkWidget	*listbox_filter;
	GtkWidget	*scrolledwindow_category;
	GtkWidget	*scrolledwindow_filter;
	GtkWidget	*no_apps_box;
	GtkWidget	*usb_action_box;
	GtkWidget	*copy_os_to_usb_button;
	GtkWidget	*cancel_os_copy_button;
	GtkWidget	*os_copy_spinner;
};

G_DEFINE_TYPE (GsCategoryPage, gs_category_page, GS_TYPE_PAGE)

static void
gs_category_page_switch_to (GsPage *page, gboolean scroll_up)
{
	GsCategoryPage *self = GS_CATEGORY_PAGE (page);
	GtkWidget *widget;

	widget = GTK_WIDGET (gtk_builder_get_object (self->builder, "buttonbox_main"));
	gtk_widget_show (widget);
}

static void
app_tile_clicked (GsAppTile *tile, gpointer data)
{
	GsCategoryPage *self = GS_CATEGORY_PAGE (data);
	GsApp *app;

	app = gs_app_tile_get_app (tile);
	gs_shell_show_app (self->shell, app);
}

static gboolean
gs_category_page_has_app (GsCategoryPage *self,
			  GsApp *app)
{
	GHashTableIter iter;
	gpointer value;
	const gchar *id = gs_app_get_unique_id (app);

	g_hash_table_iter_init (&iter, self->category_apps);
	while (g_hash_table_iter_next (&iter, NULL, &value)) {
		GsAppList *list = value;
		if (gs_app_list_lookup (list, id) != NULL)
			return TRUE;
	}
	return FALSE;
}

static void
gs_category_page_remove_apps_for_category (GsCategoryPage *self,
					   GsCategory *category,
					   GsAppList *apps_to_remove)
{
	GsAppList *list = g_hash_table_lookup (self->category_apps, category);
	if (list == NULL)
		return;

	for (guint i = 0; i < gs_app_list_length (apps_to_remove); ++i) {
		GsApp *app = gs_app_list_index (apps_to_remove, i);
		gs_app_list_remove (list, app);
	}
}

static void
gs_category_page_get_apps_cb (GObject *source_object,
                              GAsyncResult *res,
                              gpointer user_data)
{
	GtkWidget *tile;
	GsCategoryPage *self = GS_CATEGORY_PAGE (user_data);
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (source_object);
	g_autoptr(GError) error = NULL;
	g_autoptr(GsAppList) list = NULL;
	GsAppList *category_app_list = NULL;
	g_autoptr(GsAppList) new_app_list = NULL;

	list = gs_plugin_loader_job_process_finish (plugin_loader,
						    res,
						    &error);
	if (list == NULL) {
		if (!g_error_matches (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_CANCELLED))
			g_warning ("failed to get apps for category apps: %s", error->message);
		return;
	}

	category_app_list = g_hash_table_lookup (self->category_apps, self->subcategory);
	if (category_app_list == NULL) {
		category_app_list = gs_app_list_new ();
		g_hash_table_insert (self->category_apps, self->subcategory,
				     category_app_list);
	}

	/* gather any new apps that may not be already in the category view */
	new_app_list = gs_app_list_new ();
	for (guint i = 0; i < gs_app_list_length (list); ++i) {
		GsApp *app = gs_app_list_index (list, i);
		if (gs_app_list_lookup (category_app_list, gs_app_get_unique_id (app)) != NULL)
			continue;

		gs_app_list_add (new_app_list, app);
	}

	/* add the new apps to the category */
	for (guint i = 0; i < gs_app_list_length (new_app_list); i++) {
		GsApp *app = gs_app_list_index (new_app_list, i);
		/* new tiles are only created if they don't exist yet
		 * (they may have been added from another category since an
		 * app may be in several categories) */
		if (!gs_category_page_has_app (self, app)) {
			tile = gs_background_tile_new (app);
			g_signal_connect (tile, "clicked",
					  G_CALLBACK (app_tile_clicked), self);
			gtk_container_add (GTK_CONTAINER (self->category_detail_box), tile);
			gtk_widget_set_can_focus (gtk_widget_get_parent (tile), FALSE);
		}
		gs_app_list_add (category_app_list, app);
	}

	/* if an app is no longer part of a category, remove it from there */
	if (gs_app_list_length (category_app_list) != gs_app_list_length (list)) {
		g_autoptr(GsAppList) apps_to_remove = gs_app_list_new ();
		for (guint i = 0; i < gs_app_list_length (category_app_list); ++i) {
			GsApp *app = gs_app_list_index (category_app_list, i);
			if (gs_app_list_lookup (list, gs_app_get_unique_id (app)) == NULL) {
				g_debug ("App %s is no longer in category %s::%s",
					 gs_app_get_unique_id (app),
					 gs_category_get_id (self->category),
					 gs_category_get_id (self->subcategory));
				gs_app_list_add (apps_to_remove, app);
			}
		}
		gs_category_page_remove_apps_for_category (self,
							   self->subcategory,
							   apps_to_remove);
	}

	/* ensure the filter will show the apps, not the placeholders */
	self->num_placeholders_to_show = -1;
	gtk_flow_box_invalidate_filter (GTK_FLOW_BOX (self->category_detail_box));

	/* seems a good place */
	gs_shell_profile_dump (self->shell);
}

static void
set_os_copying_state (GsCategoryPage *self,
                      gboolean        copying)
{
	gtk_widget_set_visible (self->copy_os_to_usb_button, !copying);
	gtk_widget_set_visible (self->os_copy_spinner, copying);
	gtk_widget_set_visible (self->cancel_os_copy_button, copying);

	if (copying)
		gs_start_spinner (GTK_SPINNER (self->os_copy_spinner));
	else
		gs_stop_spinner (GTK_SPINNER (self->os_copy_spinner));
}

static void
gs_category_page_os_copied (GsPage *page)
{
	GsCategoryPage *self = GS_CATEGORY_PAGE (page);
	set_os_copying_state (self, FALSE);
}

static void
cancel_os_copy_button_cb (GtkButton *button, GsCategoryPage *self)
{
	g_cancellable_cancel (self->cancellable);
	set_os_copying_state (self, FALSE);
}

static void
copy_os_to_usb_button_cb (GtkButton *button, GsCategoryPage *self)
{
	g_autoptr(GList) copy_dests = gs_plugin_loader_dup_copy_dests (self->plugin_loader);
	g_return_if_fail (copy_dests != NULL);

	set_os_copying_state (self, TRUE);
	gs_page_copy_os (GS_PAGE (self), copy_dests->data, GS_SHELL_INTERACTION_FULL,
			 self->cancellable);
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

	sysroot = ostree_sysroot_new_default ();
	if (!ostree_sysroot_load (sysroot, NULL, NULL))
		return NULL;

	booted_deployment = ostree_sysroot_get_booted_deployment (sysroot);
	if (booted_deployment == NULL)
		return NULL;

	origin = ostree_deployment_get_origin (booted_deployment);
	if (origin == NULL)
		return NULL;

	refspec = g_key_file_get_string (origin, "origin", "refspec", NULL);
	if (refspec == NULL)
		return NULL;

	ostree_parse_refspec (refspec, &remote, NULL, NULL);
	if (remote == NULL)
		return NULL;

	repo = ostree_repo_new_default ();
	ostree_repo_get_remote_option (repo, remote, "collection-id", NULL, &collection_id, NULL);

	return g_steal_pointer (&collection_id);
}

static void
gs_category_page_reload (GsPage *page)
{
	GsCategoryPage *self = GS_CATEGORY_PAGE (page);
	GtkAdjustment *adj = NULL;
	g_autoptr(GsPluginJob) plugin_job = NULL;

	if (self->subcategory == NULL)
		return;

	if (self->cancellable != NULL) {
		g_cancellable_cancel (self->cancellable);
		g_object_unref (self->cancellable);
	}
	self->cancellable = g_cancellable_new ();

	g_debug ("search using %s/%s",
	         gs_category_get_id (self->category),
	         gs_category_get_id (self->subcategory));

	/* show the shell extensions header */
	if (g_strcmp0 (gs_category_get_id (self->category), "addons") == 0 &&
	    g_strcmp0 (gs_category_get_id (self->subcategory), "shell-extensions") == 0) {
		gtk_widget_set_visible (self->infobar_category_shell_extensions, TRUE);
	} else {
		gtk_widget_set_visible (self->infobar_category_shell_extensions, FALSE);
	}

	if (g_strcmp0 (gs_category_get_id (self->category), "usb") == 0) {
		g_autofree char *os_collection_id = get_os_collection_id ();
		if (os_collection_id != NULL)
		  {
			g_signal_connect (self->copy_os_to_usb_button, "clicked",
					  G_CALLBACK (copy_os_to_usb_button_cb), self);
			gtk_widget_set_visible (self->usb_action_box, TRUE);
			set_os_copying_state (self, FALSE);
		} else
			gtk_widget_set_visible (self->usb_action_box, FALSE);
	} else
		gtk_widget_set_visible (self->usb_action_box, FALSE);

	g_signal_connect (self->cancel_os_copy_button, "clicked",
			  G_CALLBACK (cancel_os_copy_button_cb), self);

	/* ensure the placeholders are shown */
	self->num_placeholders_to_show = MIN (MAX_PLACEHOLDER_TILES,
					      (gint) gs_category_get_size (self->subcategory));
	gtk_flow_box_invalidate_filter (GTK_FLOW_BOX (self->category_detail_box));

	/* scroll the list of apps to the beginning, otherwise it will show
	 * with the previous scroll value */
	adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->scrolledwindow_category));
	gtk_adjustment_set_value (adj, gtk_adjustment_get_lower (adj));

	plugin_job = gs_plugin_job_newv (GS_PLUGIN_ACTION_GET_CATEGORY_APPS,
					 "category", self->subcategory,
					 "failure-flags", GS_PLUGIN_FAILURE_FLAGS_NONE,
					 "refine-flags", GS_PLUGIN_REFINE_FLAGS_REQUIRE_ICON |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_VERSION |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_RATING |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_ORIGIN_HOSTNAME|
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_KEY_COLORS,
					 NULL);
	gs_plugin_loader_job_process_async (self->plugin_loader,
					    plugin_job,
					    self->cancellable,
					    gs_category_page_get_apps_cb,
					    self);
}

static void
gs_category_page_populate_filtered (GsCategoryPage *self, GsCategory *subcategory)
{
	g_assert (subcategory != NULL);
	g_set_object (&self->subcategory, subcategory);
	gs_category_page_reload (GS_PAGE (self));
}

static void
filter_selected (GtkListBox *filters, GtkListBoxRow *row, gpointer data)
{
	GsCategoryPage *self = GS_CATEGORY_PAGE (data);
	GsCategory *category;

	if (row == NULL)
		return;

	category = g_object_get_data (G_OBJECT (gtk_bin_get_child (GTK_BIN (row))), "category");
	gs_category_page_populate_filtered (self, category);
}

static void
gs_category_page_create_filter_list (GsCategoryPage *self,
                                     GsCategory *category)
{
	GtkWidget *row;
	GsCategory *s;
	guint i;
	GPtrArray *children;
	GtkWidget *first_subcat = NULL;

	gs_container_remove_all (GTK_CONTAINER (self->listbox_filter));

	children = gs_category_get_children (category);
	for (i = 0; i < children->len; i++) {
		s = GS_CATEGORY (g_ptr_array_index (children, i));
		if (gs_category_get_size (s) < 1) {
			gboolean category_is_usb = FALSE;

			g_debug ("not showing %s/%s as no apps",
				 gs_category_get_id (category),
				 gs_category_get_id (s));

			/* re-filter USB category with no apps so the
			 * placeholder app tiles get cleared out, then set
			 * "empty state" message for an empty USB disk
			 */
			if (g_strcmp0 (gs_category_get_id (category), "usb") == 0) {
				gs_category_page_populate_filtered (self, s);
				category_is_usb = TRUE;
			}

			gtk_widget_set_visible (self->no_apps_box, category_is_usb);
			gtk_widget_set_visible (self->scrolledwindow_category, !category_is_usb);

			continue;
		}
		row = gtk_label_new (gs_category_get_name (s));
		g_object_set_data_full (G_OBJECT (row), "category", g_object_ref (s), g_object_unref);
		g_object_set (row, "xalign", 0.0, "margin", 10, NULL);
		gtk_widget_show (row);
		gtk_list_box_insert (GTK_LIST_BOX (self->listbox_filter), row, -1);

		/* make sure the first subcategory gets selected */
		if (first_subcat == NULL)
		        first_subcat = row;
	}
	if (first_subcat != NULL)
		gtk_list_box_select_row (GTK_LIST_BOX (self->listbox_filter),
					 GTK_LIST_BOX_ROW (gtk_widget_get_parent (first_subcat)));
}

void
gs_category_page_set_category (GsCategoryPage *self, GsCategory *category)
{
	/* this means we've come from the app-view -> back */
	if (self->category == category)
		return;

	/* save this */
	g_clear_object (&self->category);
	self->category = g_object_ref (category);

	/* find apps in this group */
	gs_category_page_create_filter_list (self, category);
}

GsCategory *
gs_category_page_get_category (GsCategoryPage *self)
{
	return self->category;
}

static gboolean
gs_category_page_filter_apps_func (GtkFlowBoxChild *child,
				   gpointer user_data)
{
	GsCategoryPage *self = user_data;
	GsApp *app = gs_app_tile_get_app (GS_APP_TILE (gtk_bin_get_child (GTK_BIN (child))));
	GsAppList *category_apps = NULL;

	if (self->subcategory == NULL)
		return TRUE;

	/* let's show as many placeholders as desired */
	if (self->num_placeholders_to_show > -1) {
		/* don't show normal app tiles if we're supposed to show placeholders*/
		if (app != NULL)
			return FALSE;

		/* we cannot drop below 0 here, otherwise it will start showing
		 * the regular app tiles */
		if (self->num_placeholders_to_show > 0)
			--self->num_placeholders_to_show;
		return TRUE;
	}

	if (app == NULL)
		return FALSE;

	category_apps = g_hash_table_lookup (self->category_apps, self->subcategory);
	return category_apps != NULL &&
		gs_app_list_lookup (category_apps, gs_app_get_unique_id (app));
}

static void
gs_category_page_init (GsCategoryPage *self)
{
	gtk_widget_init_template (GTK_WIDGET (self));
}

static void
gs_category_page_dispose (GObject *object)
{
	GsCategoryPage *self = GS_CATEGORY_PAGE (object);

	if (self->cancellable != NULL) {
		g_cancellable_cancel (self->cancellable);
		g_clear_object (&self->cancellable);
	}

	g_clear_object (&self->builder);
	g_clear_object (&self->category);
	g_clear_object (&self->subcategory);
	g_clear_object (&self->plugin_loader);

	g_clear_pointer (&self->category_apps, g_hash_table_unref);

	G_OBJECT_CLASS (gs_category_page_parent_class)->dispose (object);
}

static gboolean
key_event (GtkWidget *listbox, GdkEvent *event, GsCategoryPage *self)
{
	guint keyval;
	gboolean handled;

	if (!gdk_event_get_keyval (event, &keyval))
		return FALSE;

	if (keyval == GDK_KEY_Page_Up ||
	    keyval == GDK_KEY_KP_Page_Up)
		g_signal_emit_by_name (self->scrolledwindow_category, "scroll-child",
				       GTK_SCROLL_PAGE_UP, FALSE, &handled);
	else if (keyval == GDK_KEY_Page_Down ||
	    	 keyval == GDK_KEY_KP_Page_Down)
		g_signal_emit_by_name (self->scrolledwindow_category, "scroll-child",
				       GTK_SCROLL_PAGE_DOWN, FALSE, &handled);
	else if (keyval == GDK_KEY_Tab ||
		 keyval == GDK_KEY_KP_Tab)
		gtk_widget_child_focus (self->category_detail_box, GTK_DIR_TAB_FORWARD);
	else
		return FALSE;

	return TRUE;
}

static void
button_shell_extensions_cb (GtkButton *button, GsCategoryPage *self)
{
	gboolean ret;
	g_autoptr(GError) error = NULL;
	const gchar *argv[] = { "gnome-shell-extension-prefs", NULL };
	ret = g_spawn_async (NULL, (gchar **) argv, NULL, G_SPAWN_SEARCH_PATH,
			     NULL, NULL, NULL, &error);
	if (!ret)
		g_warning ("failed to exec %s: %s", argv[0], error->message);
}

static gboolean
gs_category_page_setup (GsPage *page,
                        GsShell *shell,
                        GsPluginLoader *plugin_loader,
                        GtkBuilder *builder,
                        GCancellable *cancellable,
                        GError **error)
{
	GsCategoryPage *self = GS_CATEGORY_PAGE (page);
	GtkAdjustment *adj;

	self->plugin_loader = g_object_ref (plugin_loader);
	self->builder = g_object_ref (builder);
	self->shell = shell;
	self->category_apps = g_hash_table_new_full (g_direct_hash,
						     g_direct_equal,
						     g_object_unref,
						     g_object_unref);

	g_signal_connect (self->listbox_filter, "row-selected", G_CALLBACK (filter_selected), self);

	adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->scrolledwindow_category));
	gtk_container_set_focus_vadjustment (GTK_CONTAINER (self->category_detail_box), adj);

	gtk_flow_box_set_filter_func (GTK_FLOW_BOX (self->category_detail_box),
				      gs_category_page_filter_apps_func,
				      page,
				      NULL);

	/* add the placeholder tiles already, to be shown when loading the category view */
	for (guint i = 0; i < MAX_PLACEHOLDER_TILES; ++i) {
		GtkWidget *tile = gs_background_tile_new (NULL);
		gtk_container_add (GTK_CONTAINER (self->category_detail_box), tile);
		gtk_widget_set_can_focus (gtk_widget_get_parent (tile), FALSE);
	}


	g_signal_connect (self->listbox_filter, "key-press-event",
			  G_CALLBACK (key_event), self);

	g_signal_connect (self->button_category_shell_extensions, "clicked",
			  G_CALLBACK (button_shell_extensions_cb), self);
	return TRUE;
}

static void
gs_category_page_class_init (GsCategoryPageClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GsPageClass *page_class = GS_PAGE_CLASS (klass);
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

	object_class->dispose = gs_category_page_dispose;
	page_class->switch_to = gs_category_page_switch_to;
	page_class->reload = gs_category_page_reload;
	page_class->setup = gs_category_page_setup;
	page_class->os_copied = gs_category_page_os_copied;

	gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/Software/gs-category-page.ui");

	gtk_widget_class_bind_template_child (widget_class, GsCategoryPage, category_detail_box);
	gtk_widget_class_bind_template_child (widget_class, GsCategoryPage, infobar_category_shell_extensions);
	gtk_widget_class_bind_template_child (widget_class, GsCategoryPage, button_category_shell_extensions);
	gtk_widget_class_bind_template_child (widget_class, GsCategoryPage, listbox_filter);
	gtk_widget_class_bind_template_child (widget_class, GsCategoryPage, scrolledwindow_category);
	gtk_widget_class_bind_template_child (widget_class, GsCategoryPage, scrolledwindow_filter);
	gtk_widget_class_bind_template_child (widget_class, GsCategoryPage, no_apps_box);
	gtk_widget_class_bind_template_child (widget_class, GsCategoryPage, usb_action_box);
	gtk_widget_class_bind_template_child (widget_class, GsCategoryPage, copy_os_to_usb_button);
	gtk_widget_class_bind_template_child (widget_class, GsCategoryPage, cancel_os_copy_button);
	gtk_widget_class_bind_template_child (widget_class, GsCategoryPage, os_copy_spinner);
}

GsCategoryPage *
gs_category_page_new (void)
{
	GsCategoryPage *self;
	self = g_object_new (GS_TYPE_CATEGORY_PAGE, NULL);
	return self;
}

/* vim: set noexpandtab: */
