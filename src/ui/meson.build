ui_sources = files(
  'tc_ui_main.c',
  'tc_ui_hud.c',
  'tc_ui_resize.c',
  'tc_ui_setup_ai.c',
  '../client/tc_client.c'
)

executable(
  'teraconvert-ui',
  ui_sources,
  include_directories: [inc, config_inc, include_directories('../client')],
  dependencies: [
    glib_dep,
    gio_dep,
    gio_unix_dep,
    gobject_dep,
    gtk4_dep,
    adw_dep,
    m_dep,
    libnotify_dep
  ],
  install: true,
  install_dir: get_option('bindir')
)
