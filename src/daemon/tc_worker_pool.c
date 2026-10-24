#define _GNU_SOURCE
#include "tc_worker_pool.h"
#include "tc_governor.h"
#include "tc_runtime_limits.h"
#include "tc_logging.h"
#include "tc_process_runner.h"
#include "tc_temp_job.h"
#include "tc_ipc.h"

#ifdef HAVE_LIBNOTIFY
#include <libnotify/notify.h>
#endif

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    GQueue queue;
    pthread_t threads[4];
    unsigned count;
    bool stopping;
} TcScheduler;
static TcScheduler worker_pool = {.mutex=PTHREAD_MUTEX_INITIALIZER, .ready=PTHREAD_COND_INITIALIZER, .queue=G_QUEUE_INIT};
static TcScheduler media_pool = {.mutex=PTHREAD_MUTEX_INITIALIZER, .ready=PTHREAD_COND_INITIALIZER, .queue=G_QUEUE_INIT};
static gint active_jobs_count;
static pthread_mutex_t listener_mutex = PTHREAD_MUTEX_INITIALIZER;
static GHashTable *batches;
static uint64_t next_batch_id = 1;
/* Caller holds listener_mutex. Legacy controls are allowed only for one batch. */
static TcBatchContext *sole_batch(void) {
    if (!batches || g_hash_table_size(batches) != 1) return NULL;
    GHashTableIter it; gpointer value = NULL;
    g_hash_table_iter_init(&it, batches);
    return g_hash_table_iter_next(&it, NULL, &value) ? value : NULL;
}
static int global_ui_listener_fd = -1;
typedef struct {
    uint64_t id;
    unsigned char v1[sizeof(TcIpcHeader)+sizeof(TcIpcProgressPayload)];
    unsigned char v2[sizeof(TcIpcHeader)+sizeof(TcIpcProgressV2)];
} TcCompletion;
static TcCompletion completions[32];
static unsigned completion_cursor,reserved_jobs;
static void send_frame(int *fd, const void *frame, size_t size);
static void send_progress_event(TcBatchContext *batch, const char *filename);
static void worker_func(gpointer data, gpointer unused);

bool tc_worker_pool_begin_batch(TcBatchContext *batch, uint64_t id) {
    if(!batch || !batch->total_jobs || batch->total_jobs>TC_IPC_MAX_FILES)return false;
    pthread_mutex_lock(&listener_mutex);
    if (!batches) batches=g_hash_table_new_full(g_int64_hash,g_int64_equal,g_free,(GDestroyNotify)tc_batch_context_free);
    if (!id) id=batch->batch_id;
    TcBatchContext *existing=g_hash_table_lookup(batches,&id);
    bool ok=existing==batch;
    if (!existing && g_hash_table_size(batches)<64 && batch->total_jobs<=TC_IPC_MAX_FILES-reserved_jobs) {
        uint64_t *key=g_new(uint64_t,1); *key=id;
        batch->batch_id=id;batch->admitted=true;batch->jobs_to_release=batch->total_jobs;reserved_jobs+=batch->total_jobs;
        g_hash_table_insert(batches,key,tc_batch_context_ref(batch));
        ok=true;
    }
    pthread_mutex_unlock(&listener_mutex);
    return ok;
}

void tc_worker_pool_abort_batch(TcBatchContext *batch){
    pthread_mutex_lock(&listener_mutex);
    if(batch->admitted && batches && g_hash_table_lookup(batches,&batch->batch_id)==batch){
        reserved_jobs-=batch->jobs_to_release;batch->jobs_to_release=0;batch->admitted=false;g_hash_table_remove(batches,&batch->batch_id);
    }
    pthread_mutex_unlock(&listener_mutex);
}
void tc_worker_pool_cancel_all(void){
    GArray *ids=g_array_new(FALSE,FALSE,sizeof(uint64_t));
    pthread_mutex_lock(&listener_mutex);
    if(batches){GHashTableIter it;gpointer key;g_hash_table_iter_init(&it,batches);while(g_hash_table_iter_next(&it,&key,NULL)){uint64_t id=*(uint64_t*)key;g_array_append_val(ids,id);}}
    pthread_mutex_unlock(&listener_mutex);
    for(guint i=0;i<ids->len;i++)tc_worker_pool_cancel_batch(g_array_index(ids,uint64_t,i));
    g_array_unref(ids);
}

void tc_worker_pool_register_batch_listener(uint64_t id,int fd) {
    pthread_mutex_lock(&listener_mutex);
    TcBatchContext *batch=tc_batch_context_ref(batches?g_hash_table_lookup(batches,&id):NULL);
    if (!batch) {
        for (unsigned i=0;i<G_N_ELEMENTS(completions);i++) if (completions[i].id==id && id) {
            send_frame(&fd,completions[i].v2,sizeof(completions[i].v2));
            break;
        }
    }
    pthread_mutex_unlock(&listener_mutex);
    if (!batch) {if(fd>=0)close(fd); return;}
    pthread_mutex_lock(&batch->io_mutex);
    if(batch->ui_listener_fd>=0)close(batch->ui_listener_fd);
    batch->ui_listener_fd=fd;
    pthread_mutex_unlock(&batch->io_mutex);
    send_progress_event(batch,NULL);
    tc_batch_context_free(batch);
}

void tc_worker_pool_register_ui_listener(int fd) {
    pthread_mutex_lock(&listener_mutex);
    if(global_ui_listener_fd>=0)close(global_ui_listener_fd);
    global_ui_listener_fd=fd;
    TcBatchContext *batch=NULL;
    if(batches && g_hash_table_size(batches)==1)batch=tc_batch_context_ref(sole_batch());
    else if(!batches || !g_hash_table_size(batches)) {
        if(completion_cursor) {
            TcCompletion *c=&completions[(completion_cursor-1)%G_N_ELEMENTS(completions)];
            send_frame(&global_ui_listener_fd,c->v1,sizeof(c->v1));
        }
    } else {close(global_ui_listener_fd);global_ui_listener_fd=-1;}
    pthread_mutex_unlock(&listener_mutex);
    if(batch){send_progress_event(batch,NULL);tc_batch_context_free(batch);}
}

