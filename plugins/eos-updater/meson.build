cargs = ['-DG_LOG_DOMAIN="GsPluginEosUpdater"']

eos_updater_generated = gnome.gdbus_codegen(
  'gs-eos-updater-generated',
  sources : ['com.endlessm.Updater.xml'],
  interface_prefix : 'com.endlessm.',
  namespace : 'GsEos',
)

shared_module(
  'gs_plugin_eos-updater',
  eos_updater_generated,
  sources : 'gs-plugin-eos-updater.c',
  include_directories : [
    include_directories('../..'),
    include_directories('../../lib'),
  ],
  install : true,
  install_dir: plugin_dir,
  c_args : cargs,
  dependencies : [
    plugin_libs,
    dependency('eosmetrics-0', version: '>= 0.5.0'),
    dependency('malcontent-0', version: '>= 0.7.0'),
    ostree,
  ],
  link_with : [
    libgnomesoftware,
  ],
)
