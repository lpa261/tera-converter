#include "tc_engine.h"
#include "tc_logging.h"
#include "tc_process_runner.h"

#ifdef HAVE_POPPLER
#include <poppler.h>

static TcStatusCode pdf_estimate(const char *source,const char *format,uint16_t w,uint16_t h,GCancellable *cancel,TcEngineEstimate *estimate,char **error){
    (void)w;(void)h;
    if(cancel&&g_cancellable_is_cancelled(cancel))return TC_STATUS_ERROR_CANCELLED;
    g_autofree char *uri=g_filename_to_uri(source,NULL,NULL);GError *problem=NULL;
    PopplerDocument *document=uri?poppler_document_new_from_file(uri,NULL,&problem):NULL;
    if(!document){if(error)*error=g_strdup(problem?problem->message:"Cannot inspect PDF geometry");g_clear_error(&problem);return TC_STATUS_ERROR_CORRUPT;}
    int pages=poppler_document_get_n_pages(document);double max_pixels=0;
    if(pages<=0||pages>4096){g_object_unref(document);return TC_STATUS_ERROR_UNSUPPORTED;}
    for(int i=0;i<pages;i++){if(cancel&&g_cancellable_is_cancelled(cancel)){g_object_unref(document);return TC_STATUS_ERROR_CANCELLED;}PopplerPage *page=poppler_document_get_page(document,i);if(!page){g_object_unref(document);return TC_STATUS_ERROR_CORRUPT;}double x,y;poppler_page_get_size(page,&x,&y);g_object_unref(page);
        if(!(x>0&&y>0&&x<=20000&&y<=20000)){g_object_unref(document);return TC_STATUS_ERROR_UNSUPPORTED;}
        double pixels=(x*150.0/72.0)*(y*150.0/72.0);if(pixels>max_pixels)max_pixels=pixels;
    }
    g_object_unref(document);estimate->memory_bytes=(uint64_t)(max_pixels*4)*3+96ULL*1024*1024;
    if(!strcmp(format,"png")||!strcmp(format,"jpg")||!strcmp(format,"jpeg"))estimate->output_bytes=(uint64_t)(max_pixels*4)+16ULL*1024*1024;
    return TC_STATUS_OK;
}

static bool poppler_supports_format(const char *format) {
    if (!format) return false;
    return (g_ascii_strcasecmp(format, "txt") == 0 ||
            g_ascii_strcasecmp(format, "html") == 0 ||
            g_ascii_strcasecmp(format, "png") == 0 ||
            g_ascii_strcasecmp(format, "jpg") == 0 ||
            g_ascii_strcasecmp(format, "jpeg") == 0 ||
            g_ascii_strcasecmp(format, "extract-images") == 0 ||
            g_ascii_strcasecmp(format, "pdf") == 0 ||
            g_ascii_strcasecmp(format, "comp-pdf-web") == 0 ||
            g_ascii_strcasecmp(format, "comp-pdf-print") == 0 ||
            g_ascii_strcasecmp(format, "comp-pdf") == 0);
}

