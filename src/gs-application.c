/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 * vi:set noexpandtab tabstop=8 shiftwidth=8:
 *
 * Copyright (C) 2013 Matthias Clasen <mclasen@redhat.com>
 * Copyright (C) 2013-2018 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2014-2018 Kalev Lember <klember@redhat.com>
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#include "config.h"

#include "gs-application.h"

#include <stdlib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <handy.h>
#include <libsoup/soup.h>

#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#endif

#ifdef HAVE_PACKAGEKIT
#include "gs-dbus-helper.h"
#endif

#include "gs-build-ident.h"
#include "gs-common.h"
#include "gs-debug.h"
#include "gs-first-run-dialog.h"
#include "gs-shell.h"
#include "gs-update-monitor.h"
#include "gs-shell-search-provider.h"
#include "gs-folders.h"

#define ENABLE_REPOS_DIALOG_CONF_KEY "enable-repos-dialog"

struct _GsApplication {
	GtkApplication	 parent;
	GCancellable	*cancellable;
	GtkCssProvider	*provider;
	GsPluginLoader	*plugin_loader;
	gint		 pending_apps;
	GtkWindow	*main_window;
	GsShell		*shell;
	GsUpdateMonitor *update_monitor;
#ifdef HAVE_PACKAGEKIT
	GsDbusHelper	*dbus_helper;
#endif
	GsShellSearchProvider *search_provider;  /* (nullable) (owned) */
	GSettings       *settings;
	GSimpleActionGroup	*action_map;
	guint		 shell_loaded_handler_id;
	GsDebug		*debug;  /* (owned) (not nullable) */
};

G_DEFINE_TYPE (GsApplication, gs_application, GTK_TYPE_APPLICATION);

typedef enum {
	PROP_DEBUG = 1,
} GsApplicationProperty;

static GParamSpec *obj_props[PROP_DEBUG + 1] = { NULL, };

