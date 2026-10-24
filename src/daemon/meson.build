daemon_sources = files(
  'tc_daemon.c', 'tc_temp_job.c',
  'tc_dispatcher.c',
  'tc_governor.c',
  'tc_worker_pool.c',
  'tc_disk_guard.c'
)

daemon_bin = executable(
  'teraconvert-daemon',
  daemon_sources,
  include_directories: [inc, config_inc, include_directories('../engines')],
  link_with: [libtc_engines],
  dependencies: [
    glib_dep,
    gio_dep,
    gio_unix_dep,
    gobject_dep,
    threads_dep,
    m_dep,
    dl_dep,
    libnotify_dep
  ],
  install: true,
  install_dir: get_option('bindir')
)