static void cancel_queued(TcScheduler *pool,TcBatchContext *batch) {
    GQueue removed=G_QUEUE_INIT;
    pthread_mutex_lock(&pool->mutex);
    for(GList *node=pool->queue.head,*next;node;node=next) {
        next=node->next;
        TcJobItem *job=node->data;
        if(job->batch==batch){g_queue_delete_link(&pool->queue,node);g_queue_push_tail(&removed,job);}
    }
    pthread_mutex_unlock(&pool->mutex);
    while(!g_queue_is_empty(&removed))worker_func(g_queue_pop_head(&removed),NULL);
}

bool tc_worker_pool_cancel_batch(uint64_t id) {
    pthread_mutex_lock(&listener_mutex);
    TcBatchContext *batch=tc_batch_context_ref(batches?g_hash_table_lookup(batches,&id):NULL);
    pthread_mutex_unlock(&listener_mutex);
    if(!batch)return false;
    pthread_mutex_lock(&batch->mutex);g_cancellable_cancel(batch->cancellable);pthread_mutex_unlock(&batch->mutex);
    cancel_queued(&media_pool,batch);
    cancel_queued(&worker_pool,batch);
    tc_batch_context_free(batch);
    return true;
}
void tc_worker_pool_cancel_active(void) {
    pthread_mutex_lock(&listener_mutex);
    TcBatchContext *only=sole_batch();
    uint64_t id=only?only->batch_id:0;
    pthread_mutex_unlock(&listener_mutex);
    if(id)tc_worker_pool_cancel_batch(id);
}

static bool destination_exists(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0 || errno != ENOENT;
}

/* Helper to resolve target output path and auto-numbering */
static char *resolve_destination_path(
    const char *src_path, TcActionType action, const char *target_format,
    uint16_t width, uint16_t height, TcOverwritePolicy policy
) {
    g_autofree char *dir = g_path_get_dirname(src_path);
    g_autofree char *base = g_path_get_basename(src_path);
    char *dot = strrchr(base, '.');
    const char *original_ext = dot && dot != base && dot[1] ? dot + 1 : "png";
    if (dot && dot != base) *dot = '\0';
    const char *ext = !strcmp(target_format,"safe-flac")?"flac":target_format;
    g_autofree char *stem = NULL;
    if (action == TC_ACTION_REMOVE_BG || !g_ascii_strcasecmp(target_format, "rmbg")) {
        stem = g_strdup_printf("%s-nobg", base);
        ext = "png";
    } else if (action == TC_ACTION_RESIZE && width && height) {
        stem = g_strdup_printf("%s_%dx%d", base, width, height);
        if (!*target_format || !g_ascii_strcasecmp(target_format, "resize")) ext = original_ext;
    } else if (action == TC_ACTION_MERGE) {
        stem = g_strdup_printf("%s_merged", base);
    } else if (action == TC_ACTION_COMPRESS) {
        stem = g_strdup_printf("%s_compressed", base);
        ext = g_str_has_prefix(target_format, "comp-web") ||
              g_str_has_prefix(target_format, "comp-saver") ||
              g_str_has_prefix(target_format, "comp-hq") ? "mp4" : original_ext;
    } else {
        stem = g_strdup(base);
    }
    for (unsigned i = 0; i < 10000; i++) {
        char *candidate = i ? g_strdup_printf("%s/%s (%u).%s", dir, stem, i, ext) :
                              g_strdup_printf("%s/%s.%s", dir, stem, ext);
        if (policy != TC_OVERWRITE_AUTO_NUMBER || !destination_exists(candidate)) return candidate;
        g_free(candidate);
    }
    return NULL;
}

TcBatchContext *tc_batch_context_new(uint32_t total_jobs, int client_notify_fd) {
    TcBatchContext *ctx = g_new0(TcBatchContext, 1);
    ctx->ref_count = 1;
    pthread_mutex_lock(&listener_mutex);
    ctx->batch_id=next_batch_id++;
    pthread_mutex_unlock(&listener_mutex);
    ctx->protocol_version=1;
    ctx->ui_listener_fd=-1;
    ctx->policy=(TcCompressionPolicy){.min_saved_bytes=1};
    pthread_mutex_init(&ctx->mutex,NULL);
    pthread_mutex_init(&ctx->io_mutex,NULL);
    ctx->cancellable = g_cancellable_new();
    ctx->total_jobs = total_jobs;
    ctx->error_reports = g_ptr_array_new_with_free_func(g_free);
    ctx->start_time_us = g_get_monotonic_time();
    ctx->client_notify_fd = client_notify_fd;
    return ctx;
}

TcBatchContext *tc_batch_context_ref(TcBatchContext *ctx) {
    if (ctx) g_atomic_int_inc(&ctx->ref_count);
    return ctx;
}

void tc_batch_context_free(TcBatchContext *ctx) {
    if (!ctx || !g_atomic_int_dec_and_test(&ctx->ref_count)) return;
    g_clear_object(&ctx->cancellable);
    g_ptr_array_free(ctx->error_reports, TRUE);
    if (ctx->client_notify_fd >= 0) close(ctx->client_notify_fd);
    if (ctx->ui_listener_fd >= 0) close(ctx->ui_listener_fd);
    pthread_mutex_destroy(&ctx->io_mutex);
    pthread_mutex_destroy(&ctx->mutex);
    g_free(ctx);
}

