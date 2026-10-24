pixbuf_dep = dependency('gdk-pixbuf-2.0', version: '>=2.42')
preview_deps = [glib_dep, gio_dep, gtk4_dep, adw_dep, pixbuf_dep, m_dep]
if poppler_dep.found()
  preview_deps += poppler_dep
endif
preview_bin = executable('teraconvert-preview', ['tc_preview_app.c', 'tc_preview_load.c'],
  include_directories: [inc, config_inc], dependencies: preview_deps, install: true)
configure_file(input: '../../data/dbus/org.gnome.TeraConvert.Preview.service.in',
  output: 'org.gnome.TeraConvert.Preview.service',
  configuration: {'PREVIEW_EXEC': get_option('prefix') / get_option('bindir') / 'teraconvert-preview'},
  install_dir: get_option('datadir') / 'dbus-1/services')
preview_test = executable('test_preview', ['../../tests/test_preview.c', 'tc_preview_load.c'],
  include_directories: [inc, config_inc, include_directories('.')], dependencies: preview_deps)
test('Quick Look Loader Regression', preview_test, suite: 'preview')

install_data('../../tools/enable-quicklook.sh', rename: 'teraconvert-quicklook-setup', install_dir: get_option('bindir'), install_mode: 'rwxr-xr-x')
