#include "tc_worker_pool.h"
#include "tc_ipc.h"
#include "tc_governor.h"
#include "tc_runtime_limits.h"
#include <glib/gstdio.h>

/* A deterministic backend isolates pool ownership/commit behavior from codecs. */
static gint conversions;
static guint barrier_target, barrier_waiting;
static pthread_mutex_t barrier_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t barrier_cond;
static bool hold_media, media_entered;
static bool condition_wait_until(pthread_cond_t *cond,pthread_mutex_t *mutex,gint64 deadline){
    struct timespec until={.tv_sec=deadline/1000000,.tv_nsec=(deadline%1000000)*1000};
    return pthread_cond_timedwait(cond,mutex,&until)==0;
}

static TcStatusCode copy_convert(const char *src, const char *dst, const char *format,
    uint16_t w, uint16_t h, GCancellable *cancel, char **error,
    TcProgressCallback progress, void *data) {
    (void)w; (void)h; (void)error; (void)progress; (void)data;
    g_atomic_int_inc(&conversions);
    TcGovernorSnapshot resources;
    tc_governor_snapshot(tc_governor_default(), &resources);
    g_assert_cmpuint(resources.active_leases, >, 0);
    g_assert_cmpuint(resources.memory_bytes, >, 0);
    pthread_mutex_lock(&barrier_mutex);
    if (!strcmp(format, "mp4")) {
        media_entered = true;
        pthread_cond_broadcast(&barrier_cond);
        while (hold_media) pthread_cond_wait(&barrier_cond, &barrier_mutex);
    }
    if (barrier_target) {
        barrier_waiting++;
        pthread_cond_broadcast(&barrier_cond);
        while (barrier_waiting < barrier_target) pthread_cond_wait(&barrier_cond, &barrier_mutex);
    }
    pthread_mutex_unlock(&barrier_mutex);
    if (!strcmp(format, "fail")) return TC_STATUS_ERROR;
    if (!strcmp(format, "cancel")) g_cancellable_cancel(cancel);
    if (g_cancellable_is_cancelled(cancel)) return TC_STATUS_ERROR_CANCELLED;
    char *bytes = NULL;
    gsize length = 0;
    if (!g_file_get_contents(src, &bytes, &length, NULL)) return TC_STATUS_ERROR;
    gboolean ok = g_file_set_contents(dst, bytes, length, NULL);
    g_free(bytes);
    if(!strcmp(format,"cancel-after-write"))g_cancellable_cancel(cancel);
    if(!strcmp(format,"mutate-source"))g_assert_true(g_file_set_contents(src,"source changed by test actor",-1,NULL));
    return ok ? TC_STATUS_OK : TC_STATUS_ERROR;
}

TcEngine *tc_engines_find_for_task(TcCategory category, const char *format) {
    (void)category;
    static TcEngine engine = {.name = "test-copy", .convert = copy_convert};
    static TcEngine media = {.name = "test-media", .category = TC_CATEGORY_MEDIA, .convert = copy_convert};
    return !strcmp(format, "mp4") ? &media : &engine;
}

static void remove_dir(const char *path) {
    GDir *dir = g_dir_open(path, 0, NULL);
    const char *name;
    while (dir && (name = g_dir_read_name(dir))) {
        g_autofree char *file = g_build_filename(path, name, NULL);
        g_assert_cmpint(g_unlink(file), ==, 0);
    }
    if (dir) g_dir_close(dir);
    g_assert_cmpint(g_rmdir(path), ==, 0);
}

static void submit(TcBatchContext *batch, const char *source, TcOverwritePolicy policy) {
    TcJobItem *job = g_new0(TcJobItem, 1);
    job->src_path = g_strdup(source);
    job->target_format = g_strdup("out");
    job->overwrite_policy = policy;
    job->batch = batch;
    tc_worker_pool_submit_job(job);
}

static void weak_finalized(gpointer data, GObject *object) {
    (void)object;
    g_atomic_int_inc((gint *)data);
}

