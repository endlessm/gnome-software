cargs = ['-DG_LOG_DOMAIN="GsPluginEosBlocklist"']

shared_module(
  'gs_plugin_eos-blocklist',
  sources : 'gs-plugin-eos-blocklist.c',
  include_directories : [
    include_directories('../..'),
    include_directories('../../lib'),
  ],
  install : true,
  install_dir: plugin_dir,
  c_args : cargs,
  dependencies : [
    plugin_libs,
    flatpak,
    dependency('gnome-desktop-4', version : '>= 42.10'),
  ],
  link_with : [
    libgnomesoftware,
  ],
)
