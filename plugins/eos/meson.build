cargs = ['-DG_LOG_DOMAIN="GsPluginEos"']

ostree = dependency('ostree-1')

shared_module(
  'gs_plugin_eos',
  sources : [
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
