AM_CPPFLAGS =						\
	-DG_LOG_DOMAIN=\"GsPluginEos\"			\
	-DSYSCONFDIR=\""$(sysconfdir)"\"		\
	-DTESTDATADIR=\""$(srcdir)/tests"\"		\
	-DLOCALPLUGINDIR=\""$(builddir)/.libs"\"	\
	-DLOCALPLUGINDIR_CORE=\""$(top_builddir)/plugins/core/.libs"\"

plugindir = $(GS_PLUGIN_DIR)
plugin_LTLIBRARIES =					\
	libgs_plugin_eos.la				\
	libgs_plugin_legacy-external-apps.la		\
	libgs_plugin_external-runtimes-cleaner.la

libgs_plugin_eos_la_SOURCES =	\
	gs-appstream.c		\
	gs-appstream.h		\
	gs-flatpak.c		\
	gs-flatpak.h		\
	gs-flatpak-symlinks.c	\
	gs-flatpak-symlinks.h	\
	gs-plugin-eos.c
libgs_plugin_eos_la_LIBADD =				\
	$(GS_PLUGIN_LIBS)
libgs_plugin_eos_la_LDFLAGS = -module -avoid-version
libgs_plugin_eos_la_CFLAGS =				\
	$(GS_PLUGIN_CFLAGS) $(FLATPAK_CFLAGS)

libgs_plugin_legacy_external_apps_la_SOURCES =	\
	gs-appstream.c				\
	gs-appstream.h				\
	gs-flatpak.c				\
	gs-flatpak.h				\
	gs-flatpak-symlinks.c			\
	gs-flatpak-symlinks.h			\
	gs-legacy-external-apps.h		\
	gs-plugin-legacy-external-apps.c
libgs_plugin_legacy_external_apps_la_LIBADD = $(GS_PLUGIN_LIBS) $(FLATPAK_LIBS) $(JSON_GLIB_LIBS)
libgs_plugin_legacy_external_apps_la_LDFLAGS = -module -avoid-version
libgs_plugin_legacy_external_apps_la_CFLAGS = $(GS_PLUGIN_CFLAGS) $(JSON_GLIB_CFLAGS) $(WARN_CFLAGS) $(FLATPAK_CFLAGS)

libgs_plugin_external_runtimes_cleaner_la_SOURCES =		\
	gs-appstream.c				\
	gs-appstream.h				\
	gs-flatpak.c				\
	gs-flatpak.h				\
	gs-flatpak-symlinks.c			\
	gs-flatpak-symlinks.h			\
	gs-legacy-external-apps.h		\
	gs-plugin-external-runtimes-cleaner.c
libgs_plugin_external_runtimes_cleaner_la_LIBADD = $(GS_PLUGIN_LIBS) $(FLATPAK_LIBS) $(JSON_GLIB_LIBS)
libgs_plugin_external_runtimes_cleaner_la_LDFLAGS = -module -avoid-version
libgs_plugin_external_runtimes_cleaner_la_CFLAGS = $(GS_PLUGIN_CFLAGS) $(JSON_GLIB_CFLAGS) $(WARN_CFLAGS) $(FLATPAK_CFLAGS)

-include $(top_srcdir)/git.mk
