#ifndef TC_GOVERNOR_H
#define TC_GOVERNOR_H

#include "tc_common.h"

typedef struct TcGovernor TcGovernor;
typedef struct TcResourceLease TcResourceLease;

typedef struct {
    uint64_t memory_bytes;
    guint cpu_units;
    guint media_jobs;
    uint64_t emergency_disk_bytes;
} TcGovernorLimits;

typedef struct {
    uint64_t memory_bytes;
    uint64_t output_bytes; /* Estimate; disk accounting adds 20%. */
    uint64_t input_bytes;
    guint cpu_units;
    bool media_job;
} TcResourceRequest;

typedef struct {
    uint64_t memory_bytes, disk_bytes;
    guint cpu_units, media_jobs, active_leases, waiting_jobs, filesystems;
} TcGovernorSnapshot;

/* The governor must outlive waiting callers and leases. Limits are immutable.
 * Accounting is admission, not an OS memory/CPU/filesystem quota. */
TcGovernor *tc_governor_new(const TcGovernorLimits *limits);
void tc_governor_free(TcGovernor *governor);
TcGovernor *tc_governor_default(void); /* Borrowed process-lifetime singleton. */
void tc_governor_init(void);          /* Idempotent, also used by tests. */

/* FIFO admission reserves all resources atomically, before engine/probe/temp
 * creation. timeout_ms=0 is a nonblocking attempt; NULL cancellable is allowed.
 * Disk reservations are grouped by the output directory's device. */
TcStatusCode tc_governor_acquire(TcGovernor *governor, const TcResourceRequest *request,
                                 const char *output_dir, GCancellable *cancellable,
                                 gint64 timeout_ms, TcResourceLease **lease_out,
                                 char **error_out);
/* Consumes the lease and clears its pointer; releasing NULL is harmless. */
void tc_governor_release(TcResourceLease **lease);
void tc_governor_snapshot(TcGovernor *governor, TcGovernorSnapshot *snapshot);
/* Recheck emergency headroom and OTHER jobs' reservations before publish.
 * The completed candidate's own disk use is already reflected by statvfs. */
TcStatusCode tc_governor_check_output(TcResourceLease *lease, uint64_t output_device, char **error_out);

void tc_governor_on_job_completed(uint32_t active_jobs_count);

#endif /* TC_GOVERNOR_H */