enum {
	INSTALL_RESOURCES_DONE,
	REPOSITORY_CHANGED,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

static const char *
get_version (void)
{
	if (g_strcmp0 (BUILD_TYPE, "release") == 0)
		return VERSION;
	else
		return GS_BUILD_IDENTIFIER;
}

typedef struct {
	GsApplication *app;
	GSimpleAction *action;
	GVariant *action_param;  /* (nullable) */
} GsActivationHelper;

static GsActivationHelper *
gs_activation_helper_new (GsApplication *app,
			  GSimpleAction *action,
			  GVariant *parameter)
{
	GsActivationHelper *helper = g_slice_new0 (GsActivationHelper);
	helper->app = app;
	helper->action = G_SIMPLE_ACTION (action);
	helper->action_param = (parameter != NULL) ? g_variant_ref_sink (parameter) : NULL;

	return helper;
}

static void
gs_activation_helper_free (GsActivationHelper *helper)
{
	g_clear_pointer (&helper->action_param, g_variant_unref);
	g_slice_free (GsActivationHelper, helper);
}

GsPluginLoader *
gs_application_get_plugin_loader (GsApplication *application)
{
	return application->plugin_loader;
}

gboolean
gs_application_has_active_window (GsApplication *application)
{
	GList *windows;

	windows = gtk_application_get_windows (GTK_APPLICATION (application));
	for (GList *l = windows; l != NULL; l = l->next) {
		if (gtk_window_is_active (GTK_WINDOW (l->data)))
			return TRUE;
	}
	return FALSE;
}

static void
gs_application_init (GsApplication *application)
{
	const GOptionEntry options[] = {
		{ "mode", '\0', 0, G_OPTION_ARG_STRING, NULL,
		  /* TRANSLATORS: this is a command line option */
		  _("Start up mode: either ‘updates’, ‘updated’, ‘installed’ or ‘overview’"), _("MODE") },
		{ "search", '\0', 0, G_OPTION_ARG_STRING, NULL,
		  _("Search for applications"), _("SEARCH") },
		{ "details", '\0', 0, G_OPTION_ARG_STRING, NULL,
		  _("Show application details (using application ID)"), _("ID") },
		{ "details-pkg", '\0', 0, G_OPTION_ARG_STRING, NULL,
		  _("Show application details (using package name)"), _("PKGNAME") },
		{ "install", '\0', 0, G_OPTION_ARG_STRING, NULL,
		  _("Install the application (using application ID)"), _("ID") },
		{ "local-filename", '\0', 0, G_OPTION_ARG_FILENAME, NULL,
		  _("Open a local package file"), _("FILENAME") },
		{ "interaction", '\0', 0, G_OPTION_ARG_STRING, NULL,
		  _("The kind of interaction expected for this action: either "
		    "‘none’, ‘notify’, or ‘full’"), NULL },
		{ "verbose", '\0', 0, G_OPTION_ARG_NONE, NULL,
		  _("Show verbose debugging information"), NULL },
		{ "autoupdate", 0, 0, G_OPTION_ARG_NONE, NULL,
		  _("Installs any pending updates in the background"), NULL },
		{ "prefs", 0, 0, G_OPTION_ARG_NONE, NULL,
		  _("Show update preferences"), NULL },
		{ "quit", 0, 0, G_OPTION_ARG_NONE, NULL,
		  _("Quit the running instance"), NULL },
		{ "prefer-local", '\0', 0, G_OPTION_ARG_NONE, NULL,
		  _("Prefer local file sources to AppStream"), NULL },
		{ "version", 0, 0, G_OPTION_ARG_NONE, NULL,
		  _("Show version number"), NULL },
		{ NULL }
	};

	g_application_add_main_option_entries (G_APPLICATION (application), options);
}

static void
gs_application_initialize_plugins (GsApplication *app)
{
	static gboolean initialized = FALSE;
	g_auto(GStrv) plugin_blocklist = NULL;
	g_auto(GStrv) plugin_allowlist = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *tmp;

	if (initialized)
		return;

	initialized = TRUE;

	/* allow for debugging */
	tmp = g_getenv ("GNOME_SOFTWARE_PLUGINS_BLOCKLIST");
	if (tmp != NULL)
		plugin_blocklist = g_strsplit (tmp, ",", -1);
	tmp = g_getenv ("GNOME_SOFTWARE_PLUGINS_ALLOWLIST");
	if (tmp != NULL)
		plugin_allowlist = g_strsplit (tmp, ",", -1);

	app->plugin_loader = gs_plugin_loader_new ();
	if (g_file_test (LOCALPLUGINDIR, G_FILE_TEST_EXISTS))
		gs_plugin_loader_add_location (app->plugin_loader, LOCALPLUGINDIR);
	if (!gs_plugin_loader_setup (app->plugin_loader,
				     plugin_allowlist,
				     plugin_blocklist,
				     NULL,
				     &error)) {
		g_warning ("Failed to setup plugins: %s", error->message);
		exit (1);
	}

	/* show the priority of each plugin */
	gs_plugin_loader_dump_state (app->plugin_loader);

}

static gboolean
gs_application_dbus_register (GApplication    *application,
                              GDBusConnection *connection,
                              const gchar     *object_path,
                              GError         **error)
{
	GsApplication *app = GS_APPLICATION (application);
	app->search_provider = gs_shell_search_provider_new ();
	return gs_shell_search_provider_register (app->search_provider, connection, error);
}

static void
gs_application_dbus_unregister (GApplication    *application,
                                GDBusConnection *connection,
                                const gchar     *object_path)
{
	GsApplication *app = GS_APPLICATION (application);

	if (app->search_provider != NULL) {
		gs_shell_search_provider_unregister (app->search_provider);
		g_clear_object (&app->search_provider);
	}
}

static void
gs_application_show_first_run_dialog (GsApplication *app)
{
	/* XXX: Never show the first run dialog, since it's not useful and it
	 * delays the loading of the app tiles; we keep the setting toggling as
	 * it can be interesting for other purposes in the future */
	if (g_settings_get_boolean (app->settings, "first-run") == TRUE) {
		g_settings_set_boolean (app->settings, "first-run", FALSE);
	}
}

static void
theme_changed (GtkSettings *settings, GParamSpec *pspec, GsApplication *app)
{
	g_autoptr(GFile) file = NULL;
	g_autofree gchar *theme = NULL;

	g_object_get (settings, "gtk-theme-name", &theme, NULL);
	if (g_strcmp0 (theme, "HighContrast") == 0) {
		file = g_file_new_for_uri ("resource:///org/gnome/Software/gtk-style-hc.css");
	} else {
		file = g_file_new_for_uri ("resource:///org/gnome/Software/gtk-style.css");
	}
	gtk_css_provider_load_from_file (app->provider, file, NULL);
}

static void
gs_application_shell_loaded_cb (GsShell *shell, GsApplication *app)
{
	g_signal_handler_disconnect (app->shell, app->shell_loaded_handler_id);
	app->shell_loaded_handler_id = 0;
}

static void
gs_application_initialize_ui (GsApplication *app)
{
	static gboolean initialized = FALSE;

	if (initialized)
		return;

	initialized = TRUE;

	/* get CSS */
	app->provider = gtk_css_provider_new ();
	gtk_style_context_add_provider_for_screen (gdk_screen_get_default (),
						   GTK_STYLE_PROVIDER (app->provider),
						   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

	g_signal_connect (gtk_settings_get_default (), "notify::gtk-theme-name",
			  G_CALLBACK (theme_changed), app);
	theme_changed (gtk_settings_get_default (), NULL, app);

	gs_application_initialize_plugins (app);

	/* setup UI */
	app->shell = gs_shell_new ();
	app->cancellable = g_cancellable_new ();

	app->shell_loaded_handler_id = g_signal_connect (app->shell, "loaded",
							 G_CALLBACK (gs_application_shell_loaded_cb),
							 app);

	gs_shell_setup (app->shell, app->plugin_loader, app->cancellable);
	app->main_window = GTK_WINDOW (app->shell);
	gtk_application_add_window (GTK_APPLICATION (app), app->main_window);
}

static void
gs_application_present_window (GsApplication *app, const gchar *startup_id)
{
	GList *windows;
	GtkWindow *window;

	windows = gtk_application_get_windows (GTK_APPLICATION (app));
	if (windows) {
		window = windows->data;

		if (startup_id != NULL)
			gtk_window_set_startup_id (window, startup_id);
		gtk_window_present (window);
	}
}

static void
sources_activated (GSimpleAction *action,
		   GVariant      *parameter,
		   gpointer       app)
{
	gs_shell_show_sources (GS_APPLICATION (app)->shell);
}

static void
prefs_activated (GSimpleAction *action, GVariant *parameter, gpointer app)
{
	gs_shell_show_prefs (GS_APPLICATION (app)->shell);
}

static void
about_activated (GSimpleAction *action,
		 GVariant      *parameter,
		 gpointer       user_data)
{
	GsApplication *app = GS_APPLICATION (user_data);
	const gchar *authors[] = {
		"Richard Hughes",
		"Matthias Clasen",
		"Kalev Lember",
		"Allan Day",
		"Ryan Lerch",
		"William Jon McCann",
		"Milan Crha",
		"Joaquim Rocha",
		"Robert Ancell",
		"Philip Withnall",
		NULL
	};
	GtkAboutDialog *dialog;
	g_autofree gchar *program_name_alloc = NULL;
	const gchar *program_name;

	dialog = GTK_ABOUT_DIALOG (gtk_about_dialog_new ());
	gtk_about_dialog_set_authors (dialog, authors);
	gtk_about_dialog_set_copyright (dialog, _("Copyright \xc2\xa9 2016–2021 GNOME Software contributors"));
	gtk_about_dialog_set_license_type (dialog, GTK_LICENSE_GPL_2_0);
	gtk_about_dialog_set_logo_icon_name (dialog, APPLICATION_ID);
	gtk_about_dialog_set_translator_credits (dialog, _("translator-credits"));
	gtk_about_dialog_set_version (dialog, get_version());

	if (g_strcmp0 (BUILD_PROFILE, "Devel") == 0) {
		/* This isn’t translated as it’s never released */
		program_name = program_name_alloc = g_strdup_printf ("%s (Development Snapshot)",
								     g_get_application_name ());
	} else {
		program_name = g_get_application_name ();
	}
	gtk_about_dialog_set_program_name (dialog, program_name);

	/* TRANSLATORS: this is the title of the about window */
	gtk_window_set_title (GTK_WINDOW (dialog), _("About Software"));

	/* TRANSLATORS: well, we seem to think so, anyway */
	gtk_about_dialog_set_comments (dialog, _("A nice way to manage the "
						 "software on your system."));

	gs_shell_modal_dialog_present (app->shell, GTK_WINDOW (dialog));

	/* just destroy */
	g_signal_connect_swapped (dialog, "response",
				  G_CALLBACK (gtk_widget_destroy), dialog);
}

static void
cancel_trigger_failed_cb (GObject *source, GAsyncResult *res, gpointer user_data)
{
	GsApplication *app = GS_APPLICATION (user_data);
	g_autoptr(GError) error = NULL;
	if (!gs_plugin_loader_job_action_finish (app->plugin_loader, res, &error)) {
		g_warning ("failed to cancel trigger: %s", error->message);
		return;
	}
}

static void
reboot_failed_cb (GObject *source, GAsyncResult *res, gpointer user_data)
{
	GsApplication *app = GS_APPLICATION (user_data);
	g_autoptr(GError) error = NULL;
	g_autoptr(GVariant) retval = NULL;
	g_autoptr(GsPluginJob) plugin_job = NULL;

	/* get result */
	retval = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &error);
	if (retval != NULL)
		return;

	if (error != NULL) {
		g_warning ("Calling org.gnome.SessionManager.Reboot failed: %s",
			   error->message);
	}

	/* cancel trigger */
	plugin_job = gs_plugin_job_newv (GS_PLUGIN_ACTION_UPDATE_CANCEL, NULL);
	gs_plugin_loader_job_process_async (app->plugin_loader, plugin_job,
					    app->cancellable,
					    cancel_trigger_failed_cb,
					    app);
}

static void
offline_update_cb (GsPluginLoader *plugin_loader,
		   GAsyncResult *res,
		   GsApplication *app)
{
	g_autoptr(GError) error = NULL;
	if (!gs_plugin_loader_job_action_finish (plugin_loader, res, &error)) {
		g_warning ("Failed to trigger offline update: %s", error->message);
		return;
	}

	gs_utils_invoke_reboot_async (NULL, reboot_failed_cb, app);
}

static void
reboot_activated (GSimpleAction *action,
		   GVariant      *parameter,
		   gpointer       data)
{
	gs_utils_invoke_reboot_async (NULL, NULL, NULL);
}

static void
shutdown_activated (GSimpleAction *action,
		    GVariant      *parameter,
		    gpointer       data)
{
	GsApplication *app = GS_APPLICATION (data);
	g_application_quit (G_APPLICATION (app));
}

static void
reboot_and_install (GSimpleAction *action,
		    GVariant      *parameter,
		    gpointer       data)
{
	GsApplication *app = GS_APPLICATION (data);
	g_autoptr(GsPluginJob) plugin_job = NULL;
	gs_application_initialize_plugins (app);
	plugin_job = gs_plugin_job_newv (GS_PLUGIN_ACTION_UPDATE, NULL);
	gs_plugin_loader_job_process_async (app->plugin_loader, plugin_job,
					    app->cancellable,
					    (GAsyncReadyCallback) offline_update_cb,
					    app);
}

static void
quit_activated (GSimpleAction *action,
		GVariant      *parameter,
		gpointer       app)
{
	GApplicationFlags flags;
	GList *windows;
	GtkWidget *window;

	flags = g_application_get_flags (app);

	if (flags & G_APPLICATION_IS_SERVICE) {
		windows = gtk_application_get_windows (GTK_APPLICATION (app));
		if (windows) {
			window = windows->data;
			gtk_widget_hide (window);
		}

		return;
	}

	g_application_quit (G_APPLICATION (app));
}

static void
activate_on_shell_loaded_cb (GsActivationHelper *helper)
{
	GsApplication *app = helper->app;

	g_action_activate (G_ACTION (helper->action), helper->action_param);

	g_signal_handlers_disconnect_by_data (app->shell, helper);
	gs_activation_helper_free (helper);
}

static void
set_mode_activated (GSimpleAction *action,
		    GVariant      *parameter,
		    gpointer       data)
{
	GsApplication *app = GS_APPLICATION (data);
	const gchar *mode;

	gs_application_present_window (app, NULL);

	gs_shell_reset_state (app->shell);

	mode = g_variant_get_string (parameter, NULL);
	if (g_strcmp0 (mode, "updates") == 0) {
		gs_shell_set_mode (app->shell, GS_SHELL_MODE_UPDATES);
	} else if (g_strcmp0 (mode, "installed") == 0) {
		gs_shell_set_mode (app->shell, GS_SHELL_MODE_INSTALLED);
	} else if (g_strcmp0 (mode, "moderate") == 0) {
		gs_shell_set_mode (app->shell, GS_SHELL_MODE_MODERATE);
	} else if (g_strcmp0 (mode, "overview") == 0) {
		gs_shell_set_mode (app->shell, GS_SHELL_MODE_OVERVIEW);
	} else if (g_strcmp0 (mode, "updated") == 0) {
		gs_shell_set_mode (app->shell, GS_SHELL_MODE_UPDATES);
		gs_shell_show_installed_updates (app->shell);
	} else {
		g_warning ("Mode '%s' not recognised", mode);
	}
}

static void
search_activated (GSimpleAction *action,
		  GVariant      *parameter,
		  gpointer       data)
{
	GsApplication *app = GS_APPLICATION (data);
	const gchar *search;

	gs_application_present_window (app, NULL);

	search = g_variant_get_string (parameter, NULL);
	gs_shell_reset_state (app->shell);
	gs_shell_show_search (app->shell, search);
}

static void
_search_launchable_details_cb (GObject *source, GAsyncResult *res, gpointer user_data)
{
	GsApp *a;
	GsApplication *app = GS_APPLICATION (user_data);
	g_autoptr(GError) error = NULL;
	g_autoptr(GsAppList) list = NULL;

	list = gs_plugin_loader_job_process_finish (app->plugin_loader, res, &error);
	if (list == NULL) {
		g_warning ("failed to find application: %s", error->message);
		return;
	}
	if (gs_app_list_length (list) == 0) {
		gs_shell_set_mode (app->shell, GS_SHELL_MODE_OVERVIEW);
		gs_shell_show_notification (app->shell,
					    /* TRANSLATORS: we tried to show an app that did not exist */
					    _("Sorry! There are no details for that application."));
		return;
	}
	a = gs_app_list_index (list, 0);
	gs_shell_reset_state (app->shell);
	gs_shell_show_app (app->shell, a);
}

static void
gs_application_app_to_show_created_cb (GObject *source_object,
				       GAsyncResult *result,
				       gpointer user_data)
{
	GsApplication *gs_app = user_data;
	g_autoptr(GsApp) app = NULL;
	g_autoptr(GError) error = NULL;

	app = gs_plugin_loader_app_create_finish (GS_PLUGIN_LOADER (source_object), result, &error);
	if (app == NULL) {
		if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) &&
		    !g_error_matches (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_CANCELLED))
			g_warning ("Failed to create application: %s", error->message);
	} else {
		g_return_if_fail (GS_IS_APPLICATION (gs_app));

		gs_shell_reset_state (gs_app->shell);
		gs_shell_show_app (gs_app->shell, app);
	}
}

