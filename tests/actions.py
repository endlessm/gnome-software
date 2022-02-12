#! /usr/bin/python

# This a simple test, using the dogtail framework:
#
# Activate the app via the org.gtk.Application bus interface.
# Check that the expected actions are exported on the session bus.
# Activate each action and verify the result.

import os
from gi.repository import Gio

settings = Gio.Settings.new("org.gnome.desktop.interface")
settings.set_boolean ("toolkit-accessibility", True)

from dogtail.tree import *
from dogtail.utils import *
from dogtail.procedural import *
from dogtail.rawinput import keyCombo

try:
    run('gnome-software')

    app = root.application('gnome-software')

    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    proxy = Gio.DBusProxy.new_sync(bus, Gio.DBusProxyFlags.NONE,
                                   None,
                                   'org.gnome.Software',
                                   '/org/gnome/Software',
                                   'org.gtk.Application')
    proxy.call_sync('Activate', GLib.Variant('(a{sv})', ({},)), 0, -1, None)

    doDelay(1)
    try:
        shopping_button = app.child(name=u'Browse Software', retry=False)
        shopping_button.click()
    except tree.SearchError:
        print "not first-run, moving on"

    doDelay(1)
    assert (len(app.children) == 1)

    dbus_actions = Gio.DBusProxy.new_sync(bus, Gio.DBusProxyFlags.NONE,
                                          None,
                                          'org.gnome.Software',
                                          '/org/gnome/Software',
                                          'org.gtk.Actions')

    names = dbus_actions.call_sync('List', None, 0, -1, None).unpack()[0]
    assert (u'quit' in names)
    assert (u'about' in names)

    dbus_actions.call_sync('Activate',
                           GLib.Variant('(sava{sv})', (u'about', [], {})),
                           0, -1, None)

    doDelay (1)
    assert (len(app.children) == 2)
    keyCombo("<Esc>")
    doDelay (1)
    assert (len(app.children) == 1)

    dbus_actions.call_sync('Activate',
                           GLib.Variant('(sava{sv})', (u'quit', [], {})),
                           0, -1, None)
    assert (len(app.children) == 0)
finally:
    os.system("killall gnome-software")
