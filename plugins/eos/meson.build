cargs = ['-DG_LOG_DOMAIN="GsPluginEos"']

shared_module(
  'gs_plugin_eos',
  sources : [
    'gs-appstream.c',
    'gs-flatpak-app.c',
    'gs-flatpak.c',
    'gs-flatpak-symlinks.c',
    'gs-flatpak-utils.c',
    'gs-plugin-eos.c'
  ],
  include_directories : [
    include_directories('../..'),
    include_directories('../../lib'),
  ],
  install : true,
  install_dir: plugin_dir,
  c_args : cargs,
  dependencies : [ plugin_libs, flatpak ]
)