/* One send per complete frame. A stalled/partial-write listener is detached,
 * rather than blocking conversion threads or emitting a truncated next frame. */
static void send_frame(int *fd, const void *frame, size_t size) {
    if (*fd < 0) return;
    ssize_t sent;
    do { sent = tc_ipc_send_bytes(*fd, frame, size, MSG_NOSIGNAL | MSG_DONTWAIT); }
    while (sent < 0 && errno == EINTR);
    if (sent != (ssize_t)size) {
        close(*fd);
        *fd = -1;
    }
}

static void send_progress_event_detailed(
    TcBatchContext *batch,
    const char *current_filename,
    float intra_fraction,
    uint32_t intra_eta_sec
) {
    if (!batch) return;

    struct __attribute__((packed)) {
        TcIpcHeader header;
        TcIpcProgressPayload payload;
    } frame = {0};
    /* Serialize generation and writes, keeping snapshots in publication order. */
    pthread_mutex_lock(&batch->io_mutex);
    pthread_mutex_lock(&batch->mutex);
    int64_t elapsed_us = g_get_monotonic_time() - batch->start_time_us;
    double elapsed = MAX(0.0, elapsed_us / 1000000.0);
    guint total = MAX(1, batch->total_jobs);
    float fraction = CLAMP(((float)batch->completed_jobs + CLAMP(intra_fraction, 0.0f, 1.0f)) / total, 0.0f, 1.0f);
    batch->last_progress = MAX(batch->last_progress, fraction);
    if (current_filename) tc_utf8_copy(batch->current_filename, sizeof(batch->current_filename), current_filename);
    frame.header.magic = TC_IPC_MAGIC;
    frame.header.version = 1;
    frame.header.msg_type = TC_IPC_MSG_PROGRESS_EVENT;
    frame.header.total_files = batch->total_jobs;
    frame.header.payload_size = sizeof(frame.payload);
    frame.payload.completed_files = batch->completed_jobs;
    frame.payload.total_files = batch->total_jobs;
    frame.payload.failed_files = batch->failed_jobs;
    frame.payload.elapsed_seconds = (uint32_t)elapsed;
    frame.payload.progress_fraction = batch->last_progress;
    frame.payload.items_per_sec = elapsed > 0.1 ? batch->completed_jobs / elapsed : 0;
    frame.payload.eta_seconds = batch->total_jobs <= 1 ? intra_eta_sec :
        (batch->last_progress > 0.02f ? (uint32_t)MAX(0.0, elapsed / batch->last_progress - elapsed) : 0);
    tc_utf8_copy(frame.payload.current_filename, sizeof(frame.payload.current_filename), batch->current_filename);
    struct __attribute__((packed)) {TcIpcHeader header; TcIpcProgressV2 payload;} v2={0};
    v2.header=frame.header;v2.header.version=2;v2.header.payload_size=sizeof(v2.payload);
    v2.payload.progress=frame.payload;v2.payload.batch_id=batch->batch_id;
    v2.payload.mode=batch->policy.mode;v2.payload.phase=batch->phase;v2.payload.status=batch->result.status;
    v2.payload.saved_files=batch->saved_jobs;v2.payload.no_benefit_files=batch->no_benefit_jobs;
    v2.payload.unsupported_files=batch->unsupported_jobs;v2.payload.cancelled_files=batch->cancelled_jobs;
    v2.payload.existing_files=batch->existing_jobs;
    v2.payload.input_bytes=batch->input_bytes;v2.payload.output_bytes=batch->output_bytes;
    v2.payload.saved_bytes=batch->saved_bytes;
    tc_utf8_copy(v2.payload.reason,sizeof(v2.payload.reason),batch->result.reason);
    bool finished=batch->completed_jobs==batch->total_jobs;
    pthread_mutex_unlock(&batch->mutex);
    if(batch->protocol_version==2)send_frame(&batch->client_notify_fd,&v2,sizeof(v2));
    else send_frame(&batch->client_notify_fd,&frame,sizeof(frame));
    send_frame(&batch->ui_listener_fd,&v2,sizeof(v2));
    pthread_mutex_lock(&listener_mutex);
    if(finished){
        TcCompletion *cache=NULL;
        for(unsigned i=0;i<G_N_ELEMENTS(completions);i++)if(completions[i].id==batch->batch_id)cache=&completions[i];
        if(!cache)cache=&completions[completion_cursor++%G_N_ELEMENTS(completions)];
        cache->id=batch->batch_id;memcpy(cache->v1,&frame,sizeof(frame));memcpy(cache->v2,&v2,sizeof(v2));
    }
    if(sole_batch()==batch)
        send_frame(&global_ui_listener_fd,&frame,sizeof(frame));
    pthread_mutex_unlock(&listener_mutex);
    pthread_mutex_unlock(&batch->io_mutex);
}

static void send_progress_event(TcBatchContext *batch, const char *current_filename) {
    send_progress_event_detailed(batch, current_filename, 0.0f, 0);
}

typedef struct {
    TcBatchContext *batch;
    const char *filename;
    int64_t last_send_us;
} EngineProgressCtx;

static void on_engine_progress_cb(float fraction, uint32_t eta_sec, void *user_data) {
    EngineProgressCtx *ctx = (EngineProgressCtx *)user_data;
    if (!ctx || !ctx->batch) return;

    int64_t now = g_get_monotonic_time();
    /* Throttle to max 10 updates per second (100ms) unless finished */
    if (now - ctx->last_send_us < 100000 && fraction < 0.999f) {
        return;
    }
    ctx->last_send_us = now;

    send_progress_event_detailed(ctx->batch, ctx->filename, fraction, eta_sec);
}

