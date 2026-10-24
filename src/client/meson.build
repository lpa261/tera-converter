nautilus_extdir = nautilus_dep.get_variable('extensiondir')

gnome = import('gnome')

menu_resources_en = gnome.compile_resources(
  'teraconvert-resources-en',
  meson.project_source_root() / 'data/teraconvert-en.gresource.xml',
  source_dir: meson.project_source_root() / 'data/menu',
  c_name: 'teraconvert_en'
)

menu_resources_vi = gnome.compile_resources(
  'teraconvert-resources-vi',
  meson.project_source_root() / 'data/teraconvert-vi.gresource.xml',
  source_dir: meson.project_source_root() / 'data/menu',
  c_name: 'teraconvert_vi'
)

client_sources = files(
  'tc_client.c',
  'tc_nautilus_ext.c'
)

shared_module(
  'nautilus-teraconvert',
  client_sources,
  menu_resources_en,
  menu_resources_vi,
  include_directories: [inc, config_inc],
  dependencies: [
    glib_dep,
    gio_dep,
    gio_unix_dep,
    gobject_dep,
    nautilus_dep,
    libnotify_dep
  ],
  install: true,
  install_dir: nautilus_extdir
)