static void
details_activated (GSimpleAction *action,
		   GVariant      *parameter,
		   gpointer       data)
{
	GsApplication *app = GS_APPLICATION (data);
	const gchar *id;
	const gchar *search;

	gs_application_present_window (app, NULL);

	g_variant_get (parameter, "(&s&s)", &id, &search);
	g_debug ("trying to activate %s:%s for details", id, search);
	if (search != NULL && search[0] != '\0') {
		gs_shell_reset_state (app->shell);
		gs_shell_show_search_result (app->shell, id, search);
	} else {
		g_autofree gchar *data_id = NULL;
		g_autoptr(GsPluginJob) plugin_job = NULL;
		data_id = gs_utils_unique_id_compat_convert (id);
		if (data_id != NULL) {
			gs_plugin_loader_app_create_async (app->plugin_loader, data_id, app->cancellable,
				gs_application_app_to_show_created_cb, app);
			return;
		}

		/* find by launchable */
		plugin_job = gs_plugin_job_newv (GS_PLUGIN_ACTION_SEARCH,
						 "search", id,
						 "refine-flags", GS_PLUGIN_REFINE_FLAGS_REQUIRE_ICON,
						 "dedupe-flags", GS_APP_LIST_FILTER_FLAG_PREFER_INSTALLED |
								 GS_APP_LIST_FILTER_FLAG_KEY_ID_PROVIDES,
						 NULL);
		gs_plugin_loader_job_process_async (app->plugin_loader, plugin_job,
						    app->cancellable,
						    _search_launchable_details_cb,
						    app);
	}
}