static void emit_consolidated_notification(TcBatchContext *batch) {
    tc_log_info("Batch completed: %u/%u (failures: %u)",
                batch->completed_jobs, batch->total_jobs, batch->failed_jobs);
}

/* Registry selection is cheap; no metadata probing runs in the dispatcher. */
static TcEngine *find_job_engine(const TcJobItem *job, const char *final_dst, const char **engine_fmt) {
    TcEngine *eng = NULL;
    *engine_fmt = job->target_format;
    if (job->action == TC_ACTION_MERGE) return NULL;
    if (job->action == TC_ACTION_REMOVE_BG || g_ascii_strcasecmp(job->target_format, "rmbg") == 0) {
        eng = tc_engines_find_for_task(TC_CATEGORY_IMAGE, "rmbg");
        *engine_fmt = "rmbg";
    } else if (job->action == TC_ACTION_RESIZE) {
        const char *path = final_dst ? final_dst : job->src_path;
        const char *ext = path ? strrchr(path, '.') : NULL;
        if (ext && *(ext + 1) != '\0') {
            *engine_fmt = ext + 1;
        }
        eng = tc_engines_find_for_task(TC_CATEGORY_IMAGE, *engine_fmt);
    } else if (job->action == TC_ACTION_COMPRESS) {
        if (g_str_has_prefix(job->target_format, "comp-pdf") ||
            g_ascii_strcasecmp(job->target_format, "comp-office") == 0) {
            eng = tc_engines_find_for_task(TC_CATEGORY_DOCUMENT, job->target_format);
        } else {
            eng = tc_engines_find_for_task(TC_CATEGORY_MEDIA, job->target_format);
        }
        if (!eng) {
            eng = tc_engines_find_for_task(TC_CATEGORY_UNKNOWN, job->target_format);
        }
    } else {
        const char *src_dot = job->src_path ? strrchr(job->src_path, '.') : NULL;
        if (src_dot && g_ascii_strcasecmp(src_dot + 1, "pdf") == 0) {
            eng = tc_engines_find_for_task(TC_CATEGORY_DOCUMENT, job->target_format);
        }
        if (!eng) {
            eng = tc_engines_find_for_task(TC_CATEGORY_UNKNOWN, job->target_format);
        }
    }
    return eng;
}

static bool job_uses_media_pool(const TcJobItem *job) {
    if (job->action == TC_ACTION_MERGE || !g_ascii_strcasecmp(job->target_format, "comp-padding")) return false;
    const char *format;
    TcEngine *engine = find_job_engine(job, NULL, &format);
    return engine && engine->category == TC_CATEGORY_MEDIA;
}

static TcStatusCode estimate_job_resources(const TcJobItem *job, const TcEngine *engine,
                                            TcResourceRequest *request, char **error_out) {
    uint64_t input_bytes = 0;
    guint count = job->merge_paths && job->merge_paths->len ? job->merge_paths->len : 1;
    for (guint i = 0; i < count; i++) {
        const char *path = job->merge_paths && job->merge_paths->len ?
            g_ptr_array_index(job->merge_paths, i) : job->src_path;
        struct stat source;
        if (!path || stat(path, &source) != 0 || !S_ISREG(source.st_mode) || source.st_size < 0) {
            if (error_out) *error_out = g_strdup("Cannot estimate resources for a missing or nonregular source");
            return TC_STATUS_ERROR;
        }
        if ((uint64_t)source.st_size > UINT64_MAX - input_bytes) {
            if (error_out) *error_out = g_strdup("Input size estimate overflow");
            return TC_STATUS_ERROR_NO_SPACE;
        }
        input_bytes += (uint64_t)source.st_size;
    }
    *request = (TcResourceRequest){ .memory_bytes = 64ULL * 1024 * 1024, .cpu_units = 1 };
    request->media_job = job_uses_media_pool(job);
    if (request->media_job) {
        request->memory_bytes = 512ULL * 1024 * 1024;
        request->cpu_units = tc_engine_thread_limit();
    } else if (job->action == TC_ACTION_REMOVE_BG || !g_ascii_strcasecmp(job->target_format, "rmbg")) {
        request->memory_bytes = 1024ULL * 1024 * 1024;
    } else if (job->action == TC_ACTION_MERGE || (engine && engine->category == TC_CATEGORY_DOCUMENT)) {
        request->memory_bytes = 256ULL * 1024 * 1024;
    } else if (engine && engine->category == TC_CATEGORY_IMAGE) {
        request->memory_bytes = engine && !strcmp(engine->name,"image_preserve") ? 512ULL*1024*1024 : 128ULL*1024*1024;
    }
    request->input_bytes=input_bytes;
    if (job->action == TC_ACTION_COMPRESS) request->output_bytes = MAX(input_bytes,16ULL*1024*1024);
    else {
        const uint64_t floor_bytes = 16ULL * 1024 * 1024;
        if (input_bytes > (UINT64_MAX - floor_bytes) / 2) {
            if (error_out) *error_out = g_strdup("Output size estimate overflow");
            return TC_STATUS_ERROR_NO_SPACE;
        }
        request->output_bytes = input_bytes * 2 + floor_bytes;
    }
    return TC_STATUS_OK;
}

