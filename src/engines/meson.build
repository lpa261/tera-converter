engine_sources = files(
  'tc_engine_registry.c',
  'tc_process_runner.c'
)

if get_option('engine_ai').allowed()
  engine_sources += files('tc_ai_rembg.c')
endif

engine_deps = [
  glib_dep,
  gio_dep,
  gobject_dep,
  m_dep,
  dl_dep
]

if zlib_dep.found() and jpeg_dep.found()
  engine_sources += files('tc_image_preserve.c')
  engine_deps += [zlib_dep, jpeg_dep]
endif

if vips_dep.found()
  engine_sources += files('tc_img_vips.c')
  engine_deps += vips_dep
endif

if avcodec_dep.found() and avformat_dep.found()
  engine_sources += files('tc_media_ffmpeg.c', 'tc_media_plan.c', 'tc_media_probe.c', 'tc_media_padding.c', 'tc_audio_preserve.c', 'tc_media_estimate.c', 'tc_media_remux.c')
  engine_deps += [avcodec_dep, avformat_dep, avutil_dep, swscale_dep]
endif

if poppler_dep.found()
  engine_sources += files('tc_doc_poppler.c', 'tc_pdf_preserve.c')
  engine_deps += poppler_dep
endif

if archive_dep.found()
  engine_sources += files('tc_office_zip.c')
  engine_deps += archive_dep
endif

libtc_engines = static_library(
  'tc_engines',
  engine_sources,
  include_directories: [inc, config_inc],
  dependencies: engine_deps,
  install: false
)
