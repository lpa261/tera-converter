#include "tc_governor.h"
#include "tc_disk_guard.h"
#include "tc_runtime_limits.h"
#include "tc_logging.h"
#include <malloc.h>
#include <pthread.h>

/* POSIX locks make the admission handoff visible to race detectors even when
 * the distribution's GLib synchronization implementation is not instrumented. */
struct TcGovernor {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    TcGovernorLimits limits;
    TcGovernorSnapshot used;
    GQueue waiters;
    GHashTable *volumes;
};

typedef struct {
    uint64_t bytes;
    guint leases;
} VolumeReservation;

struct TcResourceLease {
    TcGovernor *governor; /* Borrowed; owner must join jobs before freeing it. */
    TcResourceRequest request;
    uint64_t device, disk_charge;
    int directory_fd;
};

static void lock_governor(TcGovernor *governor) {
    int status = pthread_mutex_lock(&governor->mutex);
    if (status) g_error("Governor lock failed: %s", g_strerror(status));
}

static void unlock_governor(TcGovernor *governor) {
    int status = pthread_mutex_unlock(&governor->mutex);
    if (status) g_error("Governor unlock failed: %s", g_strerror(status));
}

static void wake_waiters(TcGovernor *governor) {
    int status = pthread_cond_broadcast(&governor->changed);
    if (status) g_error("Governor wake failed: %s", g_strerror(status));
}

TcGovernor *tc_governor_new(const TcGovernorLimits *limits) {
    if (!limits || !limits->memory_bytes || !limits->cpu_units || limits->cpu_units > 1024 ||
        !limits->media_jobs || limits->media_jobs > limits->cpu_units) return NULL;
    TcGovernor *governor = g_new0(TcGovernor, 1);
    pthread_condattr_t attributes;
    if (pthread_mutex_init(&governor->mutex, NULL) != 0) { g_free(governor); return NULL; }
    if (pthread_condattr_init(&attributes) != 0) {
        pthread_mutex_destroy(&governor->mutex); g_free(governor); return NULL;
    }
    int status = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
    if (!status) status = pthread_cond_init(&governor->changed, &attributes);
    pthread_condattr_destroy(&attributes);
    if (status) { pthread_mutex_destroy(&governor->mutex); g_free(governor); return NULL; }
    governor->limits = *limits;
    governor->volumes = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
    return governor;
}

void tc_governor_free(TcGovernor *governor) {
    if (!governor) return;
    g_assert_cmpuint(governor->used.active_leases, ==, 0);
    g_assert_true(g_queue_is_empty(&governor->waiters));
    g_hash_table_unref(governor->volumes);
    pthread_cond_destroy(&governor->changed);
    pthread_mutex_destroy(&governor->mutex);
    g_free(governor);
}

static pthread_once_t default_once = PTHREAD_ONCE_INIT;
static TcGovernor *default_governor;

static void initialize_default(void) {
    const TcGovernorLimits limits = { .memory_bytes = 1024ULL * 1024 * 1024,
        .cpu_units = tc_worker_thread_limit(), .media_jobs = 1,
        .emergency_disk_bytes = TC_MIN_FREE_DISK_BYTES };
    default_governor = tc_governor_new(&limits);
    if (!default_governor) g_error("Cannot initialize resource governor");
}

TcGovernor *tc_governor_default(void) {
    int status = pthread_once(&default_once, initialize_default);
    if (status) g_error("Cannot initialize resource governor: %s", g_strerror(status));
    return default_governor;
}

void tc_governor_init(void) { (void)tc_governor_default(); }

static TcStatusCode reject(char **error_out, TcStatusCode status, const char *message) {
    if (error_out) *error_out = g_strdup(message);
    return status;
}

/* All additions use remaining capacity, so neither requests nor counters wrap. */
static bool disk_fits(uint64_t available, uint64_t emergency, uint64_t reserved, uint64_t charge) {
    return available >= emergency && reserved <= available - emergency &&
           charge <= available - emergency - reserved;
}

