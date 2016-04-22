AM_CPPFLAGS =						\
	-DG_LOG_DOMAIN=\"GsPluginEos\"			\
	-DSYSCONFDIR=\""$(sysconfdir)"\"		\
	-DTESTDATADIR=\""$(srcdir)/tests"\"		\
	-DLOCALPLUGINDIR=\""$(builddir)/.libs"\"	\
	-DLOCALPLUGINDIR_CORE=\""$(top_builddir)/plugins/core/.libs"\"

plugindir = $(GS_PLUGIN_DIR)
plugin_LTLIBRARIES = libgs_plugin_eos.la

libgs_plugin_eos_la_SOURCES =				\
	gs-plugin-eos.c
libgs_plugin_eos_la_LIBADD =				\
	$(GS_PLUGIN_LIBS)
libgs_plugin_eos_la_LDFLAGS = -module -avoid-version
libgs_plugin_eos_la_CFLAGS =				\
	$(GS_PLUGIN_CFLAGS)

-include $(top_srcdir)/git.mk
