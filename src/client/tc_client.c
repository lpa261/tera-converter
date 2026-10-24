#define _GNU_SOURCE
#include "tc_client.h"
#include "tc_logging.h"
#include <sys/mman.h>

#ifdef HAVE_LIBNOTIFY
#include <libnotify/notify.h>
#endif

static void tc_client_notify_daemon_unavailable(void) {
#ifdef HAVE_LIBNOTIFY
    if (!notify_is_initted()) {
        notify_init("TeraConvert");
    }
    NotifyNotification *n = notify_notification_new(
        "TeraConvert Service Unavailable",
        "Could not connect to teraconvert-daemon. Ensure teraconvert.socket is active.",
        "dialog-warning"
    );
    notify_notification_set_urgency(n, NOTIFY_URGENCY_NORMAL);
    notify_notification_show(n, NULL);
    g_object_unref(n);
#endif
    tc_log_warn("teraconvert-daemon is unreachable within %d ms timeout", TC_CLIENT_POLL_TIMEOUT_MS);
}

static bool tc_client_dispatch_full(
    TcActionType action,
    const char *target_format,
    uint16_t width,
    uint16_t height,
    GList *file_uris,
    TcOverwritePolicy overwrite_policy
) {
    if (!file_uris) return false;

    struct timespec start_ts, end_ts;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);

    /* 1. Build binary payload of local file paths */
    GString *payload = g_string_sized_new(4096);
    uint32_t total_files = 0;

    for (GList *l = file_uris; l != NULL; l = l->next) {
        const char *uri = (const char *)l->data;
        g_autofree char *path = g_filename_from_uri(uri, NULL, NULL);
        if (path && strchr(path, '\n')) {
            g_string_free(payload, TRUE);
            return false;
        }
        if (path && g_file_test(path, G_FILE_TEST_EXISTS)) {
            g_string_append(payload, path);
            g_string_append_c(payload, '\n');
            total_files++;
        }
    }

    if (total_files == 0) {
        g_string_free(payload, TRUE);
        return false;
    }

    if (payload->len > TC_IPC_MAX_PAYLOAD_BYTES) {
        g_string_free(payload, TRUE);
        return false;
    }

    TcCompressionPolicy policy=tc_compression_policy(action,target_format);
    TcIpcRequestV2 envelope={
        .batch_id=((uint64_t)g_random_int()<<32)|g_random_int(),
        .min_saved_bytes=policy.min_saved_bytes,.min_saved_ppm=policy.min_saved_ppm,.mode=policy.mode};
    if(!envelope.batch_id)envelope.batch_id=1;
    g_string_prepend_len(payload,(const char*)&envelope,sizeof(envelope));
    if(payload->len>TC_IPC_MAX_PAYLOAD_BYTES){g_string_free(payload,TRUE);return false;}
    /* 2. Connect to daemon socket non-blocking (<= 20ms fail-safe) */
    int sock_fd = tc_ipc_connect_client(TC_CLIENT_POLL_TIMEOUT_MS);
    if (sock_fd < 0) {
        g_string_free(payload, TRUE);
        tc_client_notify_daemon_unavailable();
        return false;
    }

    /* Negotiate before submitting work, so an old/starting daemon cannot
     * silently queue a request that the caller believes was rejected. */
    TcIpcHeader ping={.magic=TC_IPC_MAGIC,.version=2,.msg_type=TC_IPC_MSG_PING};
    TcIpcHeader pong;TcIpcAckV2 capabilities;
    bool compatible=tc_ipc_send_all(sock_fd,&ping,sizeof(ping),100) &&
        tc_ipc_receive_all(sock_fd,&pong,sizeof(pong),1000) && pong.magic==TC_IPC_MAGIC &&
        pong.version==2 && pong.msg_type==TC_IPC_MSG_PONG && pong.payload_size==sizeof(capabilities) && !pong.has_fd &&
        tc_ipc_receive_all(sock_fd,&capabilities,sizeof(capabilities),500);
    close(sock_fd);
    if(!compatible){tc_log_error("IPC v2 negotiation failed; update client and daemon together");g_string_free(payload,TRUE);tc_client_notify_daemon_unavailable();return false;}
    sock_fd=tc_ipc_connect_client(TC_CLIENT_POLL_TIMEOUT_MS);
    if(sock_fd<0){g_string_free(payload,TRUE);tc_client_notify_daemon_unavailable();return false;}

    /* 3. Prepare wire header */
    TcIpcHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = TC_IPC_MAGIC;
    hdr.version = TC_IPC_VERSION;
    hdr.msg_type = TC_IPC_MSG_CONVERT_REQ;
    hdr.action = action;
    hdr.total_files = total_files;
    hdr.payload_size = (uint32_t)payload->len;
    hdr.overwrite_policy = (uint8_t)overwrite_policy;
    hdr.target_width = width;
    hdr.target_height = height;
    if (target_format) {
        strncpy(hdr.target_format, target_format, sizeof(hdr.target_format) - 1);
    }

    bool success = false;

    if (payload->len <= TC_INLINE_PAYLOAD_MAX_BYTES) {
        /* Inline payload path */
        hdr.has_fd = 0;
        GByteArray *frame = g_byte_array_sized_new(sizeof(hdr) + payload->len);
        g_byte_array_append(frame, (const guint8 *)&hdr, sizeof(hdr));
        g_byte_array_append(frame, (const guint8 *)payload->str, payload->len);
        success = tc_ipc_send_all(sock_fd, frame->data, frame->len, TC_CLIENT_POLL_TIMEOUT_MS);
        g_byte_array_unref(frame);
    } else {
        /* Bulk sealed memfd path (> 32KB payload, immune to socket buffer limits) */
        hdr.has_fd = 1;
        int mem_fd = memfd_create(TC_MEMFD_SEAL_NAME, MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (mem_fd >= 0) {
            if (ftruncate(mem_fd, payload->len) == 0) {
                ssize_t written = write(mem_fd, payload->str, payload->len);
                if (written == (ssize_t)payload->len) {
                    /* Irreversible sealing */
                    if (fcntl(mem_fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) < 0) {
                        close(mem_fd);
                        close(sock_fd);
                        g_string_free(payload, TRUE);
                        return false;
                    }

                    /* Send header + memfd via SCM_RIGHTS */
                    ssize_t sent = tc_ipc_send_fd(sock_fd, &hdr, sizeof(hdr), mem_fd);
                    if (sent > 0 && sent <= (ssize_t)sizeof(hdr)) {
                        success = tc_ipc_send_all(sock_fd, (char *)&hdr + sent,
                            sizeof(hdr) - (size_t)sent, TC_CLIENT_POLL_TIMEOUT_MS);
                    }
                }
            }
            close(mem_fd);
        }
    }

    if(success){
        TcIpcHeader response;TcIpcAckV2 ack;
        success=tc_ipc_receive_all(sock_fd,&response,sizeof(response),500) &&
            response.magic==TC_IPC_MAGIC && response.version==2 && response.msg_type==TC_IPC_MSG_CONVERT_RESP &&
            response.payload_size==sizeof(ack) && !response.has_fd &&
            tc_ipc_receive_all(sock_fd,&ack,sizeof(ack),500) && ack.batch_id==envelope.batch_id && ack.status==0;
        if(!success)tc_log_error("Daemon did not acknowledge IPC v2. Update client and daemon together; no legacy fallback.");
    }
    close(sock_fd);
    g_string_free(payload, TRUE);

    clock_gettime(CLOCK_MONOTONIC, &end_ts);
    double elapsed_ms = (end_ts.tv_sec - start_ts.tv_sec) * 1000.0 +
                        (end_ts.tv_nsec - start_ts.tv_nsec) / 1000000.0;

    tc_log_debug("Dispatched %u files in %.3f ms (success=%d)", total_files, elapsed_ms, success);
    return success;
}

