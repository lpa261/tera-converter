#define _GNU_SOURCE
#include "tc_dispatcher.h"
#include "tc_ipc.h"
#include "tc_logging.h"
#include "tc_worker_pool.h"
#include <sys/mman.h>
#include <glib-unix.h>

static bool verify_peer_credentials(int fd) {
    struct ucred cred;
    socklen_t len = sizeof(cred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
        tc_log_error("Failed to get peer credentials: %s", g_strerror(errno));
        return false;
    }
    if (cred.uid != getuid()) {
        tc_log_error("Peer UID mismatch! Expected %u, got %u", getuid(), cred.uid);
        return false;
    }
    return true;
}

static void launch_hud_if_needed(TcActionType action, const char *target_format, uint32_t total_files, const char *first_path, uint64_t batch_id) {
    (void)target_format;
    bool launch = false;
    if (total_files > 1) {
        launch = true;
    } else if (action == TC_ACTION_COMPRESS || action == TC_ACTION_REMOVE_BG || action == TC_ACTION_MERGE) {
        launch = true;
    } else if (first_path) {
        const char *dot = strrchr(first_path, '.');
        if (dot) {
            const char *ext = dot + 1;
            if (g_ascii_strcasecmp(ext, "mp4") == 0 ||
                g_ascii_strcasecmp(ext, "mkv") == 0 ||
                g_ascii_strcasecmp(ext, "avi") == 0 ||
                g_ascii_strcasecmp(ext, "mov") == 0 ||
                g_ascii_strcasecmp(ext, "webm") == 0 ||
                g_ascii_strcasecmp(ext, "flv") == 0 ||
                g_ascii_strcasecmp(ext, "wmv") == 0) {
                launch = true;
            }
        }
    }

    if (launch) {
        const char *bin = g_file_test("/usr/bin/teraconvert-ui", G_FILE_TEST_EXISTS) ? "/usr/bin/teraconvert-ui" : "teraconvert-ui";
        g_autofree char *base = first_path ? g_path_get_basename(first_path) : NULL;
        g_autofree char *total_arg = g_strdup_printf("--total=%u", total_files);
        g_autofree char *action_arg = g_strdup_printf("--action=%d", (int)action);
        g_autofree char *file_arg = g_strdup_printf("--file=%s", base ? base : "");
        g_autofree char *batch_arg=g_strdup_printf("--batch-id=%" G_GUINT64_FORMAT,batch_id);
        char *argv[] = {(char *)bin, "--hud", total_arg, action_arg, file_arg, batch_arg, NULL};
        g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                      NULL, NULL, NULL, NULL);
    }
}