static void
details_pkg_activated (GSimpleAction *action,
		       GVariant      *parameter,
		       gpointer       data)
{
	GsApplication *app = GS_APPLICATION (data);
	const gchar *name;
	const gchar *plugin;
	g_autoptr (GsApp) a = NULL;

	gs_application_present_window (app, NULL);

	g_variant_get (parameter, "(&s&s)", &name, &plugin);
	a = gs_app_new (NULL);
	gs_app_add_source (a, name);
	if (strcmp (plugin, "") != 0)
		gs_app_set_management_plugin (a, plugin);
	gs_shell_reset_state (app->shell);
	gs_shell_show_app (app->shell, a);
}

static void
details_url_activated (GSimpleAction *action,
		       GVariant      *parameter,
		       gpointer       data)
{
	GsApplication *app = GS_APPLICATION (data);
	const gchar *url;
	g_autoptr (GsApp) a = NULL;

	gs_application_present_window (app, NULL);

	g_variant_get (parameter, "(&s)", &url);

	/* this is only used as a wrapper to transport the URL to
	 * the gs_shell_change_mode() function -- not in the GsAppList */
	a = gs_app_new (NULL);
	gs_app_set_metadata (a, "GnomeSoftware::from-url", url);
	gs_shell_reset_state (app->shell);
	gs_shell_show_app (app->shell, a);
}

typedef struct {
	GWeakRef gs_app_weakref;
	gchar *data_id;
	GsShellInteraction interaction;
} InstallActivatedHelper;

