worker_test_env = {'TC_JOB_STATE_DIR': meson.current_build_dir() / 'job-state', 'TC_HELPER_DIR': meson.project_source_root() / 'src' / 'scripts'}
test_ipc_bin = executable(
  'test_ipc',
  'test_ipc.c',
  include_directories: [inc, config_inc],
  dependencies: [
    glib_dep,
    gio_dep,
    gio_unix_dep,
    gobject_dep,
    m_dep
  ],
  install: false
)

test('IPC Protocol Tests', test_ipc_bin, suite: 'unit')

test_governor_bin = executable(
  'test_governor', ['test_governor.c', '../src/daemon/tc_governor.c', '../src/daemon/tc_disk_guard.c'],
  link_args: ['-Wl,--wrap=fstat', '-Wl,--wrap=fstatvfs', '-Wl,--wrap=fstat64', '-Wl,--wrap=fstatvfs64'],
  include_directories: [inc, config_inc, include_directories('../src/daemon')],
  dependencies: [glib_dep, gio_dep, gobject_dep, threads_dep], install: false
)
test('Resource Admission Regression', test_governor_bin, suite: 'regression', timeout: 20)

test_process_runner_bin = executable(
  'test_process_runner', ['test_process_runner.c', '../src/engines/tc_process_runner.c'],
  include_directories: [inc, config_inc],
  dependencies: [glib_dep, gio_dep, gobject_dep], install: false
)
test('Process Runner Regression', test_process_runner_bin, suite: 'regression', timeout: 20)

test_media_padding_bin = executable(
  'test_media_padding', ['test_media_padding.c', '../src/engines/tc_media_padding.c'],
  include_directories: [inc, config_inc, include_directories('../src/engines')],
  dependencies: [glib_dep, gio_dep, gobject_dep], install: false
)
test('MP4 Padding Safety Regression', test_media_padding_bin, suite: 'regression', timeout: 15)

if get_option('daemon') and avcodec_dep.found() and avformat_dep.found()
  test_media_probe_bin = executable(
    'test_media_probe', ['test_media_probe.c', '../src/engines/tc_media_probe.c',
                         '../src/engines/tc_process_runner.c', '../src/engines/tc_media_ffmpeg.c', '../src/engines/tc_media_plan.c', '../src/engines/tc_media_estimate.c',
                         '../src/engines/tc_media_padding.c'],
    link_args: ['-Wl,--wrap=tc_process_run'],
    include_directories: [inc, config_inc, include_directories('../src/engines')],
    dependencies: [glib_dep, gio_dep, gobject_dep, avcodec_dep, avformat_dep, avutil_dep, m_dep], install: false
  )
  test('Media Probe Regression', test_media_probe_bin, suite: 'regression', timeout: 30)
endif

if get_option('daemon') and vips_dep.found() and poppler_dep.found() and archive_dep.found() and avcodec_dep.found() and avformat_dep.found()
  test_e2e_bin = executable(
    'test_e2e_conversion',
    'test_e2e_conversion.c',
    include_directories: [inc, config_inc, include_directories('../src/engines')],
    link_with: [libtc_engines],
    dependencies: [
      glib_dep,
      gio_dep,
      gio_unix_dep,
      gobject_dep,
      m_dep,
      dl_dep,
      vips_dep
    ],
    install: false
  )
  
  test('End-to-End Engine Tests', test_e2e_bin, suite: 'engines')
endif

if get_option('daemon') and vips_dep.found() and avcodec_dep.found() and avformat_dep.found()
  test_worker_pool_bin = executable(
    'test_worker_pool',
    [
      'test_worker_pool.c',
      '../src/daemon/tc_worker_pool.c', '../src/daemon/tc_temp_job.c',
      '../src/daemon/tc_disk_guard.c',
      '../src/daemon/tc_governor.c'
    ],
    include_directories: [inc, config_inc, include_directories('../src/engines'), include_directories('../src/daemon')],
    link_with: [libtc_engines],
    dependencies: [
      glib_dep,
      gio_dep,
      gio_unix_dep,
      gobject_dep,
      m_dep,
      dl_dep,
      vips_dep,
      threads_dep,
      libnotify_dep
    ],
    install: false
  )
  
  test('Worker Pool & Safety Guard Tests', test_worker_pool_bin, env: worker_test_env)
endif