static void dispatch_request(int client_fd, TcIpcHeader hdr, const char *payload_buffer) {
    if(hdr.msg_type==TC_IPC_MSG_PING){
        struct __attribute__((packed)){TcIpcHeader header;TcIpcAckV2 ack;} reply={
            .header={.magic=TC_IPC_MAGIC,.version=2,.msg_type=TC_IPC_MSG_PONG,.payload_size=sizeof(TcIpcAckV2)},
            .ack={.capabilities=tc_ipc_capabilities()}};
        tc_ipc_send_all(client_fd,&reply,sizeof(reply),20);close(client_fd);return;
    }
    if (hdr.msg_type == TC_IPC_MSG_REGISTER_UI_LISTENER) {
        if(hdr.version==2){uint64_t id;memcpy(&id,payload_buffer,sizeof(id));tc_worker_pool_register_batch_listener(id,client_fd);}
        else tc_worker_pool_register_ui_listener(client_fd);
        return;
    }
    if (hdr.msg_type == TC_IPC_MSG_CANCEL_REQ) {
        if(hdr.version==2){uint64_t id;memcpy(&id,payload_buffer,sizeof(id));tc_worker_pool_cancel_batch(id);}
        else tc_worker_pool_cancel_active();
        close(client_fd);
        return;
    }
    TcIpcRequestV2 envelope={0};
    size_t path_length=hdr.payload_size;
    TcCompressionPolicy policy=tc_compression_policy(hdr.action,hdr.target_format);
    if(hdr.version==2){
        if(path_length<=sizeof(envelope)){close(client_fd);return;}
        memcpy(&envelope,payload_buffer,sizeof(envelope));
        if(!envelope.batch_id || envelope.mode!=(uint32_t)policy.mode || envelope.min_saved_ppm>1000000 || (envelope.flags&~1U)){close(client_fd);return;}
        policy.min_saved_bytes=MAX(1,envelope.min_saved_bytes);policy.min_saved_ppm=envelope.min_saved_ppm;
        payload_buffer+=sizeof(envelope);path_length-=sizeof(envelope);
    }
    if (memchr(payload_buffer, '\0', path_length)) {
        close(client_fd);
        return;
    }
    /* Validate the entire batch before submitting any worker. */
    char **paths = g_strsplit(payload_buffer, "\n", -1);

    guint count = 0;
    bool valid_paths = true;
    for (guint i = 0; paths[i]; i++) {
        if (!*paths[i] && paths[i + 1] == NULL) continue; /* trailing newline */
        if (!g_path_is_absolute(paths[i]) ||
            !g_file_test(paths[i], G_FILE_TEST_IS_REGULAR)) {
            valid_paths = false;
            break;
        }
        count++;
    }
    if (!valid_paths || count == 0 || count != hdr.total_files) {
        g_strfreev(paths);
        close(client_fd);
        return;
    }

    TcBatchContext *batch=tc_batch_context_new(hdr.action==TC_ACTION_MERGE?1:hdr.total_files,client_fd);
    batch->protocol_version=hdr.version;batch->policy=policy;
    if(!tc_worker_pool_begin_batch(batch,hdr.version==2?envelope.batch_id:batch->batch_id)){
        tc_batch_context_free(batch);g_strfreev(paths);return;
    }
    if(hdr.version==2){
        struct __attribute__((packed)){TcIpcHeader header;TcIpcAckV2 ack;} response={0};
        response.header=(TcIpcHeader){.magic=TC_IPC_MAGIC,.version=2,.msg_type=TC_IPC_MSG_CONVERT_RESP,.payload_size=sizeof(response.ack)};
        response.ack.batch_id=batch->batch_id;response.ack.capabilities=tc_ipc_capabilities();
        if(!tc_ipc_send_all(client_fd,&response,sizeof(response),20)){
            tc_worker_pool_abort_batch(batch);tc_batch_context_free(batch);g_strfreev(paths);return;
        }
    }
    /* Each worker checks and reserves its actual output filesystem. */

    /* Launch floating HUD for batch or time-consuming actions */
    if(hdr.version==1 || !(envelope.flags&1U))
        launch_hud_if_needed((TcActionType)hdr.action, hdr.target_format, hdr.total_files, paths[0], batch->batch_id);

    if (hdr.action == TC_ACTION_MERGE) {
        /* Merge all files into 1 target file */

        TcJobItem *job = g_new0(TcJobItem, 1);
        job->action = TC_ACTION_MERGE;
        job->target_format = g_strdup(hdr.target_format);
        job->overwrite_policy = (TcOverwritePolicy)hdr.overwrite_policy;
        job->merge_paths = g_ptr_array_new_with_free_func(g_free);
        job->batch = batch;

        for (guint i = 0; paths[i] != NULL; i++) {
            const char *p = paths[i];
            if (!p || *p == '\0') continue;

            if (!job->src_path) {
                job->src_path = g_strdup(p);
            }
            g_ptr_array_add(job->merge_paths, g_strdup(p));
        }

        if (job->merge_paths->len == 0) {
            g_free(job->src_path);
            g_free(job->target_format);
            g_ptr_array_free(job->merge_paths, TRUE);
            g_free(job);
            tc_batch_context_free(batch);
        } else {
            tc_worker_pool_submit_job(job);
            tc_batch_context_free(batch); /* each submitted job retains its reference */
        }
    } else {
        /* Standard individual parallel jobs */

        for (guint i = 0; paths[i] != NULL; i++) {
            const char *p = paths[i];
            if (!p || *p == '\0') continue;

            TcJobItem *job = g_new0(TcJobItem, 1);
            job->src_path = g_strdup(p);
            job->target_format = g_strdup(hdr.target_format);
            job->action = (TcActionType)hdr.action;
            job->overwrite_policy = (TcOverwritePolicy)hdr.overwrite_policy;
            job->target_width = hdr.target_width;
            job->target_height = hdr.target_height;
            job->batch = batch;

            tc_worker_pool_submit_job(job);
        }
        tc_batch_context_free(batch);
    }

    g_strfreev(paths);
}

/* Request intake is incremental on the GLib main context. Slow clients cannot
 * block socket activation, HUD registration or cancellation of other jobs. */