static TcStatusCode poppler_convert(
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
    (void)target_width;
    (void)target_height;
    (void)cancellable;
    (void)progress_cb;
    (void)user_data;

    /* Handle PDF Compression via Ghostscript */
    if (g_ascii_strcasecmp(target_format, "comp-pdf-web") == 0 ||
        g_ascii_strcasecmp(target_format, "comp-pdf-print") == 0 ||
        g_ascii_strcasecmp(target_format, "comp-pdf") == 0) {
        bool is_web = (g_ascii_strcasecmp(target_format, "comp-pdf-print") != 0);
        GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
        g_ptr_array_add(argv, g_strdup("gs"));
        g_ptr_array_add(argv, g_strdup("-sDEVICE=pdfwrite"));
        g_ptr_array_add(argv, g_strdup("-dCompatibilityLevel=1.4"));
        g_ptr_array_add(argv, g_strdup(is_web ? "-dPDFSETTINGS=/screen" : "-dPDFSETTINGS=/ebook"));
        g_ptr_array_add(argv, g_strdup("-dNOPAUSE"));
        g_ptr_array_add(argv, g_strdup("-dQUIET"));
        g_ptr_array_add(argv, g_strdup("-dBATCH"));
        g_ptr_array_add(argv, g_strdup_printf("-sOutputFile=%s", dst_path));
        g_ptr_array_add(argv, g_strdup(src_path));
        g_ptr_array_add(argv, NULL);

        int exit_status = 0;
        GError *err = NULL;
        gboolean spawned = tc_run_helper((const char *const *)argv->pdata, cancellable, &err);
        g_ptr_array_free(argv, TRUE);

        if (!spawned || exit_status != 0) {
            if (error_out) *error_out = g_strdup(err ? err->message : "Ghostscript PDF compression failed");
            g_clear_error(&err);
            return TC_STATUS_ERROR;
        }
        return TC_STATUS_OK;
    }

    if (g_ascii_strcasecmp(target_format, "txt") == 0) {
        GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
        g_ptr_array_add(argv, g_strdup("pdftotext"));
        g_ptr_array_add(argv, g_strdup("-layout"));
        g_ptr_array_add(argv, g_strdup(src_path));
        g_ptr_array_add(argv, g_strdup(dst_path));
        g_ptr_array_add(argv, NULL);

        int exit_status = 0;
        GError *err = NULL;
        gboolean spawned = tc_run_helper((const char *const *)argv->pdata, cancellable, &err);
        g_ptr_array_free(argv, TRUE);
        if (!spawned || exit_status != 0) {
            if (error_out) *error_out = g_strdup(err ? err->message : "pdftotext conversion failed");
            g_clear_error(&err);
            return TC_STATUS_ERROR;
        }
        return TC_STATUS_OK;
    }

    if (g_ascii_strcasecmp(target_format, "html") == 0) {
        g_autofree char *tmp_prefix = g_strdup_printf("%s.tmp_html", dst_path);
        GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
        g_ptr_array_add(argv, g_strdup("pdftohtml"));
        g_ptr_array_add(argv, g_strdup("-s"));
        g_ptr_array_add(argv, g_strdup("-c"));
        g_ptr_array_add(argv, g_strdup("-noframes"));
        g_ptr_array_add(argv, g_strdup(src_path));
        g_ptr_array_add(argv, g_strdup(tmp_prefix));
        g_ptr_array_add(argv, NULL);

        int exit_status = 0;
        GError *err = NULL;
        gboolean spawned = tc_run_helper((const char *const *)argv->pdata, cancellable, &err);
        g_ptr_array_free(argv, TRUE);

        g_autofree char *actual_html = g_strdup_printf("%s.html", tmp_prefix);
        if (spawned && exit_status == 0 && g_file_test(actual_html, G_FILE_TEST_EXISTS)) {
            rename(actual_html, dst_path);
            return TC_STATUS_OK;
        }
        unlink(actual_html);
        if (error_out) *error_out = g_strdup(err ? err->message : "pdftohtml conversion failed");
        g_clear_error(&err);
        return TC_STATUS_ERROR;
    }

    if (g_ascii_strcasecmp(target_format, "png") == 0 ||
        g_ascii_strcasecmp(target_format, "jpg") == 0 ||
        g_ascii_strcasecmp(target_format, "jpeg") == 0) {
        bool is_png = (g_ascii_strcasecmp(target_format, "png") == 0);
        g_autofree char *tmp_prefix = g_strdup_printf("%s.tmp", dst_path);
        
        GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
        g_ptr_array_add(argv, g_strdup("pdftoppm"));
        g_ptr_array_add(argv, g_strdup(is_png ? "-png" : "-jpeg"));
        g_ptr_array_add(argv, g_strdup("-r"));
        g_ptr_array_add(argv, g_strdup("150"));
        g_ptr_array_add(argv, g_strdup("-singlefile"));
        g_ptr_array_add(argv, g_strdup(src_path));
        g_ptr_array_add(argv, g_strdup(tmp_prefix));
        g_ptr_array_add(argv, NULL);

        int exit_status = 0;
        GError *err = NULL;
        gboolean spawned = tc_run_helper((const char *const *)argv->pdata, cancellable, &err);
        g_ptr_array_free(argv, TRUE);

        g_autofree char *actual_out = g_strdup_printf("%s.%s", tmp_prefix, is_png ? "png" : "jpg");
        if (spawned && exit_status == 0 && g_file_test(actual_out, G_FILE_TEST_EXISTS)) {
            if (rename(actual_out, dst_path) == 0) return TC_STATUS_OK;
            if (error_out) *error_out = g_strdup_printf("Cannot move PDF conversion output: %s", g_strerror(errno));
            unlink(actual_out);
            return TC_STATUS_ERROR;
        }
        unlink(actual_out);
        if (error_out) *error_out = g_strdup(err ? err->message : "pdftoppm conversion failed");
        g_clear_error(&err);
        return TC_STATUS_ERROR;
    }

    g_autofree char *uri = g_filename_to_uri(src_path, NULL, NULL);
    if (!uri) return TC_STATUS_ERROR;

    GError *err = NULL;
    PopplerDocument *doc = poppler_document_new_from_file(uri, NULL, &err);
    if (!doc) {
        if (error_out && err) {
            *error_out = g_strdup(err->message);
        }
        g_clear_error(&err);
        return TC_STATUS_ERROR_CORRUPT;
    }

    int n_pages = poppler_document_get_n_pages(doc);
    tc_log_debug("Poppler inspected %s: %d pages", src_path, n_pages);
    g_object_unref(doc);

    return TC_STATUS_OK;
}

TcEngine tc_engine_poppler = {
    .estimate = pdf_estimate,
    .name = "poppler_doc",
    .category = TC_CATEGORY_DOCUMENT,
    .supports_format = poppler_supports_format,
    .convert = poppler_convert,
    .init = NULL,
    .cleanup = NULL
};

#endif
