#include "tc_governor.h"
#include "tc_disk_guard.h"
#include <glib/gstdio.h>
#include <pthread.h>
#include <stdatomic.h>

/* Only the isolated admission tests replace filesystem observations. No mount,
 * quota, user file, or production syscall behavior is changed. */
static atomic_bool mock_space, query_failure;
static atomic_uint_fast64_t available_a, available_b;
static ino_t inode_a, inode_b;
static char *root, *dir_a, *dir_b;
int __real_fstat(int fd, struct stat *st);
int __real_fstatvfs(int fd, struct statvfs *vfs);
int __real_fstat64(int fd, struct stat64 *st);
int __real_fstatvfs64(int fd, struct statvfs64 *vfs);

int __wrap_fstat(int fd, struct stat *st) {
    int result = __real_fstat(fd, st);
    if (result == 0 && atomic_load(&mock_space)) st->st_dev = st->st_ino == inode_b ? 22 : 11;
    return result;
}

int __wrap_fstatvfs(int fd, struct statvfs *vfs) {
    if (!atomic_load(&mock_space)) return __real_fstatvfs(fd, vfs);
    if (atomic_load(&query_failure)) { errno = EIO; return -1; }
    struct stat st;
    if (__real_fstat(fd, &st) != 0) return -1;
    memset(vfs, 0, sizeof(*vfs));
    vfs->f_frsize = 1;
    vfs->f_bavail = atomic_load(st.st_ino == inode_b ? &available_b : &available_a);
    vfs->f_bfree = vfs->f_blocks = vfs->f_bavail;
    return 0;
}

int __wrap_fstat64(int fd, struct stat64 *st) {
    int result = __real_fstat64(fd, st);
    if (result == 0 && atomic_load(&mock_space)) st->st_dev = st->st_ino == inode_b ? 22 : 11;
    return result;
}

int __wrap_fstatvfs64(int fd, struct statvfs64 *vfs) {
    if (!atomic_load(&mock_space)) return __real_fstatvfs64(fd, vfs);
    if (atomic_load(&query_failure)) { errno = EIO; return -1; }
    struct stat64 st;
    if (__real_fstat64(fd, &st) != 0) return -1;
    memset(vfs, 0, sizeof(*vfs));
    vfs->f_frsize = 1;
    vfs->f_bavail = atomic_load(st.st_ino == inode_b ? &available_b : &available_a);
    vfs->f_bfree = vfs->f_blocks = vfs->f_bavail;
    return 0;
}

static void setup(void) {
    root = g_dir_make_tmp("tc-governor-XXXXXX", NULL);
    g_assert_nonnull(root);
    dir_a = g_build_filename(root, "a", NULL);
    dir_b = g_build_filename(root, "b", NULL);
    g_assert_cmpint(g_mkdir(dir_a, 0700), ==, 0);
    g_assert_cmpint(g_mkdir(dir_b, 0700), ==, 0);
    struct stat st;
    g_assert_cmpint(stat(dir_a, &st), ==, 0); inode_a = st.st_ino;
    g_assert_cmpint(stat(dir_b, &st), ==, 0); inode_b = st.st_ino;
    g_assert_cmpuint(inode_a, !=, inode_b);
    atomic_store(&available_a, 10000);
    atomic_store(&available_b, 10000);
    atomic_store(&query_failure, false);
    atomic_store(&mock_space, true);
}

static void teardown(void) {
    atomic_store(&mock_space, false);
    g_assert_cmpint(g_rmdir(dir_a), ==, 0);
    g_assert_cmpint(g_rmdir(dir_b), ==, 0);
    g_assert_cmpint(g_rmdir(root), ==, 0);
    g_free(dir_a); g_free(dir_b); g_free(root);
}

static TcGovernor *make_governor(void) {
    const TcGovernorLimits limits = { .memory_bytes = 1024, .cpu_units = 8,
                                      .media_jobs = 1, .emergency_disk_bytes = 100 };
    TcGovernor *governor = tc_governor_new(&limits);
    g_assert_nonnull(governor);
    return governor;
}

static const TcResourceRequest small = { .memory_bytes = 64, .output_bytes = 100, .cpu_units = 1 };

static void assert_empty(TcGovernor *governor) {
    TcGovernorSnapshot snapshot;
    tc_governor_snapshot(governor, &snapshot);
    g_assert_cmpuint(snapshot.active_leases, ==, 0);
    g_assert_cmpuint(snapshot.waiting_jobs, ==, 0);
    g_assert_cmpuint(snapshot.memory_bytes, ==, 0);
    g_assert_cmpuint(snapshot.cpu_units, ==, 0);
    g_assert_cmpuint(snapshot.media_jobs, ==, 0);
    g_assert_cmpuint(snapshot.disk_bytes, ==, 0);
    g_assert_cmpuint(snapshot.filesystems, ==, 0);
}