static void
gs_application_app_to_install_created_cb (GObject *source_object,
					  GAsyncResult *result,
					  gpointer user_data)
{
	InstallActivatedHelper *helper = user_data;
	g_autoptr(GsApp) app = NULL;
	g_autoptr(GError) error = NULL;

	app = gs_plugin_loader_app_create_finish (GS_PLUGIN_LOADER (source_object), result, &error);
	if (app == NULL) {
		if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) &&
		    !g_error_matches (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_CANCELLED))
			g_warning ("Failed to create application '%s': %s", helper->data_id, error->message);
	} else {
		g_autoptr(GsApplication) gs_app = NULL;

		gs_app = g_weak_ref_get (&helper->gs_app_weakref);
		if (gs_app != NULL) {
			gs_shell_reset_state (gs_app->shell);
			gs_shell_install (gs_app->shell, app, helper->interaction);
		}
	}

	g_weak_ref_clear (&helper->gs_app_weakref);
	g_free (helper->data_id);
	g_slice_free (InstallActivatedHelper, helper);
}

static void
install_activated (GSimpleAction *action,
		   GVariant      *parameter,
		   gpointer       data)
{
	GsApplication *app = GS_APPLICATION (data);
	const gchar *id;
	GsShellInteraction interaction;
	InstallActivatedHelper *helper;
	g_autoptr (GsApp) a = NULL;
	g_autofree gchar *data_id = NULL;

	g_variant_get (parameter, "(&su)", &id, &interaction);
	data_id = gs_utils_unique_id_compat_convert (id);
	if (data_id == NULL) {
		g_warning ("Need to use a valid unique-id: %s", id);
		return;
	}

	if (interaction == GS_SHELL_INTERACTION_FULL)
		gs_application_present_window (app, NULL);

	helper = g_slice_new0 (InstallActivatedHelper);
	g_weak_ref_init (&helper->gs_app_weakref, app);
	helper->data_id = g_strdup (data_id);
	helper->interaction = interaction;

	gs_plugin_loader_app_create_async (app->plugin_loader, data_id, app->cancellable,
		gs_application_app_to_install_created_cb, helper);
}

static GFile *
_copy_file_to_cache (GFile *file_src, GError **error)
{
	g_autoptr(GFile) file_dest = NULL;
	g_autofree gchar *cache_dir = NULL;
	g_autofree gchar *cache_fn = NULL;
	g_autofree gchar *basename = NULL;

	/* get destination location */
	cache_dir = g_dir_make_tmp ("gnome-software-XXXXXX", error);
	if (cache_dir == NULL)
		return NULL;
	basename = g_file_get_basename (file_src);
	cache_fn = g_build_filename (cache_dir, basename, NULL);

	/* copy file to cache */
	file_dest = g_file_new_for_path (cache_fn);
	if (!g_file_copy (file_src, file_dest,
			  G_FILE_COPY_OVERWRITE,
			  NULL, /* cancellable */
			  NULL, NULL, /* progress */
			  error)) {
		return NULL;
	}
	return g_steal_pointer (&file_dest);
}

static void
filename_activated (GSimpleAction *action,
		    GVariant      *parameter,
		    gpointer       data)
{
	GsApplication *app = GS_APPLICATION (data);
	const gchar *filename;
	g_autoptr(GFile) file = NULL;

	g_variant_get (parameter, "(&s)", &filename);

	/* this could go away at any moment, so make a local copy */
	if (g_str_has_prefix (filename, "/tmp") ||
	    g_str_has_prefix (filename, "/var/tmp")) {
		g_autoptr(GError) error = NULL;
		g_autoptr(GFile) file_src = g_file_new_for_path (filename);
		file = _copy_file_to_cache (file_src, &error);
		if (file == NULL) {
			g_warning ("failed to copy file, falling back to %s: %s",
				   filename, error->message);
			file = g_file_new_for_path (filename);
		}
	} else {
		file = g_file_new_for_path (filename);
	}
	gs_shell_reset_state (app->shell);
	gs_shell_show_local_file (app->shell, file);
}

static void
launch_activated (GSimpleAction *action,
		  GVariant      *parameter,
		  gpointer       data)
{
	GsApplication *self = GS_APPLICATION (data);
	GsApp *app = NULL;
	const gchar *id, *management_plugin;
	g_autoptr(GsAppList) list = NULL;
	g_autoptr(GsPluginJob) search_job = NULL;
	g_autoptr(GsPluginJob) launch_job = NULL;
	g_autoptr(GError) error = NULL;
	guint ii, len;

	g_variant_get (parameter, "(&s&s)", &id, &management_plugin);

	search_job = gs_plugin_job_newv (GS_PLUGIN_ACTION_SEARCH,
					 "search", id,
					 "refine-flags", GS_PLUGIN_REFINE_FLAGS_REQUIRE_DESCRIPTION |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_PERMISSIONS |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_RUNTIME,
					 NULL);
	list = gs_plugin_loader_job_process (self->plugin_loader, search_job, self->cancellable, &error);
	if (!list) {
		g_warning ("Failed to search for application '%s' (from '%s'): %s", id, management_plugin, error ? error->message : "Unknown error");
		return;
	}

	len = gs_app_list_length (list);
	for (ii = 0; ii < len && !app; ii++) {
		GsApp *list_app = gs_app_list_index (list, ii);

		if (gs_app_is_installed (list_app) &&
		    g_strcmp0 (gs_app_get_management_plugin (list_app), management_plugin) == 0)
			app = list_app;
	}

	if (!app) {
		g_warning ("Did not find application '%s' from '%s'", id, management_plugin);
		return;
	}

	launch_job = gs_plugin_job_newv (GS_PLUGIN_ACTION_LAUNCH,
					 "app", app,
					 NULL);
	if (!gs_plugin_loader_job_action (self->plugin_loader, launch_job, self->cancellable, &error)) {
		g_warning ("Failed to launch app: %s", error->message);
		return;
	}
}

static void
show_offline_updates_error (GSimpleAction *action,
			    GVariant      *parameter,
			    gpointer       data)
{
	GsApplication *app = GS_APPLICATION (data);

	gs_application_present_window (app, NULL);

	gs_shell_reset_state (app->shell);
	gs_shell_set_mode (app->shell, GS_SHELL_MODE_UPDATES);
	gs_update_monitor_show_error (app->update_monitor, app->main_window);
}

