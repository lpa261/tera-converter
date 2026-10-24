#define _GNU_SOURCE
#include "tc_common.h"
#include "tc_ipc.h"
#include "tc_logging.h"
#include "tc_worker_pool.h"
#include "tc_disk_guard.h"
#include "tc_engine.h"
#include "tc_process_runner.h"
#include <assert.h>
#include <utime.h>
#include <sys/stat.h>

static void test_disk_guard(void) {
    tc_log_info("--- [1/3] Testing Disk Guard & Cleanup ---");

    /* 1. Test space check for normal allocation (10 MB) */
    char *err_msg = NULL;
    bool ok = tc_disk_guard_check_space("/tmp", 10 * 1024 * 1024, &err_msg);
    g_assert_true(ok);
    g_assert_null(err_msg);
    tc_log_info("  ✓ Normal disk space check passed");

    /* 2. Test space check for impossible allocation (1,000,000 TB) */
    ok = tc_disk_guard_check_space("/tmp", 1000000000000000ULL, &err_msg);
    g_assert_false(ok);
    g_assert_nonnull(err_msg);
    tc_log_info("  ✓ Gargantuan disk request correctly rejected: '%s'", err_msg);
    g_free(err_msg);

    /* 3. Test stale temp file cleanup */
    g_autofree char *stale_file = NULL;
    int stale_fd = g_file_open_tmp(".tc_stale_unit_test_XXXXXX", &stale_file, NULL);
    g_assert_cmpint(stale_fd, >=, 0);
    close(stale_fd);
    FILE *f = fopen(stale_file, "wb");
    g_assert_nonnull(f);
    fputs("dummy data", f);
    fclose(f);

    /* Backdate mtime to 25 hours ago */
    struct utimbuf ubuf;
    ubuf.actime = time(NULL) - (25 * 3600);
    ubuf.modtime = time(NULL) - (25 * 3600);
    g_assert_cmpint(utime(stale_file, &ubuf), ==, 0);

    tc_disk_guard_cleanup_stale_temps();
    g_assert_true(g_file_test(stale_file, G_FILE_TEST_EXISTS));
    unlink(stale_file);
    tc_log_info("  Unowned temporary retained despite matching legacy name and age");
}

static void test_worker_pool_job_execution(void) {
    tc_log_info("--- [2/3] Testing Asynchronous Worker Pool Task Execution ---");

    /* Own the fixture; never write converted output into another project. */
    g_autofree char *fixture_dir = g_dir_make_tmp("tc-worker-test-XXXXXX", NULL);
    g_assert_nonnull(fixture_dir);
    g_autofree char *sample_img = g_build_filename(fixture_dir, "sample.ppm", NULL);
    const char ppm[] = "P6\n1 1\n255\n\xff\x00\x00";
    g_assert_true(g_file_set_contents(sample_img, ppm, sizeof(ppm) - 1, NULL));

    TcBatchContext *batch = tc_batch_context_new(1, -1);
    g_assert_nonnull(batch);

    TcJobItem *job = g_new0(TcJobItem, 1);
    job->src_path = g_strdup(sample_img);
    job->target_format = g_strdup("webp");
    job->action = TC_ACTION_CONVERT;
    job->overwrite_policy = TC_OVERWRITE_AUTO_NUMBER;
    job->batch = batch;

    tc_worker_pool_submit_job(job);

    /* Wait for completion with timeout */
    int loops = 0;
    while (loops < 50) {
        pthread_mutex_lock(&batch->mutex);
        uint32_t completed = batch->completed_jobs;
        pthread_mutex_unlock(&batch->mutex);
        if (completed >= 1) break;
        g_usleep(100000); /* 100ms */
        loops++;
    }

    pthread_mutex_lock(&batch->mutex);
    g_assert_cmpuint(batch->completed_jobs, ==, 1);
    g_assert_cmpuint(batch->failed_jobs, ==, 0);
    pthread_mutex_unlock(&batch->mutex);

    tc_log_info("  ✓ Worker pool successfully executed conversion job in worker thread");
    /* Join workers before releasing their shared batch context. */
    tc_worker_pool_shutdown();
    tc_batch_context_free(batch);
    g_autofree char *output = g_build_filename(fixture_dir, "sample.webp", NULL);
    unlink(output);
    unlink(sample_img);
    rmdir(fixture_dir);
    tc_worker_pool_init();
}

