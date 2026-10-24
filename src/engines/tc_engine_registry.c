#include "tc_engine.h"
#include "tc_logging.h"

extern TcEngine tc_engine_vips;
extern TcEngine tc_engine_ffmpeg;
extern TcEngine tc_engine_poppler;
extern TcEngine tc_engine_rembg;

static GPtrArray *registered_engines = NULL;

void tc_engines_init_registry(void) {
    if (registered_engines) return;

    registered_engines = g_ptr_array_new();

#ifdef HAVE_LIBARCHIVE
    extern TcEngine tc_engine_office;
    g_ptr_array_add(registered_engines, &tc_engine_office);
#endif

#ifdef HAVE_IMAGE_PRESERVE
    extern TcEngine tc_engine_image_preserve;
    g_ptr_array_add(registered_engines,&tc_engine_image_preserve);
#endif

#ifdef HAVE_LIBVIPS
    g_ptr_array_add(registered_engines, &tc_engine_vips);
#endif

#ifdef HAVE_FFMPEG
    extern TcEngine tc_engine_media_remux;
    g_ptr_array_add(registered_engines,&tc_engine_media_remux);
    extern TcEngine tc_engine_audio_preserve;
    g_ptr_array_add(registered_engines,&tc_engine_audio_preserve);
    g_ptr_array_add(registered_engines, &tc_engine_ffmpeg);
#endif

#ifdef HAVE_POPPLER
    extern TcEngine tc_engine_pdf_preserve;
    g_ptr_array_add(registered_engines,&tc_engine_pdf_preserve);
    g_ptr_array_add(registered_engines, &tc_engine_poppler);
#endif

#ifdef HAVE_ENGINE_AI
    g_ptr_array_add(registered_engines, &tc_engine_rembg);
#endif

    for (guint i = 0; i < registered_engines->len; i++) {
        TcEngine *eng = (TcEngine *)g_ptr_array_index(registered_engines, i);
        if (eng->init) {
            eng->init();
        }
    }

    tc_log_info("Initialized engine registry with %u backend engines", registered_engines->len);
}

TcEngine *tc_engines_find_for_task(TcCategory category, const char *target_format) {
    if (!registered_engines || !target_format) return NULL;

    for (guint i = 0; i < registered_engines->len; i++) {
        TcEngine *eng = (TcEngine *)g_ptr_array_index(registered_engines, i);
        if (category != TC_CATEGORY_UNKNOWN && eng->category != category) {
            continue;
        }
        if (eng->supports_format && eng->supports_format(target_format)) {
            return eng;
        }
    }
    return NULL;
}

void tc_engines_cleanup_all(void) {
    if (!registered_engines) return;

    for (guint i = 0; i < registered_engines->len; i++) {
        TcEngine *eng = (TcEngine *)g_ptr_array_index(registered_engines, i);
        if (eng->cleanup) {
            eng->cleanup();
        }
    }
    g_ptr_array_free(registered_engines, TRUE);
    registered_engines = NULL;
}