static void test_release_while_running(void) {
    g_autofree char *dir = g_dir_make_tmp("tc-lifetime-XXXXXX", NULL);
    g_autofree char *src = g_build_filename(dir, "data.in", NULL);
    g_assert_true(g_file_set_contents(src, "payload", -1, NULL));
    gint destroyed = 0;
    tc_worker_pool_init();
    TcBatchContext *batch = tc_batch_context_new(64, -1);
    g_object_weak_ref(G_OBJECT(batch->cancellable), weak_finalized, &destroyed);
    for (int i = 0; i < 64; i++) submit(batch, src, TC_OVERWRITE_AUTO_NUMBER);
    tc_batch_context_free(batch); /* production dispatcher releases before jobs finish */
    tc_worker_pool_shutdown();
    g_assert_cmpint(g_atomic_int_get(&destroyed), ==, 1);
    /* All 64 completions, including earlier workers finishing late, must
     * return their slots. Otherwise a full-size next batch is rejected. */
    TcBatchContext *capacity=tc_batch_context_new(TC_IPC_MAX_FILES,-1);
    g_assert_true(tc_worker_pool_begin_batch(capacity,capacity->batch_id));
    tc_worker_pool_abort_batch(capacity);tc_batch_context_free(capacity);
    remove_dir(dir);
}

static void test_autonumber_collision(void) {
    g_autofree char *dir = g_dir_make_tmp("tc-commit-race-XXXXXX", NULL);
    g_autofree char *src = g_build_filename(dir, "data.in", NULL);
    g_assert_true(g_file_set_contents(src, "payload", -1, NULL));
    barrier_target = MIN(4U, tc_worker_thread_limit());
    barrier_waiting = 0;
    tc_worker_pool_init();
    TcBatchContext *batch = tc_batch_context_new(4, -1);
    for (int i = 0; i < 4; i++) submit(batch, src, TC_OVERWRITE_AUTO_NUMBER);
    tc_worker_pool_shutdown();
    barrier_target = 0;
    g_assert_cmpuint(batch->completed_jobs, ==, 4);
    g_assert_cmpuint(batch->failed_jobs, ==, 0);
    for (int i = 0; i < 4; i++) {
        g_autofree char *name = i ? g_strdup_printf("data (%d).out", i) : g_strdup("data.out");
        g_autofree char *path = g_build_filename(dir, name, NULL);
        char *bytes = NULL;
        g_assert_true(g_file_get_contents(path, &bytes, NULL, NULL));
        g_assert_cmpstr(bytes, ==, "payload");
        g_free(bytes);
    }
    tc_batch_context_free(batch);
    remove_dir(dir);
}

static void test_skip_and_replace(void) {
    g_autofree char *dir = g_dir_make_tmp("tc-overwrite-XXXXXX", NULL);
    g_autofree char *src = g_build_filename(dir, "data.in", NULL);
    g_autofree char *dst = g_build_filename(dir, "data.out", NULL);
    g_assert_true(g_file_set_contents(src, "new", -1, NULL));
    g_assert_true(g_file_set_contents(dst, "old", -1, NULL));
    g_atomic_int_set(&conversions, 0);
    tc_worker_pool_init();
    TcBatchContext *batch = tc_batch_context_new(1, -1);
    submit(batch, src, TC_OVERWRITE_SKIP);
    tc_worker_pool_shutdown();
    g_assert_cmpint(g_atomic_int_get(&conversions), ==, 0);
    g_assert_cmpuint(batch->skipped_jobs, ==, 1);
    g_assert_cmpuint(batch->failed_jobs, ==, 0);
    tc_batch_context_free(batch);
    char *bytes = NULL;
    g_assert_true(g_file_get_contents(dst, &bytes, NULL, NULL));
    g_assert_cmpstr(bytes, ==, "old");
    g_free(bytes);
    tc_worker_pool_init();
    batch = tc_batch_context_new(1, -1);
    submit(batch, src, TC_OVERWRITE_REPLACE);
    tc_worker_pool_shutdown();
    g_assert_cmpuint(batch->failed_jobs, ==, 0);
    tc_batch_context_free(batch);
    g_assert_true(g_file_get_contents(dst, &bytes, NULL, NULL));
    g_assert_cmpstr(bytes, ==, "new");
    g_free(bytes);
    remove_dir(dir);
}

