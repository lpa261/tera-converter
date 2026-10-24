#ifndef TC_WORKER_POOL_H
#define TC_WORKER_POOL_H

#include "tc_common.h"
#include "tc_engine.h"
#include "tc_compression.h"
#include <pthread.h>

typedef struct _TcBatchContext TcBatchContext;

typedef struct {
    char *src_path;
    GPtrArray *merge_paths;     /* Non-NULL for TC_ACTION_MERGE */
    char *target_format;
    TcActionType action;
    TcOverwritePolicy overwrite_policy;
    uint16_t target_width;
    uint16_t target_height;
    TcBatchContext *batch;
} TcJobItem;

struct _TcBatchContext {
    gint ref_count;
    pthread_mutex_t mutex;
    pthread_mutex_t io_mutex;
    uint64_t batch_id;
    uint16_t protocol_version;
    bool admitted;
    uint32_t jobs_to_release; /* listener_mutex: includes jobs finishing telemetry */
    int ui_listener_fd;
    TcCompressionPolicy policy;
    TcCompressionPhase phase;
    TcCompressionResult result;
    uint32_t saved_jobs, no_benefit_jobs, unsupported_jobs, cancelled_jobs, existing_jobs;
    uint64_t input_bytes, output_bytes, saved_bytes;
    char current_filename[128];
    float last_progress;
    GCancellable *cancellable;
    uint32_t total_jobs;
    uint32_t completed_jobs;
    uint32_t failed_jobs;
    uint32_t skipped_jobs;
    GPtrArray *error_reports;   /* strings: "filename: error description" */
    int64_t start_time_us;
    int client_notify_fd;       /* socket to stream progress events back, or -1 */
};

void tc_worker_pool_init(void);
TcBatchContext *tc_batch_context_new(uint32_t total_jobs, int client_notify_fd);
/* new() returns an owned reference; submit_job() retains its own reference. */
TcBatchContext *tc_batch_context_ref(TcBatchContext *batch);
void tc_batch_context_free(TcBatchContext *batch);
void tc_worker_pool_submit_job(TcJobItem *job);
void tc_worker_pool_register_ui_listener(int fd);
void tc_worker_pool_register_batch_listener(uint64_t batch_id, int fd);
bool tc_worker_pool_begin_batch(TcBatchContext *batch, uint64_t batch_id);
void tc_worker_pool_abort_batch(TcBatchContext *batch); /* Before any submit. */
void tc_worker_pool_cancel_all(void);
bool tc_worker_pool_cancel_batch(uint64_t batch_id);
void tc_worker_pool_cancel_active(void);
void tc_worker_pool_shutdown(void);

#endif /* TC_WORKER_POOL_H */