TcStatusCode tc_governor_acquire(TcGovernor *governor, const TcResourceRequest *request,
                                 const char *output_dir, GCancellable *cancellable,
                                 gint64 timeout_ms, TcResourceLease **lease_out, char **error_out) {
    if (lease_out) *lease_out = NULL;
    if (!governor || !request || !output_dir || !lease_out || timeout_ms < 0 ||
        timeout_ms > G_MAXINT64 / 1000 || !request->memory_bytes || !request->cpu_units)
        return reject(error_out, TC_STATUS_ERROR, "Invalid resource request or admission deadline");
    if (cancellable && g_cancellable_is_cancelled(cancellable))
        return reject(error_out, TC_STATUS_ERROR_CANCELLED, "Job cancelled before admission");
    if (request->memory_bytes > governor->limits.memory_bytes)
        return reject(error_out, TC_STATUS_ERROR_MEM_LIMIT, "Job memory estimate exceeds the admission budget");
    if (request->cpu_units > governor->limits.cpu_units)
        return reject(error_out, TC_STATUS_ERROR, "Job CPU request exceeds the admission budget");
    uint64_t charge;
    if (!tc_disk_guard_charge(request->output_bytes, &charge) ||
        charge > UINT64_MAX - governor->limits.emergency_disk_bytes)
        return reject(error_out, TC_STATUS_ERROR_NO_SPACE, "Output space estimate exceeds supported size");
    int directory_fd = open(output_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_fd < 0) {
        if (error_out) *error_out = g_strdup_printf("Cannot open output directory: %s", g_strerror(errno));
        return TC_STATUS_ERROR;
    }
    TcStatusCode outcome = TC_STATUS_OK;
    gint64 started = g_get_monotonic_time();
    int waiter_identity; /* Address is unique while this call is in the queue. */
    lock_governor(governor);
    g_queue_push_tail(&governor->waiters, &waiter_identity);
    for (;;) {
        if (cancellable && g_cancellable_is_cancelled(cancellable)) {
            outcome = reject(error_out, TC_STATUS_ERROR_CANCELLED, "Job cancelled while waiting for resources");
            break;
        }
        if (g_queue_peek_head(&governor->waiters) == &waiter_identity) {
            TcDiskSpace space;
            if (!tc_disk_guard_query_fd(directory_fd, &space, error_out)) { outcome = TC_STATUS_ERROR; break; }
            VolumeReservation *volume = g_hash_table_lookup(governor->volumes, &space.device);
            uint64_t reserved = volume ? volume->bytes : 0;
            if (!disk_fits(space.available_bytes, governor->limits.emergency_disk_bytes, 0, charge) && !volume) {
                outcome = reject(error_out, TC_STATUS_ERROR_NO_SPACE, "Insufficient space for this job and the emergency buffer");
                break;
            }
            bool memory_available = request->memory_bytes <= governor->limits.memory_bytes - governor->used.memory_bytes;
            bool cpu_available = request->cpu_units <= governor->limits.cpu_units - governor->used.cpu_units;
            bool media_available = !request->media_job || governor->used.media_jobs < governor->limits.media_jobs;
            if (memory_available && cpu_available && media_available &&
                disk_fits(space.available_bytes, governor->limits.emergency_disk_bytes, reserved, charge) &&
                (timeout_ms == 0 || g_get_monotonic_time() - started < timeout_ms * 1000)) {
                if (!volume) {
                    uint64_t *key = g_new(uint64_t, 1); *key = space.device;
                    volume = g_new0(VolumeReservation, 1);
                    g_hash_table_insert(governor->volumes, key, volume);
                }
                TcResourceLease *lease = g_new0(TcResourceLease, 1);
                lease->governor = governor;
                lease->request = *request;
                lease->device = space.device;
                lease->disk_charge = charge;
                lease->directory_fd = directory_fd;
                volume->bytes += charge;
                volume->leases++;
                governor->used.memory_bytes += request->memory_bytes;
                governor->used.cpu_units += request->cpu_units;
                governor->used.media_jobs += request->media_job;
                governor->used.active_leases++;
                *lease_out = lease;
                break;
            }
        }
        gint64 elapsed = g_get_monotonic_time() - started;
        if (elapsed >= timeout_ms * 1000) {
            outcome = reject(error_out, TC_STATUS_ERROR_TIMEOUT, "Timed out waiting for resource admission");
            break;
        }
        struct timespec tick;
        if (clock_gettime(CLOCK_MONOTONIC, &tick) != 0) {
            outcome = reject(error_out, TC_STATUS_ERROR, "Cannot read admission clock"); break;
        }
        gint64 wait_us = MIN((gint64)50000, timeout_ms * 1000 - elapsed);
        tick.tv_nsec += wait_us * 1000;
        if (tick.tv_nsec >= 1000000000) { tick.tv_sec++; tick.tv_nsec -= 1000000000; }
        int status = pthread_cond_timedwait(&governor->changed, &governor->mutex, &tick);
        if (status != 0 && status != ETIMEDOUT) {
            outcome = reject(error_out, TC_STATUS_ERROR, "Resource admission wait failed"); break;
        }
    }
    g_queue_remove(&governor->waiters, &waiter_identity);
    wake_waiters(governor);
    unlock_governor(governor);
    if (!*lease_out) close(directory_fd);
    return outcome;
}

