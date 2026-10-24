project(
  'teraconvert-c',
  'c',
  version: '2.1.0',
  default_options: [
    'c_std=gnu11',
    'warning_level=2',
    'buildtype=debugoptimized',
    'b_ndebug=if-release'
  ]
)

cc = meson.get_compiler('c')

# Core dependencies
glib_dep = dependency('glib-2.0', version: '>= 2.74')
gio_dep = dependency('gio-2.0', version: '>= 2.74')
gio_unix_dep = dependency('gio-unix-2.0', version: '>= 2.74')
gobject_dep = dependency('gobject-2.0', version: '>= 2.74')
threads_dep = dependency('threads')
m_dep = cc.find_library('m', required: true)
dl_dep = cc.find_library('dl', required: false)
libnotify_dep = dependency('libnotify', required: false)

# Subproject include directory
inc = include_directories('include')

# Client dependency (Nautilus Extension)
nautilus_dep = dependency('libnautilus-extension-4', required: get_option('client'))

# UI dependencies (GTK4 + Libadwaita)
adw_dep = dependency('libadwaita-1', version: '>= 1.4', required: get_option('ui'))
gtk4_dep = dependency('gtk4', version: '>= 4.10', required: get_option('ui'))

# Engine dependencies
vips_dep = dependency('vips', version: '>= 8.12', required: get_option('engine_vips'))
poppler_dep = dependency('poppler-glib', version: '>= 22.0', required: get_option('engine_poppler'))
avcodec_dep = dependency('libavcodec', required: get_option('engine_ffmpeg'))
avformat_dep = dependency('libavformat', required: get_option('engine_ffmpeg'))
avutil_dep = dependency('libavutil', required: get_option('engine_ffmpeg'))
swscale_dep = dependency('libswscale', required: get_option('engine_ffmpeg'))
archive_dep = dependency('libarchive', version: '>= 3.6', required: get_option('engine_office'))

zlib_dep = dependency('zlib', version: '>= 1.2.9', required: get_option('engine_safe_images'))
jpeg_dep = dependency('libjpeg', required: get_option('engine_safe_images'))

# Configuration header
conf_data = configuration_data()
conf_data.set('HAVE_IMAGE_PRESERVE', zlib_dep.found() and jpeg_dep.found())
conf_data.set('HAVE_LIBVIPS', vips_dep.found())
conf_data.set('HAVE_POPPLER', poppler_dep.found())
conf_data.set('HAVE_FFMPEG', avcodec_dep.found() and avformat_dep.found())
conf_data.set('HAVE_LIBNOTIFY', libnotify_dep.found())
conf_data.set('HAVE_LIBARCHIVE', archive_dep.found())
conf_data.set('HAVE_ENGINE_AI', get_option('engine_ai').allowed())
conf_data.set_quoted('TC_HELPER_DIR', get_option('prefix') / get_option('libexecdir') / 'teraconvert')
conf_data.set_quoted('TC_VERSION', meson.project_version())
configure_file(output: 'config.h', configuration: conf_data)
config_inc = include_directories('.')

# Subdirectories
if get_option('client') and nautilus_dep.found()
  subdir('src/client')
endif

if get_option('daemon')
  subdir('src/engines')
  subdir('src/daemon')
endif

if get_option('ui').allowed() and adw_dep.found() and gtk4_dep.found()
  subdir('src/ui')
endif

subdir('src/cli')
subdir('tests')

# Install only the support files used by selected components.
if get_option('daemon')
  systemd_user_dir = get_option('prefix') / 'lib' / 'systemd' / 'user'
  install_data('data/systemd/teraconvert.socket', 'data/systemd/teraconvert.service',
    install_dir: systemd_user_dir)
  if get_option('engine_ai').allowed()
    install_data('src/scripts/teraconvert-rembg', install_dir: get_option('bindir'), install_mode: 'rwxr-xr-x')
    install_data('tools/register-ai-model.py', rename: 'teraconvert-register-model',
      install_dir: get_option('bindir'), install_mode: 'rwxr-xr-x')
  endif
  if poppler_dep.found()
    install_data('src/scripts/teraconvert-pdf-safe', install_dir: get_option('libexecdir') / 'teraconvert', install_mode: 'rwxr-xr-x')
  endif
endif
if get_option('client') or (get_option('ui').allowed() and adw_dep.found() and gtk4_dep.found())
  install_subdir('data/icons/hicolor', install_dir: get_option('datadir') / 'icons')
endif
if get_option('ui').allowed() and adw_dep.found() and gtk4_dep.found()
  install_data('data/applications/teraconvert.desktop', install_dir: get_option('datadir') / 'applications')
  install_data('data/metainfo/org.gnome.teraconvert.metainfo.xml', install_dir: get_option('datadir') / 'metainfo')
endif