/* Worker pool thread routine */
static void worker_func(gpointer data, gpointer user_data) {
    (void)user_data;
    TcJobItem *job = (TcJobItem *)data;
    if (!job) return;

    TcBatchContext *batch = job->batch;
    TcStatusCode status = TC_STATUS_OK;
    char *err_msg = NULL;
    char *final_dst = NULL;
    char *temp_pattern = NULL;
    bool temp_owned = false;
    uint64_t source_bytes=0, output_bytes=0;
    struct stat *source_stats=NULL;
    guint source_count=job->merge_paths?job->merge_paths->len:1;
    TcResourceLease *lease = NULL;
    TcTempJob *workspace=NULL;
    TcEngine *eng = NULL;
    const char *engine_fmt = job->target_format;
    char *base_name = g_path_get_basename(job->src_path ? job->src_path : (job->merge_paths && job->merge_paths->len > 0 ? (const char *)g_ptr_array_index(job->merge_paths, 0) : "output"));

    if (g_cancellable_is_cancelled(batch->cancellable)) {
        tc_log_debug("Job skipped due to cancellation: %s", job->src_path);
        status = TC_STATUS_ERROR_CANCELLED;
        err_msg = g_strdup("Conversion cancelled");
        goto complete_job;
    }

    source_stats=g_new0(struct stat,source_count);
    for(guint i=0;i<source_count;i++){
        const char *path=job->merge_paths?g_ptr_array_index(job->merge_paths,i):job->src_path;
        if(!path || stat(path,&source_stats[i]) || !S_ISREG(source_stats[i].st_mode)){
            status=TC_STATUS_ERROR;err_msg=g_strdup("Source is missing or nonregular");goto complete_job;
        }
    }
    pthread_mutex_lock(&batch->mutex);batch->phase=TC_PHASE_WAITING;pthread_mutex_unlock(&batch->mutex);
    /* Send initial progress event to notify HUD that this file is now active */
    send_progress_event(batch, base_name);

    /* 1. Resolve destination and temp sibling */
    final_dst = resolve_destination_path(
        job->src_path,
        job->action,
        job->target_format,
        job->target_width,
        job->target_height,
        job->overwrite_policy
    );
    if (!final_dst) {
        status = TC_STATUS_ERROR;
        err_msg = g_strdup("No available output name");
        goto complete_job;
    }
    if (job->overwrite_policy == TC_OVERWRITE_SKIP && destination_exists(final_dst)) {
        status = TC_STATUS_SKIPPED;
        goto complete_job;
    }
    eng = find_job_engine(job, final_dst, &engine_fmt);
    if (job->action != TC_ACTION_MERGE && (!eng || !eng->convert)) {
        status = TC_STATUS_ERROR_UNSUPPORTED;
        err_msg = g_strdup_printf("No engine found to handle format: %s", engine_fmt);
        goto complete_job;
    }
    TcResourceRequest request;
    status = estimate_job_resources(job, eng, &request, &err_msg);
    if (status != TC_STATUS_OK) goto complete_job;
    char *output_dir = g_path_get_dirname(final_dst);
    status = tc_governor_acquire(tc_governor_default(), &request, output_dir,
        batch->cancellable, 5 * 60 * 1000, &lease, &err_msg);
    g_free(output_dir);
    if (status != TC_STATUS_OK) goto complete_job;
    source_bytes=request.input_bytes;
    TcEngine *estimator=eng;
    if(job->action==TC_ACTION_MERGE)estimator=tc_engines_find_for_task(TC_CATEGORY_IMAGE,job->target_format);
    if(estimator && estimator->estimate){
        pthread_mutex_lock(&batch->mutex);batch->phase=TC_PHASE_PROBING;pthread_mutex_unlock(&batch->mutex);
        send_progress_event(batch,base_name);
        TcResourceRequest refined=request;
        uint64_t merge_memory=0,merge_output=0;
        guint count=job->merge_paths?job->merge_paths->len:1;
        for(guint i=0;i<count;i++){
            const char *path=job->merge_paths?g_ptr_array_index(job->merge_paths,i):job->src_path;
            TcEngineEstimate estimate={0};
            status=estimator->estimate(path,engine_fmt,job->target_width,job->target_height,batch->cancellable,&estimate,&err_msg);
            if(status!=TC_STATUS_OK)goto complete_job;
            if(job->merge_paths){
                if(estimate.memory_bytes>UINT64_MAX-merge_memory||estimate.output_bytes>UINT64_MAX-merge_output){status=TC_STATUS_ERROR_MEM_LIMIT;goto complete_job;}
                merge_memory+=estimate.memory_bytes;merge_output+=estimate.output_bytes;
            }else{merge_memory=estimate.memory_bytes;merge_output=estimate.output_bytes;}
        }
        refined.memory_bytes=MAX(refined.memory_bytes,merge_memory);
        refined.output_bytes=MAX(refined.output_bytes,merge_output);
        if(refined.memory_bytes!=request.memory_bytes||refined.output_bytes!=request.output_bytes){
            tc_governor_release(&lease);
            pthread_mutex_lock(&batch->mutex);batch->phase=TC_PHASE_WAITING;pthread_mutex_unlock(&batch->mutex);
            send_progress_event(batch,base_name);
            char *directory=g_path_get_dirname(final_dst);
            status=tc_governor_acquire(tc_governor_default(),&refined,directory,batch->cancellable,5*60*1000,&lease,&err_msg);
            g_free(directory);if(status!=TC_STATUS_OK)goto complete_job;
            request=refined;
        }
    }
    if(!strcmp(job->target_format,"comp-pdf-safe")){
        /* Two bounded page-render images coexist during PDF validation. */
        tc_governor_release(&lease);request.memory_bytes=1024ULL*1024*1024;
        if(request.output_bytes<source_bytes+32ULL*1024*1024)request.output_bytes=source_bytes+32ULL*1024*1024;
        char *directory=g_path_get_dirname(final_dst);
        status=tc_governor_acquire(tc_governor_default(),&request,directory,batch->cancellable,5*60*1000,&lease,&err_msg);
        g_free(directory);if(status!=TC_STATUS_OK)goto complete_job;
    }

    workspace=tc_temp_job_new(final_dst,&err_msg);
    if(!workspace){status=TC_STATUS_ERROR;goto complete_job;}
    temp_pattern=g_strdup(tc_temp_job_path(workspace));
    int tmp_fd=open(temp_pattern,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if (tmp_fd < 0) {
        tc_log_error("Failed to create temporary file for %s: %s", final_dst, g_strerror(errno));
        status = TC_STATUS_ERROR;
        err_msg = g_strdup("Could not create temp file");
        goto complete_job;
    }
    temp_owned = true;
    struct stat temp_stat;
    if (fstat(tmp_fd, &temp_stat) != 0) {
        status = TC_STATUS_ERROR;
        err_msg = g_strdup("Cannot inspect candidate filesystem");
    } else {
        status = tc_governor_check_output(lease, (uint64_t)temp_stat.st_dev, &err_msg);
    }
    close(tmp_fd);
    if (status != TC_STATUS_OK) goto complete_job;

    uint64_t output_cap=request.output_bytes+request.output_bytes/5;
    tc_process_set_budget(temp_pattern,output_cap,request.memory_bytes*4);
    pthread_mutex_lock(&batch->mutex);batch->phase=TC_PHASE_RUNNING;pthread_mutex_unlock(&batch->mutex);
    send_progress_event(batch,base_name);
    if (job->action == TC_ACTION_MERGE) {
        if (g_ascii_strcasecmp(job->target_format, "pdf") == 0) {
            /* Try img2pdf first */
            GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
            g_ptr_array_add(argv, g_strdup("img2pdf"));
            g_ptr_array_add(argv, g_strdup("--output"));
            g_ptr_array_add(argv, g_strdup(temp_pattern));
            if (job->merge_paths && job->merge_paths->len > 0) {
                for (guint i = 0; i < job->merge_paths->len; i++) {
                    g_ptr_array_add(argv, g_strdup((const char *)g_ptr_array_index(job->merge_paths, i)));
                }
            } else if (job->src_path) {
                g_ptr_array_add(argv, g_strdup(job->src_path));
            }
            g_ptr_array_add(argv, NULL);

            int exit_status = 0;
            GError *err = NULL;
            gboolean spawned = tc_run_helper((const char *const *)argv->pdata, batch->cancellable, &err);
            g_ptr_array_free(argv, TRUE);

            if (!spawned || exit_status != 0) {
                /* Fallback to Python Pillow for multi-image merge */
                const char *py_fallback =
                    "import sys\n"
                    "from PIL import Image\n"
                    "dst = sys.argv[1]\n"
                    "imgs = sys.argv[2:]\n"
                    "if not imgs: sys.exit(1)\n"
                    "im_list = []\n"
                    "first = Image.open(imgs[0]).convert('RGB')\n"
                    "for p in imgs[1:]:\n"
                    "    im_list.append(Image.open(p).convert('RGB'))\n"
                    "first.save(dst, format='PDF', save_all=True, append_images=im_list)\n";

                GPtrArray *py_argv = g_ptr_array_new_with_free_func(g_free);
                g_ptr_array_add(py_argv, g_strdup("python3"));
                g_ptr_array_add(py_argv, g_strdup("-c"));
                g_ptr_array_add(py_argv, g_strdup(py_fallback));
                g_ptr_array_add(py_argv, g_strdup(temp_pattern));
                if (job->merge_paths && job->merge_paths->len > 0) {
                    for (guint i = 0; i < job->merge_paths->len; i++) {
                        g_ptr_array_add(py_argv, g_strdup((const char *)g_ptr_array_index(job->merge_paths, i)));
                    }
                } else if (job->src_path) {
                    g_ptr_array_add(py_argv, g_strdup(job->src_path));
                }
                g_ptr_array_add(py_argv, NULL);

                int py_exit = 0;
                GError *py_err = NULL;
                gboolean py_spawned = tc_run_helper((const char *const *)py_argv->pdata, batch->cancellable, &py_err);
                g_ptr_array_free(py_argv, TRUE);

                if (!py_spawned || py_exit != 0) {
                    status = TC_STATUS_ERROR;
                    err_msg = g_strdup(py_err ? py_err->message : (err ? err->message : "PDF merge failed"));
                    g_clear_error(&py_err);
                } else {
                    status = TC_STATUS_OK;
                }
                g_clear_error(&err);
            } else {
                status = TC_STATUS_OK;
            }
        } else {
            status = TC_STATUS_ERROR_UNSUPPORTED;
            err_msg = g_strdup("Unsupported merge target format");
        }
    } else {
        /* 2. Find and execute conversion engine */
        /* Engine selected before admission. */

        if (eng && eng->convert) {
            EngineProgressCtx prog_ctx = {
                .batch = batch,
                .filename = base_name,
                .last_send_us = 0
            };
            status = eng->convert(
                job->src_path,
                temp_pattern,
                engine_fmt,
                job->target_width,
                job->target_height,
                batch->cancellable,
                &err_msg,
                on_engine_progress_cb,
                &prog_ctx
            );
        } else {
            status = TC_STATUS_ERROR_UNSUPPORTED;
            err_msg = g_strdup_printf("No engine found to handle format: %s", engine_fmt);
            tc_log_error("%s", err_msg);
        }
    }

    pthread_mutex_lock(&batch->mutex);batch->phase=TC_PHASE_VALIDATING;pthread_mutex_unlock(&batch->mutex);
    send_progress_event(batch,base_name);
    /* 3. Two-phase atomic commit or rollback */
    struct stat in_st;
    bool in_stat_ok = (stat(job->src_path, &in_st) == 0);
    bool sources_unchanged=true;
    for(guint i=0;i<source_count;i++){
        struct stat now,*before=&source_stats[i];
        const char *path=job->merge_paths?g_ptr_array_index(job->merge_paths,i):job->src_path;
        if(stat(path,&now) || now.st_dev!=before->st_dev || now.st_ino!=before->st_ino ||
           now.st_size!=before->st_size || now.st_mtim.tv_sec!=before->st_mtim.tv_sec ||
           now.st_mtim.tv_nsec!=before->st_mtim.tv_nsec || now.st_ctim.tv_sec!=before->st_ctim.tv_sec ||
           now.st_ctim.tv_nsec!=before->st_ctim.tv_nsec){sources_unchanged=false;break;}
    }
    struct stat out_st;
    /* Serialize the final cancellation decision with publication. */
    pthread_mutex_lock(&batch->mutex);
    if (status == TC_STATUS_OK && !g_cancellable_is_cancelled(batch->cancellable)) {
        if (stat(temp_pattern, &out_st) != 0 || out_st.st_size == 0) {
            tc_log_error("Conversion output %s is missing or empty (0 bytes)", temp_pattern);
            status = TC_STATUS_ERROR;
            if (!err_msg) err_msg = g_strdup("Conversion produced empty output file (0 bytes)");
        } else if ((uint64_t)out_st.st_size>output_cap){
            status=TC_STATUS_ERROR_NO_SPACE;
            if(!err_msg)err_msg=g_strdup("Candidate exceeded the reserved disk budget");
        } else if ((status = tc_governor_check_output(lease, (uint64_t)out_st.st_dev, &err_msg)) != TC_STATUS_OK) {
            /* The candidate is discarded by common cleanup. */
        } else if (!sources_unchanged) {
            status=TC_STATUS_ERROR_CORRUPT;
            if(!err_msg)err_msg=g_strdup("Source changed during processing; candidate discarded");
        } else if (job->action == TC_ACTION_COMPRESS && in_stat_ok &&
                   !tc_compression_has_benefit(&batch->policy,(uint64_t)in_st.st_size,(uint64_t)out_st.st_size)) {
            tc_log_warn("Compressed output (%ld bytes) is larger than or equal to source (%ld bytes). Rejecting output.",
                        (long)out_st.st_size, (long)in_st.st_size);
            status = TC_STATUS_NO_BENEFIT;
            err_msg = g_strdup_printf(
                "Candidate did not meet the savings policy (%.1f MB output, %.1f MB input). Original preserved.",
                (double)out_st.st_size / (1024.0 * 1024.0),
                (double)in_st.st_size / (1024.0 * 1024.0)
            );
        } else if (job->overwrite_policy == TC_OVERWRITE_REPLACE) {
            batch->phase=TC_PHASE_COMMITTING;
            if (rename(temp_pattern, final_dst) != 0) {
                status = TC_STATUS_ERROR;
                err_msg = g_strdup_printf("Could not replace output: %s", g_strerror(errno));
            } else {
                temp_owned = false; /* rename consumed the temporary name. */
            }
        } else {
            batch->phase=TC_PHASE_COMMITTING;
            /* A hard link publishes this same-filesystem regular temp file without
             * overwriting any existing entry (including a dangling symlink). */
            for (unsigned attempt = 0; ; attempt++) {
                if (link(temp_pattern, final_dst) == 0) {
                    break;
                }
                if (errno == EEXIST && job->overwrite_policy == TC_OVERWRITE_SKIP) {
                    status = TC_STATUS_SKIPPED;
                    break;
                }
                if (errno != EEXIST || attempt >= 10000) {
                    status = TC_STATUS_ERROR;
                    err_msg = g_strdup_printf("Could not publish output safely: %s", g_strerror(errno));
                    break;
                }
                g_free(final_dst);
                final_dst = resolve_destination_path(job->src_path, job->action,
                    job->target_format, job->target_width, job->target_height, job->overwrite_policy);
                if (!final_dst) {
                    status = TC_STATUS_ERROR;
                    err_msg = g_strdup("No available output name");
                    break;
                }
                if (g_cancellable_is_cancelled(batch->cancellable)) {
                    status = TC_STATUS_ERROR_CANCELLED;
                    break;
                }
            }
        }
    } else {
        if (g_cancellable_is_cancelled(batch->cancellable)) status = TC_STATUS_ERROR_CANCELLED;
    }

    pthread_mutex_unlock(&batch->mutex);
    if(status==TC_STATUS_OK)send_progress_event(batch,base_name);
    if(status==TC_STATUS_OK && temp_pattern){
        struct stat done_stat;
        if(stat(temp_owned?temp_pattern:final_dst,&done_stat)==0 && done_stat.st_size>0)output_bytes=(uint64_t)done_stat.st_size;
    }
complete_job:
    tc_process_set_budget(NULL,0,0);
    if (temp_owned) unlink(temp_pattern);
    tc_temp_job_finish(&workspace);
    tc_governor_release(&lease);
    /* 4. Update batch telemetry, including early failures and cancellation. */
    pthread_mutex_lock(&batch->mutex);
    batch->completed_jobs++;
    batch->input_bytes=source_bytes>UINT64_MAX-batch->input_bytes?UINT64_MAX:batch->input_bytes+source_bytes;
    uint64_t published_bytes=status==TC_STATUS_OK?output_bytes:source_bytes;
    batch->output_bytes=published_bytes>UINT64_MAX-batch->output_bytes?UINT64_MAX:batch->output_bytes+published_bytes;
    batch->result.input_bytes=source_bytes;batch->result.output_bytes=output_bytes;
    batch->result.saved_bytes=status==TC_STATUS_OK&&source_bytes>output_bytes?source_bytes-output_bytes:0;
    batch->saved_bytes=batch->input_bytes>batch->output_bytes?batch->input_bytes-batch->output_bytes:0;
    batch->result.status=TC_RESULT_FAILED;
    if(status==TC_STATUS_OK){
        batch->result.status=TC_RESULT_SAVED;batch->saved_jobs++;

    }else if(status==TC_STATUS_ERROR_CANCELLED){
        batch->result.status=TC_RESULT_CANCELLED;batch->cancelled_jobs++;
    }else if(status==TC_STATUS_SKIPPED){
        batch->result.status=TC_RESULT_EXISTS;batch->existing_jobs++;batch->skipped_jobs++;
    }else if(status==TC_STATUS_NO_BENEFIT){
        batch->result.status=TC_RESULT_NO_BENEFIT;batch->no_benefit_jobs++;batch->skipped_jobs++;
    }else if(status==TC_STATUS_ERROR_UNSUPPORTED){
        batch->result.status=TC_RESULT_UNSUPPORTED_SAFE_MODE;batch->unsupported_jobs++;batch->skipped_jobs++;
    }else {
        batch->failed_jobs++;
        g_ptr_array_add(
            batch->error_reports,
            g_strdup_printf("%s: %s", job->src_path, err_msg ? err_msg : "Conversion failed")
        );
    }
    tc_utf8_copy(batch->result.reason,sizeof(batch->result.reason),err_msg?err_msg:
        status==TC_STATUS_NO_BENEFIT?"Candidate does not reduce size":
        status==TC_STATUS_SKIPPED?"Output already exists":
        status==TC_STATUS_ERROR_UNSUPPORTED?"Format or feature is unsupported by this mode":
        status==TC_STATUS_ERROR_CANCELLED?"Cancelled":
        status==TC_STATUS_OK?"Output validated and published":"Backend unavailable or conversion failed");
    g_free(err_msg);

    bool is_batch_finished = (batch->completed_jobs >= batch->total_jobs);
    if(is_batch_finished)batch->phase=TC_PHASE_DONE;
    pthread_mutex_unlock(&batch->mutex);
    send_progress_event(batch, base_name);

    if (is_batch_finished) {
        emit_consolidated_notification(batch);
    }


    gint remaining = g_atomic_int_dec_and_test(&active_jobs_count);
    if (remaining) {
        tc_governor_on_job_completed(0);
    }

    pthread_mutex_lock(&listener_mutex);
    /* Count telemetry cleanup separately from completed_jobs: the last job
     * to update results may reach this lock before an earlier worker. Keep the
     * registration alive until every reserved slot has actually been returned. */
    if(batch->admitted && batch->jobs_to_release){batch->jobs_to_release--;reserved_jobs--;}
    if(batch->admitted && !batch->jobs_to_release){batch->admitted=false;if(batches)g_hash_table_remove(batches,&batch->batch_id);}
    pthread_mutex_unlock(&listener_mutex);

    g_free(final_dst);
    g_free(temp_pattern);
    g_free(base_name);
    g_free(source_stats);
    g_free(job->src_path);
    g_free(job->target_format);
    if (job->merge_paths) {
        g_ptr_array_free(job->merge_paths, TRUE);
    }
    g_free(job);
    tc_batch_context_free(batch); /* job reference */
}

static void *scheduler_thread(void *data) {
    TcScheduler *pool=data;
    for(;;){
        pthread_mutex_lock(&pool->mutex);
        while(g_queue_is_empty(&pool->queue)&&!pool->stopping)pthread_cond_wait(&pool->ready,&pool->mutex);
        if(g_queue_is_empty(&pool->queue)&&pool->stopping){pthread_mutex_unlock(&pool->mutex);return NULL;}
        TcJobItem *job=g_queue_pop_head(&pool->queue);
        pthread_mutex_unlock(&pool->mutex);
        worker_func(job,NULL);
    }
}
static bool start_scheduler(TcScheduler *pool,unsigned count) {
    pool->stopping=false;
    for(unsigned i=0;i<count;i++) {
        int error=pthread_create(&pool->threads[pool->count],NULL,scheduler_thread,pool);
        if(error){tc_log_error("Cannot start worker: %s",g_strerror(error));return false;}
        pool->count++;
    }
    return true;
}
void tc_worker_pool_init(void) {
    if(worker_pool.count||media_pool.count)return;
    tc_governor_init();
    tc_temp_job_recover();
    if(!start_scheduler(&worker_pool,tc_worker_thread_limit())||!start_scheduler(&media_pool,1))tc_worker_pool_shutdown();
    else tc_log_info("Worker pools initialized: %u general workers, 1 media worker",worker_pool.count);
}
void tc_worker_pool_submit_job(TcJobItem *job) {
    if(!job)return;
    TcBatchContext *batch=tc_batch_context_ref(job->batch);
    bool registered=tc_worker_pool_begin_batch(batch,batch->batch_id);
    g_atomic_int_inc(&active_jobs_count);
    TcScheduler *pool=job_uses_media_pool(job)?&media_pool:&worker_pool;
    pthread_mutex_lock(&pool->mutex);
    bool accepted=registered && pool->count && !pool->stopping;
    if(accepted){g_queue_push_tail(&pool->queue,job);pthread_cond_signal(&pool->ready);}
    pthread_mutex_unlock(&pool->mutex);
    if(!accepted){g_cancellable_cancel(batch->cancellable);worker_func(job,NULL);}
}
static void stop_scheduler(TcScheduler *pool) {
    pthread_mutex_lock(&pool->mutex);pool->stopping=true;pthread_cond_broadcast(&pool->ready);pthread_mutex_unlock(&pool->mutex);
    for(unsigned i=0;i<pool->count;i++)pthread_join(pool->threads[i],NULL);
    pool->count=0;
}
void tc_worker_pool_shutdown(void) {
    stop_scheduler(&media_pool);stop_scheduler(&worker_pool);
    pthread_mutex_lock(&listener_mutex);
    g_clear_pointer(&batches,g_hash_table_unref);
    if(global_ui_listener_fd>=0){close(global_ui_listener_fd);global_ui_listener_fd=-1;}
    pthread_mutex_unlock(&listener_mutex);
}
