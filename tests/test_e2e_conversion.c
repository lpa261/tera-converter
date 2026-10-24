#define _GNU_SOURCE
#include "tc_common.h"
#include "tc_engine.h"
#include "tc_logging.h"
#include <assert.h>
#include <sys/stat.h>
#include <utime.h>

/* Every run owns its fixtures and outputs; no shared /tmp names or user data. */
static char *fixture_dir;
static GPtrArray *allocated_paths;
static const char *g_fixture_png, *g_fixture_mp4, *g_fixture_pdf, *g_fixture_docx;

static const char *test_path(const char *name) {
    char *path = g_build_filename(fixture_dir, name, NULL);
    g_ptr_array_add(allocated_paths, path);
    return path;
}

static void generate(const char * const *argv) {
    GError *error = NULL;
    GSubprocess *child = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_NONE, &error);
    g_assert_no_error(error);
    g_assert_nonnull(child);
    g_assert_true(g_subprocess_wait_check(child, NULL, &error));
    g_assert_no_error(error);
    g_object_unref(child);
}

static void ensure_test_fixtures(void) {
    fixture_dir = g_dir_make_tmp("tc-engine-tests-XXXXXX", NULL);
    g_assert_nonnull(fixture_dir);
    allocated_paths = g_ptr_array_new_with_free_func(g_free);
    g_fixture_png = test_path("sample.png");
    g_fixture_mp4 = test_path("sample.mp4");
    g_fixture_pdf = test_path("sample.pdf");
    g_fixture_docx = test_path("sample.docx");
    generate((const char *[]) {"/usr/bin/ffmpeg", "-v", "error", "-nostdin", "-y", "-f", "lavfi", "-i",
        "testsrc=duration=0.1:size=320x240:rate=1", "-frames:v", "1", g_fixture_png, NULL});
    generate((const char *[]) {"/usr/bin/ffmpeg", "-v", "error", "-nostdin", "-y", "-f", "lavfi", "-i",
        "testsrc=duration=2:size=640x360:rate=24", "-f", "lavfi", "-i", "sine=duration=2:frequency=1000",
        "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac", g_fixture_mp4, NULL});
    g_autofree char *pdf_arg = g_strdup_printf("-sOutputFile=%s", g_fixture_pdf);
    generate((const char *[]) {"gs", "-q", "-dBATCH", "-dNOPAUSE", "-sDEVICE=pdfwrite", pdf_arg, "-c",
        "/Helvetica findfont 20 scalefont setfont 50 700 moveto (TeraConvert Test Document) show showpage", NULL});
    generate((const char *[]) {"python3", "-c",
        "import zipfile,sys\n"
        "with zipfile.ZipFile(sys.argv[1],'w',compression=zipfile.ZIP_STORED) as z:\n"
        " z.writestr('[Content_Types].xml','<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\"><Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/></Types>')\n"
        " z.writestr('word/document.xml','<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:body>'+('<w:p><w:r><w:t>TeraConvert Office Test</w:t></w:r></w:p>'*4096)+'</w:body></w:document>')\n",
        g_fixture_docx, NULL});
}

static void cleanup_fixtures(void) {
    GDir *dir = g_dir_open(fixture_dir, 0, NULL);
    const char *name;
    while (dir && (name = g_dir_read_name(dir))) {
        g_autofree char *path = g_build_filename(fixture_dir, name, NULL);
        unlink(path);
    }
    if (dir) g_dir_close(dir);
    rmdir(fixture_dir);
    g_ptr_array_free(allocated_paths, TRUE);
    g_free(fixture_dir);
}

/* ========================================================================= */
/* SECTION 1: Image Conversions & Manipulations (tc_img_vips)                */
/* ========================================================================= */

