cargs = ['-DG_LOG_DOMAIN="GsPluginEosBlacklist"']

shared_module(
  'gs_plugin_eos-blacklist',
  sources : 'gs-plugin-eos-blacklist.c',
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
  ],
  link_with : [
    libgnomesoftware,
  ],
)