static void
autoupdate_activated (GSimpleAction *action, GVariant *parameter, gpointer data)
{
	GsApplication *app = GS_APPLICATION (data);
	gs_shell_reset_state (app->shell);
	gs_shell_set_mode (app->shell, GS_SHELL_MODE_UPDATES);
	gs_update_monitor_autoupdate (app->update_monitor);
}

static void
install_resources_activated (GSimpleAction *action,
                             GVariant      *parameter,
                             gpointer       data)
{
	GsApplication *app = GS_APPLICATION (data);
	GdkDisplay *display;
	const gchar *mode;
	const gchar *startup_id;
	const gchar *desktop_id;
	const gchar *ident;
	g_autofree gchar **resources = NULL;

	g_variant_get (parameter, "(&s^a&s&s&s&s)", &mode, &resources, &startup_id, &desktop_id, &ident);

	display = gdk_display_get_default ();
#ifdef GDK_WINDOWING_X11
	if (GDK_IS_X11_DISPLAY (display)) {
		if (startup_id != NULL && startup_id[0] != '\0')
			gdk_x11_display_set_startup_notification_id (display,
			                                             startup_id);
	}
#endif
#ifdef GDK_WINDOWING_WAYLAND
	if (GDK_IS_WAYLAND_DISPLAY (display)) {
		if (startup_id != NULL && startup_id[0] != '\0')
			gdk_wayland_display_set_startup_notification_id (display,
			                                                 startup_id);
	}
#endif

	gs_application_present_window (app, startup_id);

	gs_shell_reset_state (app->shell);
	gs_shell_show_extras_search (app->shell, mode, resources, desktop_id, ident);
}

static GActionEntry actions[] = {
	{ "about", about_activated, NULL, NULL, NULL },
	{ "quit", quit_activated, NULL, NULL, NULL },
	{ "reboot-and-install", reboot_and_install, NULL, NULL, NULL },
	{ "reboot", reboot_activated, NULL, NULL, NULL },
	{ "shutdown", shutdown_activated, NULL, NULL, NULL },
	{ "launch", launch_activated, "(ss)", NULL, NULL },
	{ "show-offline-update-error", show_offline_updates_error, NULL, NULL, NULL },
	{ "autoupdate", autoupdate_activated, NULL, NULL, NULL },
	{ "nop", NULL, NULL, NULL }
};

static GActionEntry actions_after_loading[] = {
	{ "sources", sources_activated, NULL, NULL, NULL },
	{ "prefs", prefs_activated, NULL, NULL, NULL },
	{ "set-mode", set_mode_activated, "s", NULL, NULL },
	{ "search", search_activated, "s", NULL, NULL },
	{ "details", details_activated, "(ss)", NULL, NULL },
	{ "details-pkg", details_pkg_activated, "(ss)", NULL, NULL },
	{ "details-url", details_url_activated, "(s)", NULL, NULL },
	{ "install", install_activated, "(su)", NULL, NULL },
	{ "filename", filename_activated, "(s)", NULL, NULL },
	{ "install-resources", install_resources_activated, "(sassss)", NULL, NULL },
	{ "nop", NULL, NULL, NULL }
};

static void
gs_application_update_software_sources_presence (GApplication *self)
{
	GsApplication *app = GS_APPLICATION (self);
	GSimpleAction *action;
	gboolean enable_sources;

	action = G_SIMPLE_ACTION (g_action_map_lookup_action (G_ACTION_MAP (self),
							      "sources"));
	enable_sources = g_settings_get_boolean (app->settings,
						 ENABLE_REPOS_DIALOG_CONF_KEY);
	g_simple_action_set_enabled (action, enable_sources);
}

static void
gs_application_settings_changed_cb (GApplication *self,
				    const gchar *key,
				    gpointer data)
{
	if (g_strcmp0 (key, ENABLE_REPOS_DIALOG_CONF_KEY) == 0) {
		gs_application_update_software_sources_presence (self);
	}
}

static void
gs_application_setup_search_provider (GsApplication *app)
{
	gs_application_initialize_plugins (app);
	if (app->search_provider)
		gs_shell_search_provider_setup (app->search_provider, app->plugin_loader);
}

static void
wrapper_action_activated_cb (GSimpleAction *action,
			     GVariant	   *parameter,
			     gpointer	    data)
{
	GsApplication *app = GS_APPLICATION (data);
	const gchar *action_name = g_action_get_name (G_ACTION (action));
	GAction *real_action = g_action_map_lookup_action (G_ACTION_MAP (app->action_map),
							   action_name);

	if (app->shell_loaded_handler_id != 0) {
		GsActivationHelper *helper = gs_activation_helper_new (app,
								       G_SIMPLE_ACTION (real_action),
								       parameter);

		g_signal_connect_swapped (app->shell, "loaded",
					  G_CALLBACK (activate_on_shell_loaded_cb), helper);
		return;
	}

	g_action_activate (real_action, parameter);
}

static void
gs_application_add_wrapper_actions (GApplication *application)
{
	GsApplication *app = GS_APPLICATION (application);
	GActionMap *map = NULL;

	app->action_map = g_simple_action_group_new ();
	map = G_ACTION_MAP (app->action_map);

	/* add the real actions to a different map and add wrapper actions to the
	 * application instead; the wrapper actions will call the real ones but
	 * after the "loading state" has finished */

	g_action_map_add_action_entries (G_ACTION_MAP (map), actions_after_loading,
					 G_N_ELEMENTS (actions_after_loading),
					 application);

	for (guint i = 0; i < G_N_ELEMENTS (actions_after_loading); ++i) {
		const GActionEntry *entry = &actions_after_loading[i];
		GAction *action = g_action_map_lookup_action (map, entry->name);
		g_autoptr (GSimpleAction) simple_action = NULL;

		simple_action = g_simple_action_new (g_action_get_name (action),
						     g_action_get_parameter_type (action));
		g_signal_connect (simple_action, "activate",
				  G_CALLBACK (wrapper_action_activated_cb),
				  application);
		g_object_bind_property (simple_action, "enabled", action,
					"enabled", G_BINDING_DEFAULT);
		g_action_map_add_action (G_ACTION_MAP (application),
					 G_ACTION (simple_action));
	}
}