static void test_dangling_symlink(void) {
    g_autofree char *dir = g_dir_make_tmp("tc-symlink-XXXXXX", NULL);
    g_autofree char *src = g_build_filename(dir, "data.in", NULL);
    g_autofree char *dst = g_build_filename(dir, "data.out", NULL);
    g_autofree char *numbered = g_build_filename(dir, "data (1).out", NULL);
    g_assert_true(g_file_set_contents(src, "payload", -1, NULL));
    g_assert_cmpint(symlink("missing", dst), ==, 0);
    tc_worker_pool_init();
    TcBatchContext *batch = tc_batch_context_new(1, -1);
    submit(batch, src, TC_OVERWRITE_AUTO_NUMBER);
    tc_worker_pool_shutdown();
    g_assert_cmpuint(batch->failed_jobs, ==, 0);
    g_assert_true(g_file_test(dst, G_FILE_TEST_IS_SYMLINK));
    g_assert_true(g_file_test(numbered, G_FILE_TEST_IS_REGULAR));
    tc_batch_context_free(batch);
    remove_dir(dir);
}

static void test_stalled_listener(void) {
    int sockets[2];
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, sockets), ==, 0);
    char fill[4096] = {0};
    while (tc_ipc_send_bytes(sockets[0], fill, sizeof(fill), MSG_NOSIGNAL) > 0) {}
    g_assert_true(errno == EAGAIN || errno == EWOULDBLOCK);
    tc_worker_pool_register_ui_listener(sockets[0]);
    g_autofree char *dir = g_dir_make_tmp("tc-stalled-hud-XXXXXX", NULL);
    g_autofree char *src = g_build_filename(dir, "data.in", NULL);
    g_assert_true(g_file_set_contents(src, "payload", -1, NULL));
    tc_worker_pool_init();
    TcBatchContext *batch = tc_batch_context_new(1, -1);
    submit(batch, src, TC_OVERWRITE_AUTO_NUMBER);
    tc_worker_pool_shutdown();
    g_assert_cmpuint(batch->completed_jobs, ==, 1);
    g_assert_cmpuint(batch->failed_jobs, ==, 0);
    tc_batch_context_free(batch);
    close(sockets[1]);
    remove_dir(dir);
}

static void test_late_hud(void) {
    g_autofree char *dir = g_dir_make_tmp("tc-late-hud-XXXXXX", NULL);
    g_autofree char *src = g_build_filename(dir, "data.in", NULL);
    g_assert_true(g_file_set_contents(src, "payload", -1, NULL));
    tc_worker_pool_init();
    TcBatchContext *batch = tc_batch_context_new(1, -1);
    submit(batch, src, TC_OVERWRITE_AUTO_NUMBER);
    tc_batch_context_free(batch);
    tc_worker_pool_shutdown();
    int sockets[2];
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, sockets), ==, 0);
    tc_worker_pool_register_ui_listener(sockets[0]);
    TcIpcProgressReader reader = {0};
    TcIpcProgressPayload payload = {0};
    g_assert_cmpint(tc_ipc_read_progress(sockets[1], &reader, &payload), ==, 1);
    g_assert_cmpuint(payload.completed_files, ==, 1);
    g_assert_cmpuint(payload.total_files, ==, 1);
    close(sockets[1]);
    remove_dir(dir);
}

static void assert_resources_released(void) {
    TcGovernorSnapshot snapshot;
    tc_governor_snapshot(tc_governor_default(), &snapshot);
    g_assert_cmpuint(snapshot.active_leases, ==, 0);
    g_assert_cmpuint(snapshot.waiting_jobs, ==, 0);
    g_assert_cmpuint(snapshot.memory_bytes, ==, 0);
    g_assert_cmpuint(snapshot.cpu_units, ==, 0);
    g_assert_cmpuint(snapshot.media_jobs, ==, 0);
    g_assert_cmpuint(snapshot.disk_bytes, ==, 0);
    g_assert_cmpuint(snapshot.filesystems, ==, 0);
}