static void test_accounting(void) {
    setup();
    TcGovernor *governor = make_governor();
    TcResourceLease *first = NULL, *second = NULL;
    g_assert_cmpint(tc_governor_acquire(governor, &small, dir_a, NULL, 0, &first, NULL), ==, TC_STATUS_OK);
    g_assert_cmpint(tc_governor_acquire(governor, &small, dir_a, NULL, 0, &second, NULL), ==, TC_STATUS_OK);
    TcGovernorSnapshot snapshot;
    tc_governor_snapshot(governor, &snapshot);
    g_assert_cmpuint(snapshot.active_leases, ==, 2);
    g_assert_cmpuint(snapshot.memory_bytes, ==, 128);
    g_assert_cmpuint(snapshot.cpu_units, ==, 2);
    g_assert_cmpuint(snapshot.disk_bytes, ==, 240);
    g_assert_cmpuint(snapshot.filesystems, ==, 1);
    tc_governor_release(&first);
    tc_governor_release(&first); /* Cleared ownership can safely be released again. */
    tc_governor_release(&second);
    assert_empty(governor);
    tc_governor_free(governor);
    teardown();
}

static void test_disk_isolation(void) {
    setup();
    atomic_store(&available_a, 300); atomic_store(&available_b, 300);
    TcGovernor *governor = make_governor();
    TcResourceLease *a = NULL, *b = NULL, *blocked = NULL;
    g_assert_cmpint(tc_governor_acquire(governor, &small, dir_a, NULL, 0, &a, NULL), ==, TC_STATUS_OK);
    g_assert_cmpint(tc_governor_acquire(governor, &small, dir_a, NULL, 0, &blocked, NULL), ==, TC_STATUS_ERROR_TIMEOUT);
    g_assert_null(blocked);
    g_assert_cmpint(tc_governor_acquire(governor, &small, dir_b, NULL, 0, &b, NULL), ==, TC_STATUS_OK);
    TcGovernorSnapshot snapshot;
    tc_governor_snapshot(governor, &snapshot);
    g_assert_cmpuint(snapshot.filesystems, ==, 2);
    tc_governor_release(&a); tc_governor_release(&b);
    assert_empty(governor);
    tc_governor_free(governor);
    teardown();
}

static void test_output_headroom(void) {
    setup();
    TcGovernor *governor = make_governor();
    TcResourceLease *first = NULL, *second = NULL;
    g_assert_cmpint(tc_governor_acquire(governor, &small, dir_a, NULL, 0, &first, NULL), ==, TC_STATUS_OK);
    /* A completed candidate already consumed disk space: do not count its own
     * original estimate again when checking the remaining free space. */
    atomic_store(&available_a, 100);
    g_assert_cmpint(tc_governor_check_output(first, 11, NULL), ==, TC_STATUS_OK);
    atomic_store(&available_a, 99);
    g_assert_cmpint(tc_governor_check_output(first, 11, NULL), ==, TC_STATUS_ERROR_NO_SPACE);
    atomic_store(&available_a, 10000);
    g_assert_cmpint(tc_governor_acquire(governor, &small, dir_a, NULL, 0, &second, NULL), ==, TC_STATUS_OK);
    atomic_store(&available_a, 219); /* 100 buffer + other job's 120 charge. */
    g_assert_cmpint(tc_governor_check_output(first, 11, NULL), ==, TC_STATUS_ERROR_NO_SPACE);
    atomic_store(&available_a, 220);
    g_assert_cmpint(tc_governor_check_output(first, 11, NULL), ==, TC_STATUS_OK);
    g_assert_cmpint(tc_governor_check_output(first, 22, NULL), ==, TC_STATUS_ERROR);
    tc_governor_release(&first); tc_governor_release(&second);
    assert_empty(governor);
    tc_governor_free(governor);
    teardown();
}