bool tc_client_dispatch_request(
    TcActionType action,
    const char *target_format,
    GList *file_uris,
    TcOverwritePolicy overwrite_policy
) {
    return tc_client_dispatch_full(action, target_format, 0, 0, file_uris, overwrite_policy);
}

bool tc_client_dispatch_resize_request(
    uint16_t width,
    uint16_t height,
    GList *file_uris,
    TcOverwritePolicy overwrite_policy
) {
    return tc_client_dispatch_full(TC_ACTION_RESIZE, "png", width, height, file_uris, overwrite_policy);
}

bool tc_client_launch_ui(const char *mode, GList *file_uris) {
    if (!mode) return false;

    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(argv, g_strdup("/usr/bin/teraconvert-ui"));
    g_ptr_array_add(argv, g_strdup_printf("--mode=%s", mode));

    for (GList *l = file_uris; l != NULL; l = l->next) {
        g_autofree char *path = g_filename_from_uri((const char *)l->data, NULL, NULL);
        if (path) {
            g_ptr_array_add(argv, g_strdup(path));
        }
    }
    g_ptr_array_add(argv, NULL);

    GError *err = NULL;
    gboolean spawned = g_spawn_async(
        NULL,
        (char **)argv->pdata,
        NULL,
        G_SPAWN_SEARCH_PATH,
        NULL,
        NULL,
        NULL,
        &err
    );

    if (!spawned && err) {
        tc_log_error("Failed to launch teraconvert-ui: %s", err->message);
        g_error_free(err);
    }

    g_ptr_array_free(argv, TRUE);
    return spawned;
}
