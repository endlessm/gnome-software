cargs = ['-DG_LOG_DOMAIN="GsPluginEos"']

gdbus_eos_updater_src = gnome.gdbus_codegen(
  'eos-updater-generated',
  'eos-updater.xml',
  interface_prefix : 'com.endlessm.',
  namespace : 'Eos'
)

ostree = dependency('ostree-1')

shared_module(
  'gs_plugin_eos',
  gdbus_eos_updater_src,
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
  dependencies : [ plugin_libs, flatpak, ostree, ]
)