static void test_rejections(void) {
    setup();
    TcGovernor *governor = make_governor();
    TcResourceLease *lease = NULL;
    TcResourceRequest request = small;
    request.memory_bytes = 1025;
    g_assert_cmpint(tc_governor_acquire(governor, &request, dir_a, NULL, 0, &lease, NULL), ==, TC_STATUS_ERROR_MEM_LIMIT);
    request = small; request.cpu_units = 9;
    g_assert_cmpint(tc_governor_acquire(governor, &request, dir_a, NULL, 0, &lease, NULL), ==, TC_STATUS_ERROR);
    request = small; request.output_bytes = UINT64_MAX;
    g_assert_cmpint(tc_governor_acquire(governor, &request, dir_a, NULL, 0, &lease, NULL), ==, TC_STATUS_ERROR_NO_SPACE);
    atomic_store(&available_a, 219);
    g_assert_cmpint(tc_governor_acquire(governor, &small, dir_a, NULL, 0, &lease, NULL), ==, TC_STATUS_ERROR_NO_SPACE);
    atomic_store(&query_failure, true);
    g_assert_cmpint(tc_governor_acquire(governor, &small, dir_a, NULL, 0, &lease, NULL), ==, TC_STATUS_ERROR);
    atomic_store(&query_failure, false);
    g_assert_cmpint(tc_governor_acquire(governor, &small, "/missing-tc-governor-output", NULL, 0, &lease, NULL), ==, TC_STATUS_ERROR);
    g_autoptr(GCancellable) cancelled = g_cancellable_new();
    g_cancellable_cancel(cancelled);
    g_assert_cmpint(tc_governor_acquire(governor, &small, dir_a, cancelled, 0, &lease, NULL), ==, TC_STATUS_ERROR_CANCELLED);
    g_assert_null(lease);
    assert_empty(governor);
    tc_governor_free(governor);
    teardown();
}

typedef struct {
    TcGovernor *governor;
    TcResourceRequest request;
    GCancellable *cancellable;
    gint64 timeout_ms;
    TcStatusCode result;
} WaitJob;

static void *wait_job(void *opaque) {
    WaitJob *job = opaque;
    TcResourceLease *lease = NULL;
    job->result = tc_governor_acquire(job->governor, &job->request, dir_a,
        job->cancellable, job->timeout_ms, &lease, NULL);
    tc_governor_release(&lease);
    return NULL;
}