static void test_vips_conversions(void) {
    tc_log_info("--- [1/6] Testing Image Engine (tc_img_vips) ---");
    TcEngine *vips_eng = tc_engines_find_for_task(TC_CATEGORY_IMAGE, "webp");
    g_assert_nonnull(vips_eng);

    struct timespec t0, t1;
    char *err = NULL;

    /* 1. PNG -> WebP */
    const char *out_webp = test_path("tc_test_out.webp");
    unlink(out_webp);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    TcStatusCode st = vips_eng->convert(g_fixture_png, out_webp, "webp", 0, 0, NULL, &err, NULL, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1000000.0;
    g_assert_cmpint(st, ==, TC_STATUS_OK);
    g_assert_true(g_file_test(out_webp, G_FILE_TEST_EXISTS));
    tc_log_info("  ✓ PNG -> WebP converted in %.2f ms", ms);
    unlink(out_webp);

    /* 2. PNG -> JPG */
    const char *out_jpg = test_path("tc_test_out.jpg");
    unlink(out_jpg);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    st = vips_eng->convert(g_fixture_png, out_jpg, "jpg", 0, 0, NULL, &err, NULL, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1000000.0;
    g_assert_cmpint(st, ==, TC_STATUS_OK);
    g_assert_true(g_file_test(out_jpg, G_FILE_TEST_EXISTS));
    tc_log_info("  ✓ PNG -> JPG converted in %.2f ms", ms);
    unlink(out_jpg);

    /* 3. PNG -> BMP */
    const char *out_bmp = test_path("tc_test_out.bmp");
    unlink(out_bmp);
    st = vips_eng->convert(g_fixture_png, out_bmp, "bmp", 0, 0, NULL, &err, NULL, NULL);
    g_assert_cmpint(st, ==, TC_STATUS_OK);
    g_assert_true(g_file_test(out_bmp, G_FILE_TEST_EXISTS));
    tc_log_info("  ✓ PNG -> BMP converted successfully");
    unlink(out_bmp);

    /* 4. PNG -> ICO (multi-res Windows Icon) */
    const char *out_ico = test_path("tc_test_out.ico");
    unlink(out_ico);
    st = vips_eng->convert(g_fixture_png, out_ico, "ico", 0, 0, NULL, &err, NULL, NULL);
    g_assert_cmpint(st, ==, TC_STATUS_OK);
    g_assert_true(g_file_test(out_ico, G_FILE_TEST_EXISTS));
    /* Verify ICO magic \x00\x00\x01\x00 */
    FILE *f_ico = fopen(out_ico, "rb");
    g_assert_nonnull(f_ico);
    uint8_t ico_hdr[4];
    g_assert_cmpuint(fread(ico_hdr, 1, 4, f_ico), ==, 4);
    fclose(f_ico);
    g_assert_cmpuint(ico_hdr[0], ==, 0);
    g_assert_cmpuint(ico_hdr[1], ==, 0);
    g_assert_cmpuint(ico_hdr[2], ==, 1);
    g_assert_cmpuint(ico_hdr[3], ==, 0);
    tc_log_info("  ✓ PNG -> ICO converted successfully (verified valid ICO header)");
    unlink(out_ico);

    /* 5. PNG -> TIFF */
    const char *out_tiff = test_path("tc_test_out.tiff");
    unlink(out_tiff);
    st = vips_eng->convert(g_fixture_png, out_tiff, "tiff", 0, 0, NULL, &err, NULL, NULL);
    g_assert_cmpint(st, ==, TC_STATUS_OK);
    g_assert_true(g_file_test(out_tiff, G_FILE_TEST_EXISTS));
    tc_log_info("  ✓ PNG -> TIFF converted successfully");
    unlink(out_tiff);

    /* 6. Image Resize (320x240) */
    const char *out_resized = test_path("tc_test_out_resized.png");
    unlink(out_resized);
    st = vips_eng->convert(g_fixture_png, out_resized, "png", 320, 240, NULL, &err, NULL, NULL);
    g_assert_cmpint(st, ==, TC_STATUS_OK);
    g_assert_true(g_file_test(out_resized, G_FILE_TEST_EXISTS));
    tc_log_info("  ✓ Image Resize 320x240 completed successfully");
    unlink(out_resized);

    /* 7. Single Image -> PDF */
    const char *out_img_pdf = test_path("tc_test_out_single.pdf");
    unlink(out_img_pdf);
    st = vips_eng->convert(g_fixture_png, out_img_pdf, "pdf", 0, 0, NULL, &err, NULL, NULL);
    g_assert_cmpint(st, ==, TC_STATUS_OK);
    g_assert_true(g_file_test(out_img_pdf, G_FILE_TEST_EXISTS));
    tc_log_info("  ✓ Image -> PDF (lossless direct embedding) completed successfully");
    unlink(out_img_pdf);

    /* 8. Multi-Image Merge -> PDF */
    const char *out_merged_pdf = test_path("tc_test_out_merged.pdf");
    unlink(out_merged_pdf);
    GPtrArray *argv_merge = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(argv_merge, g_strdup("img2pdf"));
    g_ptr_array_add(argv_merge, g_strdup("--output"));
    g_ptr_array_add(argv_merge, g_strdup(out_merged_pdf));
    g_ptr_array_add(argv_merge, g_strdup(g_fixture_png));
    g_ptr_array_add(argv_merge, g_strdup(g_fixture_png));
    g_ptr_array_add(argv_merge, NULL);

    int exit_status = 0;
    GError *g_err = NULL;
    gboolean spawned = g_spawn_sync(
        NULL, (char **)argv_merge->pdata, NULL,
        G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL,
        &exit_status, &g_err
    );
    g_ptr_array_free(argv_merge, TRUE);
    g_assert_true(spawned);
    g_assert_cmpint(exit_status, ==, 0);
    g_assert_true(g_file_test(out_merged_pdf, G_FILE_TEST_EXISTS));
    tc_log_info("  ✓ Multi-image PDF Merge (2 pages) created successfully");
    unlink(out_merged_pdf);
}

/* ========================================================================= */
/* SECTION 2: AI Background Removal (tc_ai_birefnet)                         */
/* ========================================================================= */

static void test_rmbg_conversion(void) {
    if (g_strcmp0(g_getenv("TC_TEST_AI"), "1") != 0) {
        g_test_skip("Opt-in AI test: set TC_TEST_AI=1 with a preinstalled model/runtime");
        return;
    }
    tc_log_info("--- [2/6] Testing AI Background Removal (tc_ai_birefnet) ---");
    TcEngine *rmbg_eng = tc_engines_find_for_task(TC_CATEGORY_IMAGE, "rmbg");
    g_assert_nonnull(rmbg_eng);

    const char *out_rmbg = test_path("tc_test_out_rmbg.png");
    unlink(out_rmbg);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    char *err = NULL;
    TcStatusCode status = rmbg_eng->convert(g_fixture_png, out_rmbg, "rmbg", 0, 0, NULL, &err, NULL, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1000000.0;
    if (status != TC_STATUS_OK) {
        tc_log_error("RMBG failed: %s", err ? err : "unknown error");
    }
    g_assert_cmpint(status, ==, TC_STATUS_OK);
    g_assert_true(g_file_test(out_rmbg, G_FILE_TEST_EXISTS));
    tc_log_info("  ✓ Background Removal completed in %.2f ms (output: %s)", ms, out_rmbg);
    unlink(out_rmbg);
    g_free(err);
}

/* ========================================================================= */
/* SECTION 3: Video Conversions (tc_media_ffmpeg)                            */
/* ========================================================================= */

static void test_video_conversions(void) {
    tc_log_info("--- [3/6] Testing Video Conversions (tc_media_ffmpeg) ---");
    TcEngine *ff_eng = tc_engines_find_for_task(TC_CATEGORY_MEDIA, "webm");
    g_assert_nonnull(ff_eng);

    char *err = NULL;
    struct timespec t0, t1;

    /* 1. MP4 -> WebM (VP9 Realtime Multithreading) */
    const char *out_webm = test_path("tc_test_out.webm");
    unlink(out_webm);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    TcStatusCode st = ff_eng->convert(g_fixture_mp4, out_webm, "webm", 0, 0, NULL, &err, NULL, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1000000.0;
    g_assert_cmpint(st, ==, TC_STATUS_OK);
    g_assert_true(g_file_test(out_webm, G_FILE_TEST_EXISTS));
    tc_log_info("  ✓ MP4 -> WebM (VP9 realtime -deadline realtime -cpu-used 5 -row-mt 1) finished in %.2f ms", ms);
    unlink(out_webm);

    /* 2. MP4 -> MKV (Smart Remuxing) */
    const char *out_mkv = test_path("tc_test_out.mkv");
    unlink(out_mkv);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    st = ff_eng->convert(g_fixture_mp4, out_mkv, "mkv", 0, 0, NULL, &err, NULL, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1000000.0;
    g_assert_cmpint(st, ==, TC_STATUS_OK);
    g_assert_true(g_file_test(out_mkv, G_FILE_TEST_EXISTS));
    tc_log_info("  ✓ MP4 -> MKV (Smart Remuxing) finished in %.2f ms", ms);
    unlink(out_mkv);

    /* 3. MP4 -> MOV (Native QuickTime Container) */
    const char *out_mov = test_path("tc_test_out.mov");
    unlink(out_mov);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    st = ff_eng->convert(g_fixture_mp4, out_mov, "mov", 0, 0, NULL, &err, NULL, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1000000.0;
    g_assert_cmpint(st, ==, TC_STATUS_OK);
    g_assert_true(g_file_test(out_mov, G_FILE_TEST_EXISTS));
    tc_log_info("  ✓ MP4 -> MOV (Native Container) finished in %.2f ms", ms);
    unlink(out_mov);

    /* 4. MP4 -> AVI */
    const char *out_avi = test_path("tc_test_out.avi");
    unlink(out_avi);
    st = ff_eng->convert(g_fixture_mp4, out_avi, "avi", 0, 0, NULL, &err, NULL, NULL);
    g_assert_cmpint(st, ==, TC_STATUS_OK);
    g_assert_true(g_file_test(out_avi, G_FILE_TEST_EXISTS));
    tc_log_info("  ✓ MP4 -> AVI finished successfully");
    unlink(out_avi);

    /* 5. MP4 -> Animated GIF */
    const char *out_gif = test_path("tc_test_out_anim.gif");
    unlink(out_gif);
    st = ff_eng->convert(g_fixture_mp4, out_gif, "gif", 0, 0, NULL, &err, NULL, NULL);
    g_assert_cmpint(st, ==, TC_STATUS_OK);
    g_assert_true(g_file_test(out_gif, G_FILE_TEST_EXISTS));
    tc_log_info("  ✓ MP4 -> Animated GIF finished successfully");
    unlink(out_gif);
}

/* ========================================================================= */
/* SECTION 4: Audio Extractions & Transcoding (tc_media_ffmpeg)              */
/* ========================================================================= */

static void test_audio_conversions(void) {
    tc_log_info("--- [4/6] Testing Audio Extraction & Transcoding (tc_media_ffmpeg) ---");
    TcEngine *ff_eng = tc_engines_find_for_task(TC_CATEGORY_MEDIA, "mp3");
    g_assert_nonnull(ff_eng);

    char *err = NULL;

    /* 1. Video -> MP3 */
    const char *out_mp3 = test_path("tc_test_out.mp3");
    unlink(out_mp3);
    TcStatusCode st = ff_eng->convert(g_fixture_mp4, out_mp3, "mp3", 0, 0, NULL, &err, NULL, NULL);
    g_assert_cmpint(st, ==, TC_STATUS_OK);
    g_assert_true(g_file_test(out_mp3, G_FILE_TEST_EXISTS));
    tc_log_info("  ✓ Video -> MP3 extracted successfully (output: %s)", out_mp3);
    unlink(out_mp3);

    /* 2. Video -> WAV */
    const char *out_wav = test_path("tc_test_out.wav");
    unlink(out_wav);
    st = ff_eng->convert(g_fixture_mp4, out_wav, "wav", 0, 0, NULL, &err, NULL, NULL);
    g_assert_cmpint(st, ==, TC_STATUS_OK);
    g_assert_true(g_file_test(out_wav, G_FILE_TEST_EXISTS));
    tc_log_info("  ✓ Video -> WAV lossless PCM extracted successfully");
    unlink(out_wav);

    /* 3. Video -> FLAC */
    const char *out_flac = test_path("tc_test_out.flac");
    unlink(out_flac);
    st = ff_eng->convert(g_fixture_mp4, out_flac, "flac", 0, 0, NULL, &err, NULL, NULL);
    g_assert_cmpint(st, ==, TC_STATUS_OK);
    g_assert_true(g_file_test(out_flac, G_FILE_TEST_EXISTS));
    tc_log_info("  ✓ Video -> FLAC lossless audio extracted successfully");
    unlink(out_flac);

    /* 4. Video -> M4A */
    const char *out_m4a = test_path("tc_test_out.m4a");
    unlink(out_m4a);
    st = ff_eng->convert(g_fixture_mp4, out_m4a, "m4a", 0, 0, NULL, &err, NULL, NULL);
    g_assert_cmpint(st, ==, TC_STATUS_OK);
    g_assert_true(g_file_test(out_m4a, G_FILE_TEST_EXISTS));
    tc_log_info("  ✓ Video -> M4A (AAC) extracted successfully");
    unlink(out_m4a);

    /* 5. Video -> OGG */
    const char *out_ogg = test_path("tc_test_out.ogg");
    unlink(out_ogg);
    st = ff_eng->convert(g_fixture_mp4, out_ogg, "ogg", 0, 0, NULL, &err, NULL, NULL);
    g_assert_cmpint(st, ==, TC_STATUS_OK);
    g_assert_true(g_file_test(out_ogg, G_FILE_TEST_EXISTS));
    tc_log_info("  ✓ Video -> OGG (Vorbis) extracted successfully");
    unlink(out_ogg);

    /* 6. Video -> OPUS */
    const char *out_opus = test_path("tc_test_out.opus");
    unlink(out_opus);
    st = ff_eng->convert(g_fixture_mp4, out_opus, "opus", 0, 0, NULL, &err, NULL, NULL);
    g_assert_cmpint(st, ==, TC_STATUS_OK);
    g_assert_true(g_file_test(out_opus, G_FILE_TEST_EXISTS));
    tc_log_info("  ✓ Video -> OPUS modern audio extracted successfully");
    unlink(out_opus);
}

/* ========================================================================= */
/* SECTION 5: Adaptive Video Compression & Telemetry (tc_media_ffmpeg)       */
/* ========================================================================= */

static int g_telemetry_updates = 0;
static float g_last_fraction = 0.0f;
static void test_telemetry_cb(float fraction, uint32_t eta_sec, void *user_data) {
    (void)eta_sec;
    (void)user_data;
    g_telemetry_updates++;
    g_last_fraction = fraction;
}

static void test_video_compression_adaptive(void) {
    tc_log_info("--- [5/6] Testing Adaptive Video Compression & Telemetry ---");
    TcEngine *ff_eng = tc_engines_find_for_task(TC_CATEGORY_MEDIA, "comp-web");
    g_assert_nonnull(ff_eng);

    struct stat st_src;
    g_assert_cmpint(stat(g_fixture_mp4, &st_src), ==, 0);

    /* Profile 1: comp-web (720p H.264) with live telemetry */
    const char *out_comp_web = test_path("tc_test_comp_web.mp4");
    unlink(out_comp_web);
    g_telemetry_updates = 0;
    g_last_fraction = 0.0f;
    char *err = NULL;

    TcStatusCode st = ff_eng->convert(g_fixture_mp4, out_comp_web, "comp-web", 0, 0, NULL, &err, test_telemetry_cb, NULL);
    g_assert_cmpint(st, ==, TC_STATUS_OK);
    g_assert_true(g_file_test(out_comp_web, G_FILE_TEST_EXISTS));
    g_assert_cmpint(g_telemetry_updates, >, 0);
    g_assert_cmpfloat(g_last_fraction, >=, 0.99f);

    struct stat st_web;
    g_assert_cmpint(stat(out_comp_web, &st_web), ==, 0);
    g_assert_cmpint(st_web.st_size, >, 0);
    tc_log_info("  ✓ Profile 'comp-web' completed: %ld -> %ld bytes (telemetry: %d updates, fraction: %.2f)",
                (long)st_src.st_size, (long)st_web.st_size, g_telemetry_updates, g_last_fraction);
    unlink(out_comp_web);

    /* Profile 2: comp-saver (1080p HEVC, extreme space saver) */
    const char *out_comp_saver = test_path("tc_test_comp_saver.mp4");
    unlink(out_comp_saver);
    st = ff_eng->convert(g_fixture_mp4, out_comp_saver, "comp-saver", 0, 0, NULL, &err, NULL, NULL);
    g_assert_cmpint(st, ==, TC_STATUS_OK);
    g_assert_true(g_file_test(out_comp_saver, G_FILE_TEST_EXISTS));
    struct stat st_saver;
    g_assert_cmpint(stat(out_comp_saver, &st_saver), ==, 0);
    g_assert_cmpint(st_saver.st_size, >, 0);
    tc_log_info("  ✓ Profile 'comp-saver' (HEVC) completed: %ld -> %ld bytes",
                (long)st_src.st_size, (long)st_saver.st_size);
    unlink(out_comp_saver);

    /* Profile 3: comp-hq (High Quality codec preservation) */
    const char *out_comp_hq = test_path("tc_test_comp_hq.mp4");
    unlink(out_comp_hq);
    st = ff_eng->convert(g_fixture_mp4, out_comp_hq, "comp-hq", 0, 0, NULL, &err, NULL, NULL);
    g_assert_cmpint(st, ==, TC_STATUS_OK);
    g_assert_true(g_file_test(out_comp_hq, G_FILE_TEST_EXISTS));
    struct stat st_hq;
    g_assert_cmpint(stat(out_comp_hq, &st_hq), ==, 0);
    g_assert_cmpint(st_hq.st_size, >, 0);
    tc_log_info("  ✓ Profile 'comp-hq' completed: %ld -> %ld bytes",
                (long)st_src.st_size, (long)st_hq.st_size);
    unlink(out_comp_hq);
    g_free(err);
}

/* ========================================================================= */
/* SECTION 6: Document Conversions & Compression (tc_doc_poppler)            */
/* ========================================================================= */

static void test_document_conversions(void) {
    tc_log_info("--- [6/6] Testing Document & PDF Engine (tc_doc_poppler) ---");
    TcEngine *doc_eng = tc_engines_find_for_task(TC_CATEGORY_DOCUMENT, "txt");
    g_assert_nonnull(doc_eng);

    char *err = NULL;

    /* 1. PDF -> TXT text extraction */
    const char *out_txt = test_path("tc_test_out.txt");
    unlink(out_txt);
    TcStatusCode st = doc_eng->convert(g_fixture_pdf, out_txt, "txt", 0, 0, NULL, &err, NULL, NULL);
    g_assert_cmpint(st, ==, TC_STATUS_OK);
    g_assert_true(g_file_test(out_txt, G_FILE_TEST_EXISTS));

    char *extracted_content = NULL;
    g_file_get_contents(out_txt, &extracted_content, NULL, NULL);
    g_assert_nonnull(extracted_content);
    g_assert_true(g_str_has_prefix(extracted_content, "TeraConvert"));
    g_free(extracted_content);
    tc_log_info("  ✓ PDF -> TXT extracted successfully with verified text content");
    unlink(out_txt);

    /* 1b. PDF -> HTML conversion */
    const char *out_pdf_html = test_path("tc_test_pdf_page.html");
    unlink(out_pdf_html);
    st = doc_eng->convert(g_fixture_pdf, out_pdf_html, "html", 0, 0, NULL, &err, NULL, NULL);
    g_assert_cmpint(st, ==, TC_STATUS_OK);
    g_assert_true(g_file_test(out_pdf_html, G_FILE_TEST_EXISTS));
    tc_log_info("  ✓ PDF -> HTML webpage generated successfully");
    unlink(out_pdf_html);

    /* 2. PDF -> PNG (page render) */
    const char *out_pdf_png = test_path("tc_test_pdf_page.png");
    unlink(out_pdf_png);
    st = doc_eng->convert(g_fixture_pdf, out_pdf_png, "png", 0, 0, NULL, &err, NULL, NULL);
    g_assert_cmpint(st, ==, TC_STATUS_OK);
    g_assert_true(g_file_test(out_pdf_png, G_FILE_TEST_EXISTS));
    tc_log_info("  ✓ PDF -> PNG page rendered successfully");
    unlink(out_pdf_png);

    /* 3. PDF -> JPG (page render) */
    const char *out_pdf_jpg = test_path("tc_test_pdf_page.jpg");
    unlink(out_pdf_jpg);
    st = doc_eng->convert(g_fixture_pdf, out_pdf_jpg, "jpg", 0, 0, NULL, &err, NULL, NULL);
    g_assert_cmpint(st, ==, TC_STATUS_OK);
    g_assert_true(g_file_test(out_pdf_jpg, G_FILE_TEST_EXISTS));
    tc_log_info("  ✓ PDF -> JPG page rendered successfully");
    unlink(out_pdf_jpg);

    /* 4. PDF Compression: comp-pdf-web (Ghostscript screen) */
    const char *out_comp_pdf = test_path("tc_test_comp_web.pdf");
    unlink(out_comp_pdf);
    st = doc_eng->convert(g_fixture_pdf, out_comp_pdf, "comp-pdf-web", 0, 0, NULL, &err, NULL, NULL);
    g_assert_cmpint(st, ==, TC_STATUS_OK);
    g_assert_true(g_file_test(out_comp_pdf, G_FILE_TEST_EXISTS));
    tc_log_info("  ✓ PDF Compression (comp-pdf-web) completed successfully");
    unlink(out_comp_pdf);

    /* 5. PDF Compression: comp-pdf-print (Ghostscript ebook) */
    const char *out_comp_print = test_path("tc_test_comp_print.pdf");
    unlink(out_comp_print);
    st = doc_eng->convert(g_fixture_pdf, out_comp_print, "comp-pdf-print", 0, 0, NULL, &err, NULL, NULL);
    g_assert_cmpint(st, ==, TC_STATUS_OK);
    g_assert_true(g_file_test(out_comp_print, G_FILE_TEST_EXISTS));
    tc_log_info("  ✓ PDF Compression (comp-pdf-print) completed successfully");
    unlink(out_comp_print);

    /* 6. Office Compression: comp-office (Zip DEFLATE 9 optimization) */
    const char *out_comp_docx = test_path("tc_test_comp_office.docx");
    unlink(out_comp_docx);
    TcEngine *office_eng = tc_engines_find_for_task(TC_CATEGORY_DOCUMENT, "comp-office");
    g_assert_nonnull(office_eng);
    st = office_eng->convert(g_fixture_docx, out_comp_docx, "comp-office", 0, 0, NULL, &err, NULL, NULL);
    g_assert_cmpint(st, ==, TC_STATUS_OK);
    g_assert_true(g_file_test(out_comp_docx, G_FILE_TEST_EXISTS));

    struct stat st_doc_in, st_doc_out;
    g_assert_cmpint(stat(g_fixture_docx, &st_doc_in), ==, 0);
    g_assert_cmpint(stat(out_comp_docx, &st_doc_out), ==, 0);
    g_assert_cmpint(st_doc_out.st_size, <, st_doc_in.st_size);
    tc_log_info("  ✓ Office Document Compression (comp-office) verified: %ld -> %ld bytes (%.1f%% reduction)",
                (long)st_doc_in.st_size, (long)st_doc_out.st_size,
                (1.0 - (double)st_doc_out.st_size / (double)st_doc_in.st_size) * 100.0);
    unlink(out_comp_docx);
    g_free(err);
}

int main(int argc, char *argv[]) {
    g_test_init(&argc, &argv, NULL);
    tc_engines_init_registry();

    ensure_test_fixtures();

    g_test_add_func("/engines/images", test_vips_conversions);
    g_test_add_func("/engines/ai-opt-in", test_rmbg_conversion);
    g_test_add_func("/engines/video", test_video_conversions);
    g_test_add_func("/engines/audio", test_audio_conversions);
    g_test_add_func("/engines/compression", test_video_compression_adaptive);
    g_test_add_func("/engines/documents", test_document_conversions);
    int result = g_test_run();
    cleanup_fixtures();
    tc_engines_cleanup_all();
    return result;
}
