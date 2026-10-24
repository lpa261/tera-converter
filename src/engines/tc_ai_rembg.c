#define _GNU_SOURCE
#include "tc_engine.h"
#include "tc_logging.h"
#include "tc_process_runner.h"
#include <sys/wait.h>

static bool rembg_supports_format(const char *format) {
    if (!format) return false;
    return (g_ascii_strcasecmp(format, "rmbg") == 0 ||
            g_ascii_strcasecmp(format, "remove-bg") == 0);
}

static TcStatusCode rembg_convert(
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
    (void)target_format;
    (void)target_width;
    (void)target_height;
    (void)cancellable;
    (void)progress_cb;
    (void)user_data;

    tc_log_info("Executing AI Background Removal: %s -> %s", src_path, dst_path);

    /* Locate teraconvert-rembg binary */
    g_autofree char *rembg_bin=g_file_test("/usr/bin/teraconvert-rembg",G_FILE_TEST_IS_EXECUTABLE)?
        g_strdup("/usr/bin/teraconvert-rembg"):g_find_program_in_path("teraconvert-rembg");
    if(!rembg_bin){if(error_out)*error_out=g_strdup("AI helper is not installed");return TC_STATUS_MISSING_AI_RUNTIME;}

    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(argv, g_strdup(rembg_bin));
    g_ptr_array_add(argv, g_strdup("-o"));
    g_ptr_array_add(argv, g_strdup(dst_path));
    g_ptr_array_add(argv, g_strdup(src_path));
    g_ptr_array_add(argv, NULL);

    int exit_status = 0;
    GError *err = NULL;
    gboolean spawned = tc_run_helper((const char *const *)argv->pdata, cancellable, &err);
    g_ptr_array_free(argv, TRUE);

    if (!spawned || exit_status != 0) {
        if (error_out) {
            *error_out = g_strdup(err ? err->message : "teraconvert-rembg failed to process image");
        }
        g_clear_error(&err);
        return TC_STATUS_ERROR;
    }

    if (!g_file_test(dst_path, G_FILE_TEST_EXISTS)) {
        if (error_out) {
            *error_out = g_strdup("AI output file was not created");
        }
        return TC_STATUS_ERROR;
    }

    tc_log_info("AI RMBG successfully generated output at %s", dst_path);
    return TC_STATUS_OK;
}

static void rembg_cleanup(void) {
}

TcEngine tc_engine_rembg = {
    .name = "rembg_onnx",
    .category = TC_CATEGORY_IMAGE,
    .supports_format = rembg_supports_format,
    .convert = rembg_convert,
    .init = NULL,
    .cleanup = rembg_cleanup
};