static void wait_for_queue(TcGovernor *governor, guint count) {
    gint64 deadline = g_get_monotonic_time() + 1000000;
    TcGovernorSnapshot snapshot;
    do {
        tc_governor_snapshot(governor, &snapshot);
        if (snapshot.waiting_jobs == count) return;
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
    g_assert_cmpuint(snapshot.waiting_jobs, ==, count);
}

static void test_wait_paths(gconstpointer mode) {
    setup();
    TcGovernor *governor = make_governor();
    TcResourceRequest full = small; full.memory_bytes = 1024;
    if (!strcmp(mode, "cpu")) { full = small; full.cpu_units = 8; }
    if (!strcmp(mode, "media")) { full = small; full.media_job = true; }
    if (!strcmp(mode, "disk")) { full = small; full.output_bytes = 6000; }
    TcResourceLease *held = NULL;
    g_assert_cmpint(tc_governor_acquire(governor, &full, dir_a, NULL, 0, &held, NULL), ==, TC_STATUS_OK);
    g_autoptr(GCancellable) cancellable = g_cancellable_new();
    WaitJob job = { governor, small, cancellable, 2000, TC_STATUS_ERROR };
    if (!strcmp(mode, "media")) job.request.media_job = true;
    if (!strcmp(mode, "disk")) job.request.output_bytes = 3000;
    gboolean timeout = !strcmp(mode, "timeout");
    if (timeout) job.timeout_ms = 80;
    pthread_t thread;
    g_assert_cmpint(pthread_create(&thread, NULL, wait_job, &job), ==, 0);
    wait_for_queue(governor, 1);
    gint64 started = g_get_monotonic_time();
    gboolean cancel = !strcmp(mode, "cancel");
    if (!timeout && !cancel) tc_governor_release(&held);
    else if (cancel) g_cancellable_cancel(cancellable);
    g_assert_cmpint(pthread_join(thread, NULL), ==, 0);
    g_assert_cmpint(g_get_monotonic_time() - started, <, 1000000);
    g_assert_cmpint(job.result, ==, timeout ? TC_STATUS_ERROR_TIMEOUT :
        cancel ? TC_STATUS_ERROR_CANCELLED : TC_STATUS_OK);
    tc_governor_release(&held);
    /* A cancelled/timed-out head must not strand the next request. */
    g_assert_cmpint(tc_governor_acquire(governor, &small, dir_a, NULL, 0, &held, NULL), ==, TC_STATUS_OK);
    tc_governor_release(&held);
    assert_empty(governor);
    tc_governor_free(governor);
    teardown();
}

typedef struct {
    TcGovernor *governor;
    pthread_barrier_t barrier;
    pthread_mutex_t counter_mutex;
    guint current, peak, completed;
} Stress;

static void *stress_job(void *opaque) {
    Stress *stress = opaque;
    int barrier_status = pthread_barrier_wait(&stress->barrier);
    g_assert_true(barrier_status == 0 || barrier_status == PTHREAD_BARRIER_SERIAL_THREAD);
    TcResourceRequest media = small; media.media_job = true;
    for (guint i = 0; i < 12; i++) {
        TcResourceLease *lease = NULL;
        g_assert_cmpint(tc_governor_acquire(stress->governor, &media, dir_a, NULL, 5000, &lease, NULL), ==, TC_STATUS_OK);
        pthread_mutex_lock(&stress->counter_mutex);
        stress->current++;
        stress->peak = MAX(stress->peak, stress->current);
        pthread_mutex_unlock(&stress->counter_mutex);
        g_usleep(1000);
        pthread_mutex_lock(&stress->counter_mutex);
        stress->current--; stress->completed++;
        pthread_mutex_unlock(&stress->counter_mutex);
        tc_governor_release(&lease);
    }
    return NULL;
}

static void test_concurrent_media(void) {
    setup();
    Stress stress = { .governor = make_governor(), .counter_mutex = PTHREAD_MUTEX_INITIALIZER };
    pthread_t threads[8];
    g_assert_cmpint(pthread_barrier_init(&stress.barrier, NULL, 8), ==, 0);
    for (guint i = 0; i < 8; i++) g_assert_cmpint(pthread_create(&threads[i], NULL, stress_job, &stress), ==, 0);
    for (guint i = 0; i < 8; i++) g_assert_cmpint(pthread_join(threads[i], NULL), ==, 0);
    g_assert_cmpuint(stress.peak, ==, 1);
    g_assert_cmpuint(stress.completed, ==, 96);
    assert_empty(stress.governor);
    pthread_barrier_destroy(&stress.barrier);
    pthread_mutex_destroy(&stress.counter_mutex);
    tc_governor_free(stress.governor);
    teardown();
}

static void test_disk_math(void) {
    uint64_t charge;
    g_assert_true(tc_disk_guard_charge(0, &charge)); g_assert_cmpuint(charge, ==, 0);
    g_assert_true(tc_disk_guard_charge(1, &charge)); g_assert_cmpuint(charge, ==, 2);
    g_assert_true(tc_disk_guard_charge(100, &charge)); g_assert_cmpuint(charge, ==, 120);
    g_assert_false(tc_disk_guard_charge(UINT64_MAX, &charge));
    g_autofree char *error = NULL;
    g_assert_false(tc_disk_guard_check_space("/missing-tc-disk-query", 1, &error));
    g_assert_nonnull(error);
}

static guint open_fd_count(void) {
    g_autoptr(GDir) dir = g_dir_open("/proc/self/fd", 0, NULL);
    g_assert_nonnull(dir);
    guint count = 0;
    while (g_dir_read_name(dir)) count++;
    return count;
}

static void test_fd_cleanup(void) {
    setup();
    TcGovernor *governor = make_governor();
    guint before = open_fd_count();
    for (guint i = 0; i < 32; i++) {
        TcResourceLease *lease = NULL;
        atomic_store(&query_failure, true);
        g_assert_cmpint(tc_governor_acquire(governor, &small, dir_a, NULL, 0, &lease, NULL), ==, TC_STATUS_ERROR);
        atomic_store(&query_failure, false);
        g_assert_cmpint(tc_governor_acquire(governor, &small, dir_a, NULL, 0, &lease, NULL), ==, TC_STATUS_OK);
        tc_governor_release(&lease);
    }
    g_assert_cmpuint(open_fd_count(), ==, before);
    assert_empty(governor);
    tc_governor_free(governor);
    teardown();
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/governor/accounting-release", test_accounting);
    g_test_add_func("/governor/disk-per-filesystem", test_disk_isolation);
    g_test_add_func("/governor/output-headroom", test_output_headroom);
    g_test_add_func("/governor/rejections-no-leases", test_rejections);
    g_test_add_data_func("/governor/wait-release", "release", test_wait_paths);
    g_test_add_data_func("/governor/wait-cpu", "cpu", test_wait_paths);
    g_test_add_data_func("/governor/wait-disk", "disk", test_wait_paths);
    g_test_add_data_func("/governor/wait-media", "media", test_wait_paths);
    g_test_add_data_func("/governor/wait-cancel", "cancel", test_wait_paths);
    g_test_add_data_func("/governor/wait-timeout", "timeout", test_wait_paths);
    g_test_add_func("/governor/concurrent-media", test_concurrent_media);
    g_test_add_func("/governor/disk-math-fail-closed", test_disk_math);
    g_test_add_func("/governor/fd-cleanup", test_fd_cleanup);
    return g_test_run();
}