static void
gs_application_startup (GApplication *application)
{
	GSettings *settings;
	GsApplication *app = GS_APPLICATION (application);
	G_APPLICATION_CLASS (gs_application_parent_class)->startup (application);

	hdy_init ();

	gs_application_add_wrapper_actions (application);

	g_action_map_add_action_entries (G_ACTION_MAP (application),
					 actions, G_N_ELEMENTS (actions),
					 application);

	gs_application_setup_search_provider (GS_APPLICATION (application));

#ifdef HAVE_PACKAGEKIT
	GS_APPLICATION (application)->dbus_helper = gs_dbus_helper_new ();
#endif
	settings = g_settings_new ("org.gnome.software");
	GS_APPLICATION (application)->settings = settings;
	g_signal_connect_swapped (settings, "changed",
				  G_CALLBACK (gs_application_settings_changed_cb),
				  application);

	gs_application_initialize_ui (app);

	GS_APPLICATION (application)->update_monitor =
		gs_update_monitor_new (GS_APPLICATION (application));
	gs_folders_convert ();

	gs_application_update_software_sources_presence (application);
}

static void
gs_application_activate (GApplication *application)
{
	GsApplication *app = GS_APPLICATION (application);

	if (app->shell_loaded_handler_id == 0)
		gs_shell_set_mode (app->shell, GS_SHELL_MODE_OVERVIEW);

	gs_shell_activate (GS_APPLICATION (application)->shell);

	gs_application_show_first_run_dialog (GS_APPLICATION (application));
}

static void
gs_application_constructed (GObject *object)
{
	GsApplication *self = GS_APPLICATION (object);

	G_OBJECT_CLASS (gs_application_parent_class)->constructed (object);

	/* This is needed when the the application's ID isn't
	 * org.gnome.Software, e.g. for the development profile (when
	 * `BUILD_PROFILE` is defined). Without this, icon resources can't
	 * be loaded appropriately. */
	g_application_set_resource_base_path (G_APPLICATION (self),
					      "/org/gnome/Software");

	/* Check on our construct-only properties */
	g_assert (self->debug != NULL);
}