typedef struct {
    int fd, memfd;
    TcIpcHeader header;
    size_t header_bytes, payload_bytes;
    char *payload;
    guint watch, deadline;
} TcPendingRequest;

static GHashTable *pending_requests;

static void pending_free(TcPendingRequest *request) {
    if (request->watch) g_source_remove(request->watch);
    if (request->deadline) g_source_remove(request->deadline);
    if (request->fd >= 0) close(request->fd);
    if (request->memfd >= 0) close(request->memfd);
    g_free(request->payload);
    g_hash_table_remove(pending_requests, request);
    g_free(request);
}

static gboolean request_expired(gpointer data) {
    TcPendingRequest *request = data;
    request->deadline = 0;
    pending_free(request);
    return G_SOURCE_REMOVE;
}

static gboolean request_ready(gint fd, GIOCondition condition, gpointer data) {
    TcPendingRequest *request = data;
    (void)condition; /* drain data first, even when the peer has already hung up */
    while (request->header_bytes < sizeof(request->header)) {
        int received_fd = -1;
        ssize_t bytes = tc_ipc_recv_fd(fd, (char *)&request->header + request->header_bytes,
            sizeof(request->header) - request->header_bytes, &received_fd);
        if (received_fd >= 0) {
            if (request->memfd >= 0) { close(received_fd); goto reject; }
            request->memfd = received_fd;
        }
        if (bytes < 0 && errno == EINTR) continue;
        if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return G_SOURCE_CONTINUE;
        if (bytes <= 0) goto reject;
        request->header_bytes += bytes;
    }
    if (!tc_ipc_header_is_valid(&request->header) ||
        ((request->memfd >= 0) != (request->header.has_fd != 0))) goto reject;
    size_t length = request->header.payload_size;
    if (request->memfd >= 0) {
        int seals = fcntl(request->memfd, F_GET_SEALS);
        int required = F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL;
        struct stat st;
        if (seals < 0 || (seals & required) != required ||
            fstat(request->memfd, &st) || st.st_size != (off_t)length) goto reject;
        void *map = mmap(NULL, length, PROT_READ, MAP_PRIVATE, request->memfd, 0);
        if (map == MAP_FAILED) goto reject;
        request->payload = g_malloc(length + 1);
        memcpy(request->payload, map, length);
        request->payload[length] = '\0';
        munmap(map, length);
        request->payload_bytes = length;
        close(request->memfd);
        request->memfd = -1;
    } else {
        if (!request->payload) request->payload = g_malloc0(length + 1);
        while (request->payload_bytes < length) {
            ssize_t bytes = recv(fd, request->payload + request->payload_bytes,
                                 length - request->payload_bytes, MSG_DONTWAIT);
            if (bytes < 0 && errno == EINTR) continue;
            if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return G_SOURCE_CONTINUE;
            if (bytes <= 0) goto reject;
            request->payload_bytes += bytes;
        }
    }
    request->watch = 0;
    request->fd = -1; /* dispatch_request consumes the socket */
    dispatch_request(fd, request->header, request->payload);
    pending_free(request);
    return G_SOURCE_REMOVE;
reject:
    request->watch = 0;
    pending_free(request);
    return G_SOURCE_REMOVE;
}

void tc_dispatcher_handle_client(int fd) {
    if (!pending_requests) pending_requests = g_hash_table_new(g_direct_hash, g_direct_equal);
    int flags = fcntl(fd, F_GETFL, 0);
    if (g_hash_table_size(pending_requests) >= 64 || !verify_peer_credentials(fd) ||
        flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        tc_log_warn("Rejected IPC intake: flags=%d errno=%s", flags, g_strerror(errno));
        close(fd);
        return;
    }
    TcPendingRequest *request = g_new0(TcPendingRequest, 1);
    request->fd = fd;
    request->memfd = -1;
    g_hash_table_add(pending_requests, request);
    request->watch = g_unix_fd_add(fd, G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL, request_ready, request);
    request->deadline = g_timeout_add(1000, request_expired, request);
}

void tc_dispatcher_shutdown(void) {
    if (!pending_requests) return;
    while (g_hash_table_size(pending_requests)) {
        GHashTableIter iter;
        gpointer request;
        g_hash_table_iter_init(&iter, pending_requests);
        g_hash_table_iter_next(&iter, &request, NULL);
        pending_free(request);
    }
    g_clear_pointer(&pending_requests, g_hash_table_unref);
}
