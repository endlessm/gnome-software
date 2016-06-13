SUBDIRS =						\
	appdata						\
	tests						\
	icons

resourcesdir = $(datadir)/gnome-software
dist_resources_DATA =					\
	upgrade-bg.png					\
	featured-ardour.png				\
	featured-ardour-bg.png				\
	featured-blender.png				\
	featured-builder.png				\
	featured-builder-bg.jpg				\
	featured-chess.png				\
	featured-firefox.png				\
	featured-gimp.png				\
	featured-gnome-sudoku.png			\
	featured-inkscape.svg				\
	featured-maps.png           \
	featured-maps-bg.png        \
	featured-mypaint.png				\
	featured-polari.svg				\
	featured-transmission.png			\
	featured-weather.png				\
	featured-weather-bg.png

@GSETTINGS_RULES@
gsettings_SCHEMAS = org.gnome.software.gschema.xml

install-sample-data:
	wget -O fedora-20.xml.gz http://people.freedesktop.org/~hughsient/temp/fedora-20.xml.gz; \
	mkdir -p $(DESTDIR)$(datadir)/app-info/xmls; \
	cp fedora-20.xml.gz $(DESTDIR)$(datadir)/app-info/xmls/; \
	wget -O fedora-20-icons.tar.gz http://people.freedesktop.org/~hughsient/temp/fedora-20-icons.tar.gz; \
	mkdir -p $(DESTDIR)$(datadir)/app-info/icons/fedora-20; \
	cp fedora-20-icons.tar.gz $(DESTDIR)$(datadir)/app-info/icons/fedora-20; \
	cd $(DESTDIR)$(datadir)/app-info/icons/fedora-20; \
	tar xvzf fedora-20-icons.tar.gz &> /dev/null; \
	rm fedora-20-icons.tar.gz; \
	cd -

polkit_rulesdir = $(datadir)/polkit-1/rules.d
polkit_rules_DATA =					\
	com.endlessm.SystemExternalApps.rules

polkit_policydir = $(datadir)/polkit-1/actions
polkit_policy_in_files = com.endlessm.SystemExternalApps.policy.in
polkit_policy_DATA = $(polkit_policy_in_files:.policy.in=.policy)

$(polkit_policy_DATA): $(polkit_policy_in_files) Makefile
	sed -e "s|@libexecdir[@]|$(libexecdir)|g" $< > $@

EXTRA_DIST =						\
	$(gsettings_SCHEMAS)				\
	com.endlessm.SystemExternalApps.policy.in

CLEANFILES =						\
	com.endlessm.SystemExternalApps.policy

MAINTAINERCLEANFILES =					\
	*~						\
	Makefile.in

-include $(top_srcdir)/git.mk