static void test_failsafe_size_guard(void) {
    tc_log_info("--- [3/3] Testing Universal Fail-Safe Size Guard ---");

    /* Create an already compressed tiny test video (1 second, low bitrate) */
    g_autofree char *tiny_dir = g_dir_make_tmp("tc-size-guard-XXXXXX", NULL);
    g_assert_nonnull(tiny_dir);
    g_autofree char *tiny_vid = g_build_filename(tiny_dir, "tiny.mp4", NULL);
    GError *error = NULL;
    GSubprocess *child = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, &error,
        "/usr/bin/ffmpeg", "-v", "error", "-nostdin", "-y", "-f", "lavfi", "-i",
        "testsrc=duration=1:size=160x120:rate=10", "-c:v", "libx264", "-crf", "38",
        "-pix_fmt", "yuv420p", tiny_vid, NULL);
    g_assert_no_error(error);
    g_assert_true(g_subprocess_wait_check(child, NULL, &error));
    g_assert_no_error(error);
    g_object_unref(child);
    g_assert_true(g_file_test(tiny_vid, G_FILE_TEST_EXISTS));

    struct stat st_in;
    g_assert_cmpint(stat(tiny_vid, &st_in), ==, 0);

    TcBatchContext *batch = tc_batch_context_new(1, -1);
    g_assert_nonnull(batch);

    TcJobItem *job = g_new0(TcJobItem, 1);
    job->src_path = g_strdup(tiny_vid);
    job->target_format = g_strdup("comp-web");
    job->action = TC_ACTION_COMPRESS;
    job->overwrite_policy = TC_OVERWRITE_AUTO_NUMBER;
    job->batch = batch;

    tc_worker_pool_submit_job(job);

    /* Wait for job completion */
    int loops = 0;
    while (loops < 100) {
        pthread_mutex_lock(&batch->mutex);
        uint32_t completed = batch->completed_jobs;
        pthread_mutex_unlock(&batch->mutex);
        if (completed >= 1) break;
        g_usleep(100000);
        loops++;
    }

    pthread_mutex_lock(&batch->mutex);
    g_assert_cmpuint(batch->completed_jobs, ==, 1);
    
    g_assert_cmpuint(batch->failed_jobs, ==, 0);
    if (batch->skipped_jobs > 0) {
        g_assert_cmpuint(batch->skipped_jobs, ==, 1);
        tc_log_info("  Candidate provided no size benefit; original preserved");
    } else {
        tc_log_info("  Candidate reduced file size");
    }
    pthread_mutex_unlock(&batch->mutex);

    tc_worker_pool_shutdown();
    tc_batch_context_free(batch);
    g_autofree char *candidate = g_build_filename(tiny_dir, "tiny_compressed.mp4", NULL);
    unlink(candidate);
    unlink(tiny_vid);
    g_assert_cmpint(rmdir(tiny_dir), ==, 0);
}

static void test_early_job_completion(void) {
    g_autofree char *dir = g_dir_make_tmp("tc-missing-parent-XXXXXX", NULL);
    g_assert_nonnull(dir);
    g_autofree char *src = g_build_filename(dir, "missing.png", NULL);
    g_assert_cmpint(rmdir(dir), ==, 0);

    for (int cancel = 0; cancel < 2; cancel++) {
        tc_worker_pool_init();
        TcBatchContext *batch = tc_batch_context_new(1, -1);
        TcJobItem *job = g_new0(TcJobItem, 1);
        job->src_path = g_strdup(src);
        job->target_format = g_strdup("webp");
        job->batch = batch;
        if (cancel) g_cancellable_cancel(batch->cancellable);
        tc_worker_pool_submit_job(job);
        tc_worker_pool_shutdown();
        g_assert_cmpuint(batch->completed_jobs, ==, 1);
        g_assert_cmpuint(batch->failed_jobs, ==, cancel?0:1);
        g_assert_cmpuint(batch->cancelled_jobs, ==, cancel?1:0);
        g_assert_cmpuint(batch->error_reports->len, ==, cancel?0:1);
        tc_batch_context_free(batch);
        /* Must be safe after the previously active context was freed. */
        tc_worker_pool_cancel_active();
    }
}