void tc_governor_release(TcResourceLease **lease_pointer) {
    if (!lease_pointer || !*lease_pointer) return;
    TcResourceLease *lease = *lease_pointer;
    *lease_pointer = NULL;
    TcGovernor *governor = lease->governor;
    lock_governor(governor);
    VolumeReservation *volume = g_hash_table_lookup(governor->volumes, &lease->device);
    g_assert_nonnull(volume);
    g_assert_cmpuint(volume->bytes, >=, lease->disk_charge);
    volume->bytes -= lease->disk_charge;
    if (--volume->leases == 0) g_hash_table_remove(governor->volumes, &lease->device);
    governor->used.memory_bytes -= lease->request.memory_bytes;
    governor->used.cpu_units -= lease->request.cpu_units;
    governor->used.media_jobs -= lease->request.media_job;
    governor->used.active_leases--;
    wake_waiters(governor);
    unlock_governor(governor);
    close(lease->directory_fd);
    g_free(lease);
}

void tc_governor_snapshot(TcGovernor *governor, TcGovernorSnapshot *snapshot) {
    if (!governor || !snapshot) return;
    lock_governor(governor);
    *snapshot = governor->used;
    snapshot->waiting_jobs = g_queue_get_length(&governor->waiters);
    snapshot->filesystems = g_hash_table_size(governor->volumes);
    GHashTableIter iterator;
    gpointer value;
    g_hash_table_iter_init(&iterator, governor->volumes);
    while (g_hash_table_iter_next(&iterator, NULL, &value)) {
        uint64_t bytes = ((VolumeReservation *)value)->bytes;
        snapshot->disk_bytes = bytes > UINT64_MAX - snapshot->disk_bytes ? UINT64_MAX : snapshot->disk_bytes + bytes;
    }
    unlock_governor(governor);
}

TcStatusCode tc_governor_check_output(TcResourceLease *lease, uint64_t output_device, char **error_out) {
    if (!lease) return reject(error_out, TC_STATUS_ERROR, "Missing output resource lease");
    if (output_device != lease->device)
        return reject(error_out, TC_STATUS_ERROR, "Candidate is on a different output filesystem");
    TcGovernor *governor = lease->governor;
    lock_governor(governor);
    TcDiskSpace space;
    TcStatusCode status = TC_STATUS_OK;
    if (!tc_disk_guard_query_fd(lease->directory_fd, &space, error_out)) status = TC_STATUS_ERROR;
    else if (space.device != lease->device) status = reject(error_out, TC_STATUS_ERROR, "Output filesystem changed");
    else {
        VolumeReservation *volume = g_hash_table_lookup(governor->volumes, &lease->device);
        g_assert_nonnull(volume);
        if (!disk_fits(space.available_bytes, governor->limits.emergency_disk_bytes, volume->bytes - lease->disk_charge, 0))
            status = reject(error_out, TC_STATUS_ERROR_NO_SPACE, "Output filesystem no longer has reserved headroom");
    }
    unlock_governor(governor);
    return status;
}

void tc_governor_on_job_completed(uint32_t active_jobs_count) {
    if (active_jobs_count == 0) {
#if defined(__GLIBC__)
        malloc_trim(0);
#endif
    }
}