if get_option('daemon') and archive_dep.found()
  test_office_worker_bin = executable(
    'test_office_worker',
    ['test_office_worker.c', '../src/daemon/tc_worker_pool.c', '../src/daemon/tc_temp_job.c',
     '../src/daemon/tc_disk_guard.c', '../src/daemon/tc_governor.c'],
    include_directories: [inc, config_inc, include_directories('../src/engines'), include_directories('../src/daemon')],
    link_with: [libtc_engines],
    dependencies: [glib_dep, gio_dep, gio_unix_dep, gobject_dep, threads_dep, m_dep, dl_dep, libnotify_dep],
    install: false
  )
  test('Office Worker Regression', test_office_worker_bin, suite: 'regression', env: worker_test_env)
endif

test_worker_lifecycle_bin = executable(
  'test_worker_lifecycle',
  ['test_worker_lifecycle.c', '../src/daemon/tc_worker_pool.c', '../src/daemon/tc_temp_job.c', '../src/engines/tc_process_runner.c',
   '../src/daemon/tc_disk_guard.c', '../src/daemon/tc_governor.c'],
  include_directories: [inc, config_inc, include_directories('../src/engines'), include_directories('../src/daemon')],
  dependencies: [glib_dep, gio_dep, gio_unix_dep, gobject_dep, threads_dep, m_dep, libnotify_dep],
  install: false
)
test('Worker Lifetime and Output Safety', test_worker_lifecycle_bin, suite: 'regression', timeout: 15, env: worker_test_env)

test_dispatcher_bin = executable(
  'test_dispatcher', ['test_dispatcher.c', '../src/daemon/tc_dispatcher.c'],
  link_args: ['-Wl,--wrap=getsockopt'],
  include_directories: [inc, config_inc, include_directories('../src/daemon'), include_directories('../src/engines')],
  dependencies: [glib_dep, gio_dep, gio_unix_dep, gobject_dep], install: false
)
test('Async Dispatcher Regression', test_dispatcher_bin, suite: 'regression', timeout: 10)

if get_option('daemon') and zlib_dep.found() and jpeg_dep.found()
  image_preserve_test = executable('test_image_preserve',
    ['test_image_preserve.c', '../src/engines/tc_image_preserve.c'],
    include_directories: [inc, config_inc, include_directories('../src/engines')],
    dependencies: [glib_dep, gio_dep, zlib_dep, jpeg_dep])
  test('Lossless Image Regression', image_preserve_test, suite: 'regression', timeout: 30)
endif

temp_job_test = executable('test_temp_job',
  ['test_temp_job.c', '../src/daemon/tc_temp_job.c'],
  include_directories: [inc, config_inc], dependencies: [glib_dep, gio_dep])
test('Owned Temp Recovery Regression', temp_job_test, suite: 'regression')

if get_option('daemon') and poppler_dep.found()
  pdf_preserve_test = executable('test_pdf_preserve',
    ['test_pdf_preserve.c', '../src/engines/tc_pdf_preserve.c', '../src/engines/tc_process_runner.c'],
    include_directories: [inc, config_inc, include_directories('../src/engines')],
    dependencies: [glib_dep, gio_dep])
  test('Lossless PDF Regression', pdf_preserve_test, suite: 'regression', timeout: 30,
    env: {'TC_HELPER_DIR': meson.project_source_root() / 'src' / 'scripts'})
endif

if get_option('daemon') and avcodec_dep.found() and avformat_dep.found()
  audio_preserve_test = executable('test_audio_preserve', 'test_audio_preserve.c',
    include_directories: [inc, config_inc, include_directories('../src/engines')],
    link_with: [libtc_engines], dependencies: [glib_dep, gio_dep])
  test('Lossless Audio Regression', audio_preserve_test, suite: 'regression', timeout: 30)
endif

if get_option('daemon') and avcodec_dep.found() and avformat_dep.found()
  media_remux_test = executable('test_media_remux', 'test_media_remux.c',
    include_directories: [inc, config_inc, include_directories('../src/engines')],
    link_with: [libtc_engines], dependencies: [glib_dep, gio_dep])
  test('Validated Remux Regression', media_remux_test, suite: 'regression', timeout: 30)
endif

if get_option('daemon') and vips_dep.found() and poppler_dep.found() and archive_dep.found() and avcodec_dep.found() and zlib_dep.found() and jpeg_dep.found()
  test('Production IPC Integration', find_program('python3'),
    args: [meson.current_source_dir() / 'test_daemon_rpc.py', daemon_bin, cli_bin],
    suite: 'integration', timeout: 90)
endif