static void test_padding_worker(void) {
    g_autofree char *dir = g_dir_make_tmp("tc-padding-worker-XXXXXX", NULL);
    g_autofree char *source = g_build_filename(dir, "movie.mp4", NULL);
    g_autofree char *output = g_build_filename(dir, "movie_compressed.mp4", NULL);
    const char *argv[] = { "/usr/bin/ffmpeg", "-v", "error", "-nostdin", "-y", "-f", "lavfi", "-i",
                          "testsrc2=s=96x64:r=10:d=1", "-c:v", "libx264", "-threads:v", "1", source, NULL };
    const TcProcessOptions options = { .timeout_ms = 10000, .terminate_grace_ms = 100 };
    g_assert_cmpint(tc_process_run(argv, &options, NULL, NULL, NULL, NULL, NULL), ==, TC_STATUS_OK);
    g_autofree char *native = NULL;
    gsize native_size;
    g_assert_true(g_file_get_contents(source, &native, &native_size, NULL));
    g_autoptr(GByteArray) padded = g_byte_array_new();
    g_byte_array_append(padded, (guint8 *)native, native_size);
    guint32 padding_size = GUINT32_TO_BE(65536);
    g_byte_array_append(padded, (guint8 *)&padding_size, 4);
    g_byte_array_append(padded, (guint8 *)"free", 4);
    g_byte_array_set_size(padded, native_size + 65536);
    memset(padded->data + native_size + 8, 0, 65536 - 8);
    g_assert_true(g_file_set_contents(source, (char *)padded->data, padded->len, NULL));
    for (int no_benefit = 0; no_benefit < 2; no_benefit++) {
        tc_worker_pool_init();
        TcBatchContext *batch = tc_batch_context_new(1, -1);
        TcJobItem *job = g_new0(TcJobItem, 1);
        job->src_path = g_strdup(no_benefit ? output : source);
        job->target_format = g_strdup("comp-padding");
        job->action = TC_ACTION_COMPRESS;
        job->overwrite_policy = TC_OVERWRITE_AUTO_NUMBER;
        job->batch = batch;
        tc_worker_pool_submit_job(job);
        tc_worker_pool_shutdown();
        g_assert_cmpuint(batch->completed_jobs, ==, 1);
        g_assert_cmpuint(batch->failed_jobs, ==, 0);
        g_assert_cmpuint(batch->skipped_jobs, ==, no_benefit);
        tc_batch_context_free(batch);
    }
    g_autofree char *optimized = NULL, *unchanged = NULL;
    gsize optimized_size, source_size;
    g_assert_true(g_file_get_contents(output, &optimized, &optimized_size, NULL));
    g_assert_cmpmem(optimized, optimized_size, native, native_size);
    g_assert_true(g_file_get_contents(source, &unchanged, &source_size, NULL));
    g_assert_cmpmem(unchanged, source_size, padded->data, padded->len);
    g_autofree char *unneeded = g_build_filename(dir, "movie_compressed_compressed.mp4", NULL);
    g_assert_false(g_file_test(unneeded, G_FILE_TEST_EXISTS));
    unlink(source); unlink(output);
    /* Also detects any leaked sibling candidate after the no-benefit path. */
    g_assert_cmpint(rmdir(dir), ==, 0);
}

int main(int argc, char *argv[]) {
    g_test_init(&argc, &argv, NULL);

    tc_engines_init_registry();
    tc_worker_pool_init();

    test_disk_guard();
    test_worker_pool_job_execution();
    test_failsafe_size_guard();
    test_early_job_completion();
    test_padding_worker();

    tc_worker_pool_shutdown();
    tc_engines_cleanup_all();

    tc_log_info("=========================================================================");
    tc_log_info("✓ All Worker Pool & Safety Guard tests passed successfully!");
    tc_log_info("=========================================================================");
    return 0;
}