static void wait_completed(TcBatchContext *batch) {
    gint64 deadline = g_get_monotonic_time() + 2000000;
    uint32_t completed;
    do {
        pthread_mutex_lock(&batch->mutex);
        completed = batch->completed_jobs;
        pthread_mutex_unlock(&batch->mutex);
        if (completed) return;
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
    g_assert_cmpuint(completed, ==, 1);
}

static void submit_format(TcBatchContext *batch, const char *source, const char *format, TcActionType action) {
    TcJobItem *job = g_new0(TcJobItem, 1);
    job->src_path = g_strdup(source);
    job->target_format = g_strdup(format);
    job->action = action;
    job->batch = batch;
    tc_worker_pool_submit_job(job);
}

static void test_media_queue_and_general_work(void) {
    if (tc_worker_thread_limit() <= tc_engine_thread_limit()) {
        g_test_skip("No spare admitted CPU unit alongside a media job on this machine");
        return;
    }
    g_autofree char *dir = g_dir_make_tmp("tc-media-queue-XXXXXX", NULL);
    g_autofree char *source = g_build_filename(dir, "data.in", NULL);
    g_assert_true(g_file_set_contents(source, "payload", -1, NULL));
    hold_media = true; media_entered = false;
    g_atomic_int_set(&conversions, 0);
    tc_worker_pool_init();
    TcBatchContext *active = tc_batch_context_new(1, -1);
    TcBatchContext *cancelled = tc_batch_context_new(1, -1);
    TcBatchContext *general = tc_batch_context_new(1, -1);
    submit_format(active, source, "mp4", TC_ACTION_CONVERT);
    pthread_mutex_lock(&barrier_mutex);
    gint64 deadline = g_get_monotonic_time() + 2000000;
    while (!media_entered && condition_wait_until(&barrier_cond, &barrier_mutex, deadline)) {}
    g_assert_true(media_entered);
    pthread_mutex_unlock(&barrier_mutex);
    submit_format(cancelled, source, "mp4", TC_ACTION_CONVERT);
    g_cancellable_cancel(cancelled->cancellable);
    submit(general, source, TC_OVERWRITE_AUTO_NUMBER);
    wait_completed(general); /* Must finish while the first video is held. */
    pthread_mutex_lock(&general->mutex);
    g_assert_cmpuint(general->failed_jobs, ==, 0);
    pthread_mutex_unlock(&general->mutex);
    pthread_mutex_lock(&barrier_mutex);
    hold_media = false;
    pthread_cond_broadcast(&barrier_cond);
    pthread_mutex_unlock(&barrier_mutex);
    tc_worker_pool_shutdown();
    g_assert_cmpint(g_atomic_int_get(&conversions), ==, 2);
    g_assert_cmpuint(active->completed_jobs, ==, 1);
    g_assert_cmpuint(active->failed_jobs, ==, 0);
    g_assert_cmpuint(cancelled->completed_jobs, ==, 1);
    g_assert_cmpuint(cancelled->cancelled_jobs, ==, 1);
    assert_resources_released();
    tc_batch_context_free(active); tc_batch_context_free(cancelled); tc_batch_context_free(general);
    remove_dir(dir);
}

static void test_cancel_waiting_admission(void) {
    g_autofree char *dir = g_dir_make_tmp("tc-worker-admission-XXXXXX", NULL);
    g_autofree char *source = g_build_filename(dir, "data.in", NULL);
    g_assert_true(g_file_set_contents(source, "payload", -1, NULL));
    const TcResourceRequest all_memory = { .memory_bytes = 1024ULL * 1024 * 1024, .cpu_units = 1 };
    TcResourceLease *held = NULL;
    g_assert_cmpint(tc_governor_acquire(tc_governor_default(), &all_memory, dir, NULL, 0, &held, NULL), ==, TC_STATUS_OK);
    tc_worker_pool_init();
    TcBatchContext *batch = tc_batch_context_new(1, -1);
    g_atomic_int_set(&conversions, 0);
    submit(batch, source, TC_OVERWRITE_AUTO_NUMBER);
    TcGovernorSnapshot snapshot;
    gint64 deadline = g_get_monotonic_time() + 1000000;
    do {
        tc_governor_snapshot(tc_governor_default(), &snapshot);
        if (snapshot.waiting_jobs) break;
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
    g_assert_cmpuint(snapshot.waiting_jobs, ==, 1);
    g_cancellable_cancel(batch->cancellable);
    wait_completed(batch);
    tc_worker_pool_shutdown();
    g_assert_cmpint(g_atomic_int_get(&conversions), ==, 0);
    g_assert_cmpuint(batch->cancelled_jobs, ==, 1);
    g_autoptr(GDir) files = g_dir_open(dir, 0, NULL);
    g_assert_cmpstr(g_dir_read_name(files), ==, "data.in");
    g_assert_null(g_dir_read_name(files)); /* Admission runs before temp creation. */
    tc_governor_release(&held);
    assert_resources_released();
    tc_batch_context_free(batch);
    remove_dir(dir);
}

static void test_resource_cleanup_paths(void) {
    g_autofree char *dir = g_dir_make_tmp("tc-worker-budget-cleanup-XXXXXX", NULL);
    g_autofree char *source = g_build_filename(dir, "data.in", NULL);
    g_assert_true(g_file_set_contents(source, "payload", -1, NULL));
    const char *formats[] = { "fail", "cancel", "comp-office" };
    for (guint i = 0; i < G_N_ELEMENTS(formats); i++) {
        tc_worker_pool_init();
        TcBatchContext *batch = tc_batch_context_new(1, -1);
        submit_format(batch, source, formats[i], i == 2 ? TC_ACTION_COMPRESS : TC_ACTION_CONVERT);
        tc_worker_pool_shutdown();
        g_assert_cmpuint(batch->completed_jobs, ==, 1);
        g_assert_cmpuint(batch->failed_jobs, ==, i == 0 ? 1 : 0);
        g_assert_cmpuint(batch->cancelled_jobs, ==, i == 1 ? 1 : 0);
        g_assert_cmpuint(batch->skipped_jobs, ==, i == 2 ? 1 : 0);
        assert_resources_released();
        tc_batch_context_free(batch);
    }
    g_autoptr(GDir) files = g_dir_open(dir, 0, NULL);
    g_assert_cmpstr(g_dir_read_name(files), ==, "data.in");
    g_assert_null(g_dir_read_name(files));
    remove_dir(dir);
}

static void test_targeted_cancel_and_replay(void) {
    g_autofree char *dir=g_dir_make_tmp("tc-batch-isolation-XXXXXX",NULL);
    g_autofree char *source=g_build_filename(dir,"data.in",NULL);
    g_assert_true(g_file_set_contents(source,"payload",-1,NULL));
    g_atomic_int_set(&conversions,0);hold_media=true;media_entered=false;
    tc_worker_pool_init();
    TcBatchContext *a=tc_batch_context_new(1,-1),*b=tc_batch_context_new(1,-1);
    a->protocol_version=b->protocol_version=2;
    submit_format(a,source,"mp4",TC_ACTION_CONVERT);
    pthread_mutex_lock(&barrier_mutex);
    gint64 until=g_get_monotonic_time()+2000000;
    while(!media_entered&&condition_wait_until(&barrier_cond,&barrier_mutex,until)){}
    g_assert_true(media_entered);pthread_mutex_unlock(&barrier_mutex);
    submit_format(b,source,"mp4",TC_ACTION_CONVERT);
    uint64_t id=b->batch_id;
    g_assert_true(tc_worker_pool_cancel_batch(id));
    wait_completed(b); /* Completes before a blocked, unrelated media job. */
    pthread_mutex_lock(&b->mutex);g_assert_cmpuint(b->cancelled_jobs,==,1);pthread_mutex_unlock(&b->mutex);
    pthread_mutex_lock(&a->mutex);g_assert_cmpuint(a->completed_jobs,==,0);pthread_mutex_unlock(&a->mutex);
    g_assert_cmpint(g_atomic_int_get(&conversions),==,1);
    int pair[2];g_assert_cmpint(socketpair(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0,pair),==,0);
    tc_worker_pool_register_batch_listener(id,pair[0]);
    TcIpcProgressReaderV2 reader={0};TcIpcProgressV2 event={0};
    g_assert_cmpint(tc_ipc_read_progress_v2(pair[1],&reader,&event),==,1);
    g_assert_cmpuint(event.batch_id,==,id);g_assert_cmpuint(event.cancelled_files,==,1);
    g_assert_cmpuint(event.progress.failed_files,==,0);close(pair[1]);
    pthread_mutex_lock(&barrier_mutex);hold_media=false;pthread_cond_broadcast(&barrier_cond);pthread_mutex_unlock(&barrier_mutex);
    tc_worker_pool_shutdown();g_assert_cmpuint(a->saved_jobs,==,1);
    tc_batch_context_free(a);tc_batch_context_free(b);remove_dir(dir);
}

static void test_bounded_batches_and_legacy_cancel(void) {
    TcBatchContext *full=tc_batch_context_new(TC_IPC_MAX_FILES,-1),*extra=tc_batch_context_new(1,-1);
    g_assert_true(tc_worker_pool_begin_batch(full,full->batch_id));
    g_assert_false(tc_worker_pool_begin_batch(extra,extra->batch_id));
    tc_worker_pool_abort_batch(full);
    g_assert_true(tc_worker_pool_begin_batch(extra,extra->batch_id));
    TcBatchContext *later=tc_batch_context_new(1,-1);
    g_assert_true(tc_worker_pool_begin_batch(later,later->batch_id));
    tc_worker_pool_cancel_active(); /* ambiguous legacy request must cancel neither */
    g_assert_false(g_cancellable_is_cancelled(extra->cancellable));
    g_assert_false(g_cancellable_is_cancelled(later->cancellable));
    tc_worker_pool_abort_batch(later); /* the older remaining batch is now unambiguous */
    tc_worker_pool_cancel_active();g_assert_true(g_cancellable_is_cancelled(extra->cancellable));
    tc_worker_pool_abort_batch(extra);
    tc_batch_context_free(full);tc_batch_context_free(extra);tc_batch_context_free(later);
}

static void test_reject_candidate_before_publish(void) {
    g_autofree char *dir=g_dir_make_tmp("tc-final-guard-XXXXXX",NULL),*src=g_build_filename(dir,"data.in",NULL);
    const char *formats[]={"cancel-after-write","mutate-source"};
    for(unsigned i=0;i<G_N_ELEMENTS(formats);i++){
        g_assert_true(g_file_set_contents(src,"initial-source",-1,NULL));
        tc_worker_pool_init();TcBatchContext *batch=tc_batch_context_new(1,-1);
        submit_format(batch,src,formats[i],TC_ACTION_CONVERT);tc_worker_pool_shutdown();
        g_assert_cmpuint(batch->saved_jobs,==,0);
        g_assert_cmpuint(batch->cancelled_jobs,==,i?0:1);g_assert_cmpuint(batch->failed_jobs,==,i?1:0);
        g_autoptr(GDir) files=g_dir_open(dir,0,NULL);g_assert_cmpstr(g_dir_read_name(files),==,"data.in");g_assert_null(g_dir_read_name(files));
        tc_batch_context_free(batch);assert_resources_released();
    }
    remove_dir(dir);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/worker/cancel-and-source-change-before-publish",test_reject_candidate_before_publish);
    g_test_add_func("/worker/bounded-batches-and-unambiguous-legacy-cancel",test_bounded_batches_and_legacy_cancel);
    pthread_condattr_t attributes;pthread_condattr_init(&attributes);pthread_condattr_setclock(&attributes,CLOCK_MONOTONIC);
    pthread_cond_init(&barrier_cond,&attributes);pthread_condattr_destroy(&attributes);
    g_test_add_func("/worker/release-caller-reference", test_release_while_running);
    g_test_add_func("/worker/auto-number-race", test_autonumber_collision);
    g_test_add_func("/worker/skip-replace", test_skip_and_replace);
    g_test_add_func("/worker/dangling-symlink", test_dangling_symlink);
    g_test_add_func("/worker/stalled-listener", test_stalled_listener);
    g_test_add_func("/worker/late-hud", test_late_hud);
    g_test_add_func("/worker/media-queue-general-work", test_media_queue_and_general_work);
    g_test_add_func("/worker/cancel-admission-before-temp", test_cancel_waiting_admission);
    g_test_add_func("/worker/resource-cleanup-error-cancel-no-benefit", test_resource_cleanup_paths);
    g_test_add_func("/worker/targeted-cancel-and-late-v2-replay",test_targeted_cancel_and_replay);
    return g_test_run();
}