static void
gs_application_get_property (GObject    *object,
                             guint       prop_id,
                             GValue     *value,
                             GParamSpec *pspec)
{
	GsApplication *self = GS_APPLICATION (object);

	switch ((GsApplicationProperty) prop_id) {
	case PROP_DEBUG:
		g_value_set_object (value, self->debug);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
gs_application_set_property (GObject      *object,
                             guint         prop_id,
                             const GValue *value,
                             GParamSpec   *pspec)
{
	GsApplication *self = GS_APPLICATION (object);

	switch ((GsApplicationProperty) prop_id) {
	case PROP_DEBUG:
		/* Construct only */
		g_assert (self->debug == NULL);
		self->debug = g_value_dup_object (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
gs_application_dispose (GObject *object)
{
	GsApplication *app = GS_APPLICATION (object);

	g_cancellable_cancel (app->cancellable);
	g_clear_object (&app->cancellable);

	g_clear_object (&app->plugin_loader);
	g_clear_object (&app->shell);
	g_clear_object (&app->provider);
	g_clear_object (&app->update_monitor);
#ifdef HAVE_PACKAGEKIT
	g_clear_object (&app->dbus_helper);
#endif
	g_clear_object (&app->settings);
	g_clear_object (&app->action_map);
	g_clear_object (&app->debug);

	G_OBJECT_CLASS (gs_application_parent_class)->dispose (object);
}

static GsShellInteraction
get_page_interaction_from_string (const gchar *interaction)
{
	if (g_strcmp0 (interaction, "notify") == 0)
		return GS_SHELL_INTERACTION_NOTIFY;
	else if (g_strcmp0 (interaction, "none") == 0)
		return GS_SHELL_INTERACTION_NONE;
	return GS_SHELL_INTERACTION_FULL;
}

static int
gs_application_handle_local_options (GApplication *app, GVariantDict *options)
{
	GsApplication *self = GS_APPLICATION (app);
	const gchar *id;
	const gchar *pkgname;
	const gchar *local_filename;
	const gchar *mode;
	const gchar *search;
	gint rc = -1;
	g_autoptr(GError) error = NULL;

	gs_debug_set_verbose (self->debug, g_variant_dict_contains (options, "verbose"));

	/* prefer local sources */
	if (g_variant_dict_contains (options, "prefer-local"))
		g_setenv ("GNOME_SOFTWARE_PREFER_LOCAL", "true", TRUE);

	if (g_variant_dict_contains (options, "version")) {
		g_print ("gnome-software %s\n", get_version());
		return 0;
	}

	if (!g_application_register (app, NULL, &error)) {
		g_printerr ("%s\n", error->message);
		return 1;
	}

	if (g_variant_dict_contains (options, "autoupdate")) {
		g_action_group_activate_action (G_ACTION_GROUP (app),
						"autoupdate",
						NULL);
	}
	if (g_variant_dict_contains (options, "prefs")) {
		g_action_group_activate_action (G_ACTION_GROUP (app),
						"prefs",
						NULL);
	}
	if (g_variant_dict_contains (options, "quit")) {
		/* The 'quit' command-line option shuts down everything,
		 * including the backend service */
		g_action_group_activate_action (G_ACTION_GROUP (app),
						"shutdown",
						NULL);
		return 0;
	}

	if (g_variant_dict_lookup (options, "mode", "&s", &mode)) {
		g_action_group_activate_action (G_ACTION_GROUP (app),
						"set-mode",
						g_variant_new_string (mode));
		rc = 0;
	} else if (g_variant_dict_lookup (options, "search", "&s", &search)) {
		g_action_group_activate_action (G_ACTION_GROUP (app),
						"search",
						g_variant_new_string (search));
		rc = 0;
	} else if (g_variant_dict_lookup (options, "details", "&s", &id)) {
		g_action_group_activate_action (G_ACTION_GROUP (app),
						"details",
						g_variant_new ("(ss)", id, ""));
		rc = 0;
	} else if (g_variant_dict_lookup (options, "details-pkg", "&s", &pkgname)) {
		g_action_group_activate_action (G_ACTION_GROUP (app),
						"details-pkg",
						g_variant_new ("(ss)", pkgname, ""));
		rc = 0;
	} else if (g_variant_dict_lookup (options, "install", "&s", &id)) {
		GsShellInteraction interaction = GS_SHELL_INTERACTION_FULL;
		const gchar *str_interaction = NULL;

		if (g_variant_dict_lookup (options, "interaction", "&s",
					   &str_interaction))
			interaction = get_page_interaction_from_string (str_interaction);

		g_action_group_activate_action (G_ACTION_GROUP (app),
						"install",
						g_variant_new ("(su)", id,
							       interaction));
		rc = 0;
	} else if (g_variant_dict_lookup (options, "local-filename", "^&ay", &local_filename)) {
		g_autoptr(GFile) file = NULL;
		g_autofree gchar *absolute_filename = NULL;

		file = g_file_new_for_path (local_filename);
		absolute_filename = g_file_get_path (file);
		g_action_group_activate_action (G_ACTION_GROUP (app),
						"filename",
						g_variant_new ("(s)", absolute_filename));
		rc = 0;
	}

	return rc;
}

static void
gs_application_open (GApplication  *application,
                     GFile        **files,
                     gint           n_files,
                     const gchar   *hint)
{
	GsApplication *app = GS_APPLICATION (application);
	gint i;

	for (i = 0; i < n_files; i++) {
		g_autofree gchar *str = g_file_get_uri (files[i]);
		g_action_group_activate_action (G_ACTION_GROUP (app),
						"details-url",
						g_variant_new ("(s)", str));
	}
}

static void
gs_application_class_init (GsApplicationClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GApplicationClass *application_class = G_APPLICATION_CLASS (klass);

	object_class->constructed = gs_application_constructed;
	object_class->get_property = gs_application_get_property;
	object_class->set_property = gs_application_set_property;
	object_class->dispose = gs_application_dispose;

	application_class->startup = gs_application_startup;
	application_class->activate = gs_application_activate;
	application_class->handle_local_options = gs_application_handle_local_options;
	application_class->open = gs_application_open;
	application_class->dbus_register = gs_application_dbus_register;
	application_class->dbus_unregister = gs_application_dbus_unregister;

	/**
	 * GsApplication:debug: (nullable)
	 *
	 * A #GsDebug object to control debug and logging output from the
	 * application and everything within it.
	 *
	 * This may be %NULL if you don’t care about log output.
	 *
	 * Since: 40
	 */
	obj_props[PROP_DEBUG] =
		g_param_spec_object ("debug", NULL, NULL,
				     GS_TYPE_DEBUG,
				     G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

	g_object_class_install_properties (object_class, G_N_ELEMENTS (obj_props), obj_props);

	/**
	 * GsApplication::install-resources-done:
	 * @ident: Operation identificator, as string
	 * @op_error: (nullable): an install #GError, or %NULL on success
	 *
	 * Emitted after a resource installation operation identified by @ident
	 * had finished. The @op_error can hold eventual error message, when
	 * the installation failed.
	 */
	signals[INSTALL_RESOURCES_DONE] = g_signal_new (
		"install-resources-done",
		G_TYPE_FROM_CLASS (klass),
		G_SIGNAL_RUN_LAST | G_SIGNAL_NO_RECURSE,
		0,
		NULL, NULL,
		NULL,
		G_TYPE_NONE, 2,
		G_TYPE_STRING, G_TYPE_ERROR);

	/**
	 * GsApplication::repository-changed:
	 * @repository: a #GsApp of the repository
	 *
	 * Emitted when the repository changed, usually when it is enabled or disabled.
	 *
	 * Since: 40
	 */
	signals[REPOSITORY_CHANGED] = g_signal_new (
		"repository-changed",
		G_TYPE_FROM_CLASS (klass),
		G_SIGNAL_ACTION | G_SIGNAL_NO_RECURSE,
		0,
		NULL, NULL,
		NULL,
		G_TYPE_NONE, 1,
		GS_TYPE_APP);
}

/**
 * gs_application_new:
 * @debug: (transfer none) (not nullable): a #GsDebug for the application instance
 *
 * Create a new #GsApplication.
 *
 * Returns: (transfer full): a new #GsApplication
 * Since: 40
 */
GsApplication *
gs_application_new (GsDebug *debug)
{
	return g_object_new (GS_APPLICATION_TYPE,
			     "application-id", APPLICATION_ID,
			     "flags", G_APPLICATION_HANDLES_OPEN,
			     "inactivity-timeout", 12000,
			     "debug", debug,
			     NULL);
}

void
gs_application_emit_install_resources_done (GsApplication *application,
					    const gchar *ident,
					    const GError *op_error)
{
	g_signal_emit (application, signals[INSTALL_RESOURCES_DONE], 0, ident, op_error, NULL);
}
