#include "tc_engine.h"
#include "tc_logging.h"
#include "tc_process_runner.h"
#include "tc_runtime_limits.h"

#ifdef HAVE_LIBVIPS
#include <vips/vips.h>

static bool vips_initialized = false;

static void vips_lazy_init(void) {
    if (!vips_initialized) {
        if (VIPS_INIT("teraconvert") != 0) {
            tc_log_error("vips_init() failed: %s", vips_error_buffer());
            return;
        }
        vips_concurrency_set(tc_engine_thread_limit());
        vips_cache_set_max_mem(64ULL*1024*1024);
        vips_initialized = true;
        tc_log_info("libvips initialized with concurrency %u", tc_engine_thread_limit());
    }
}

static TcStatusCode vips_estimate(const char *source,const char *format,uint16_t w,uint16_t h,GCancellable *cancel,TcEngineEstimate *estimate,char **error){
    if(cancel&&g_cancellable_is_cancelled(cancel))return TC_STATUS_ERROR_CANCELLED;
    vips_lazy_init();
    VipsImage *image=vips_image_new_from_file(source,"access",VIPS_ACCESS_SEQUENTIAL,NULL);
    if(!image){if(error)*error=g_strdup(vips_error_buffer());vips_error_clear();return TC_STATUS_ERROR_CORRUPT;}
    int width=vips_image_get_width(image),height=vips_image_get_height(image),bands=vips_image_get_bands(image);
    int bytes=vips_format_sizeof(vips_image_get_format(image));
    if(width<=0||height<=0||width>TC_MAX_IMAGE_DIMENSION||height>TC_MAX_IMAGE_DIMENSION||bands<=0||bands>16||bytes<=0){g_object_unref(image);if(error)*error=g_strdup("Image geometry exceeds configured bounds");return TC_STATUS_ERROR_UNSUPPORTED;}
    uint64_t input=(uint64_t)width*height*bands*bytes;
    uint64_t output=(uint64_t)(w?w:width)*(h?h:height)*MAX(bands,4)*MAX(bytes,2);
    estimate->memory_bytes=MAX(input,output)*3+64ULL*1024*1024;
    estimate->output_bytes=output+16ULL*1024*1024;
    if(!strcmp(format,"pdf"))estimate->output_bytes=MAX(input,output)+32ULL*1024*1024;
    g_object_unref(image);return TC_STATUS_OK;
}

static bool vips_supports_format(const char *format) {
    if (!format) return false;
    return (g_ascii_strcasecmp(format, "webp") == 0 ||
            g_ascii_strcasecmp(format, "png") == 0 ||
            g_ascii_strcasecmp(format, "jpg") == 0 ||
            g_ascii_strcasecmp(format, "jpeg") == 0 ||
            g_ascii_strcasecmp(format, "avif") == 0 ||
            g_ascii_strcasecmp(format, "bmp") == 0 ||
            g_ascii_strcasecmp(format, "tiff") == 0 ||
            g_ascii_strcasecmp(format, "tif") == 0 ||
            g_ascii_strcasecmp(format, "ico") == 0 ||
            g_ascii_strcasecmp(format, "gif") == 0 ||
            g_ascii_strcasecmp(format, "pdf") == 0 ||
            g_ascii_strcasecmp(format, "resize") == 0);
}

static TcStatusCode vips_convert(
    const char *src_path,
    const char *dst_path,
    const char *target_format,
    uint16_t target_width,
    uint16_t target_height,
    GCancellable *cancellable,
    char **error_out,
    TcProgressCallback progress_cb,
    void *user_data
) {
    (void)cancellable;
    (void)progress_cb;
    (void)user_data;

    /* Handle PDF format via img2pdf (lossless direct embedding) with PIL fallback */
    if (g_ascii_strcasecmp(target_format, "pdf") == 0) {
        GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
        g_ptr_array_add(argv, g_strdup("img2pdf"));
        g_ptr_array_add(argv, g_strdup("--output"));
        g_ptr_array_add(argv, g_strdup(dst_path));
        g_ptr_array_add(argv, g_strdup(src_path));
        g_ptr_array_add(argv, NULL);

        int exit_status = 0;
        GError *err = NULL;
        gboolean spawned = tc_run_helper((const char *const *)argv->pdata, cancellable, &err);
        g_ptr_array_free(argv, TRUE);

        if (!spawned || exit_status != 0) {
            /* Fallback to Python Pillow */
            const char *py_pdf_fallback =
                "import sys\n"
                "from PIL import Image\n"
                "src, dst = sys.argv[1], sys.argv[2]\n"
                "im = Image.open(src)\n"
                "if im.mode not in ('RGB', 'L'): im = im.convert('RGB')\n"
                "im.save(dst, format='PDF')\n";
            GPtrArray *py_argv = g_ptr_array_new_with_free_func(g_free);
            g_ptr_array_add(py_argv, g_strdup("python3"));
            g_ptr_array_add(py_argv, g_strdup("-c"));
            g_ptr_array_add(py_argv, g_strdup(py_pdf_fallback));
            g_ptr_array_add(py_argv, g_strdup(src_path));
            g_ptr_array_add(py_argv, g_strdup(dst_path));
            g_ptr_array_add(py_argv, NULL);

            int py_exit = 0;
            GError *py_err = NULL;
            gboolean py_spawned = tc_run_helper((const char *const *)py_argv->pdata, cancellable, &py_err);
            g_ptr_array_free(py_argv, TRUE);
            if (!py_spawned || py_exit != 0) {
                if (error_out) *error_out = g_strdup(py_err ? py_err->message : (err ? err->message : "PDF conversion failed"));
                g_clear_error(&py_err);
                g_clear_error(&err);
                return TC_STATUS_ERROR;
            }
            g_clear_error(&err);
        }
        return TC_STATUS_OK;
    }

    /* Handle ICO format via Python Pillow (standard multi-resolution icon resource) */
    if (g_ascii_strcasecmp(target_format, "ico") == 0) {
        const char *py_ico_script =
            "import sys\n"
            "from PIL import Image\n"
            "src, dst = sys.argv[1], sys.argv[2]\n"
            "tw = int(sys.argv[3]) if len(sys.argv) > 3 else 0\n"
            "th = int(sys.argv[4]) if len(sys.argv) > 4 else 0\n"
            "im = Image.open(src)\n"
            "if im.mode not in ('RGB', 'RGBA'): im = im.convert('RGBA')\n"
            "if tw > 0 and th > 0:\n"
            "    tw_c, th_c = min(tw, 256), min(th, 256)\n"
            "    im = im.resize((tw_c, th_c), Image.Resampling.LANCZOS)\n"
            "    sizes = [(tw_c, th_c)]\n"
            "else:\n"
            "    sizes = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]\n"
            "im.save(dst, format='ICO', sizes=sizes)\n";

        g_autofree char *tw_str = g_strdup_printf("%u", (unsigned int)target_width);
        g_autofree char *th_str = g_strdup_printf("%u", (unsigned int)target_height);

        GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
        g_ptr_array_add(argv, g_strdup("python3"));
        g_ptr_array_add(argv, g_strdup("-c"));
        g_ptr_array_add(argv, g_strdup(py_ico_script));
        g_ptr_array_add(argv, g_strdup(src_path));
        g_ptr_array_add(argv, g_strdup(dst_path));
        g_ptr_array_add(argv, g_strdup(tw_str));
        g_ptr_array_add(argv, g_strdup(th_str));
        g_ptr_array_add(argv, NULL);

        int exit_status = 0;
        GError *err = NULL;
        gboolean spawned = tc_run_helper((const char *const *)argv->pdata, cancellable, &err);
        g_ptr_array_free(argv, TRUE);

        if (!spawned || exit_status != 0) {
            if (error_out) *error_out = g_strdup(err ? err->message : "ICO conversion failed");
            g_clear_error(&err);
            return TC_STATUS_ERROR;
        }
        return TC_STATUS_OK;
    }

    vips_lazy_init();

    VipsImage *in = vips_image_new_from_file(src_path, "access", VIPS_ACCESS_SEQUENTIAL, NULL);
    if (!in) {
        if (error_out) *error_out = g_strdup(vips_error_buffer());
        vips_error_clear();
        return TC_STATUS_ERROR_CORRUPT;
    }

    /* DEC-005: Dimension clamp check (16,384 x 16,384 px max) */
    int width = vips_image_get_width(in);
    int height = vips_image_get_height(in);
    if (width > TC_MAX_IMAGE_DIMENSION || height > TC_MAX_IMAGE_DIMENSION) {
        if (error_out) {
            *error_out = g_strdup_printf(
                "Image dimensions (%dx%d) exceed maximum safety limit (%dx%d)",
                width, height, TC_MAX_IMAGE_DIMENSION, TC_MAX_IMAGE_DIMENSION
            );
        }
        g_object_unref(in);
        return TC_STATUS_ERROR_MEM_LIMIT;
    }

    /* If resizing requested, scale image */
    if (target_width > 0 && target_height > 0 && (target_width != width || target_height != height)) {
        VipsImage *resized = NULL;
        double hscale = (double)target_width / (double)width;
        double vscale = (double)target_height / (double)height;
        if (vips_resize(in, &resized, hscale, "vscale", vscale, NULL) != 0) {
            if (error_out) *error_out = g_strdup(vips_error_buffer());
            vips_error_clear();
            g_object_unref(in);
            return TC_STATUS_ERROR;
        }
        g_object_unref(in);
        in = resized;
    }

    int ret = -1;
    if (g_ascii_strcasecmp(target_format, "webp") == 0) {
        ret = vips_webpsave(in, dst_path, "Q", 85, NULL);
    } else if (g_ascii_strcasecmp(target_format, "png") == 0) {
        ret = vips_pngsave(in, dst_path, "compression", 6, NULL);
    } else if (g_ascii_strcasecmp(target_format, "jpg") == 0 ||
               g_ascii_strcasecmp(target_format, "jpeg") == 0) {
        ret = vips_jpegsave(in, dst_path, "Q", 90, NULL);
    } else if (g_ascii_strcasecmp(target_format, "avif") == 0) {
        ret = vips_heifsave(in, dst_path, "compression", VIPS_FOREIGN_HEIF_COMPRESSION_AV1, "Q", 80, NULL);
    } else if (g_ascii_strcasecmp(target_format, "tiff") == 0 || g_ascii_strcasecmp(target_format, "tif") == 0) {
        ret = vips_tiffsave(in, dst_path, "compression", VIPS_FOREIGN_TIFF_COMPRESSION_DEFLATE, NULL);
    } else {
        ret = vips_image_write_to_file(in, dst_path, NULL);
    }

    g_object_unref(in);

    if (ret != 0) {
        if (error_out) *error_out = g_strdup(vips_error_buffer());
        vips_error_clear();
        return TC_STATUS_ERROR;
    }

    return TC_STATUS_OK;
}

static void vips_cleanup_engine(void) {
    if (vips_initialized) {
        vips_shutdown();
        vips_initialized = false;
    }
}

TcEngine tc_engine_vips = {
    .estimate = vips_estimate,
    .name = "libvips",
    .category = TC_CATEGORY_IMAGE,
    .supports_format = vips_supports_format,
    .convert = vips_convert,
    .init = vips_lazy_init,
    .cleanup = vips_cleanup_engine
};
#endif
