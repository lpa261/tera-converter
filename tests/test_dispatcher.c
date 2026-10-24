#include "tc_dispatcher.h"
#include "tc_worker_pool.h"
#include "tc_ipc.h"
#include <glib/gstdio.h>
#include <sys/mman.h>

/* Observe intake without spawning real codecs or touching the desktop. */
static guint cancelled, submitted;
static bool wrong_peer;
int __real_getsockopt(int, int, int, void *, socklen_t *);
/* Mock only the credential lookup for these framing unit tests. The managed
 * test environment denies SO_PEERCRED even on a local socketpair. Production
 * links libc getsockopt directly and still enforces the real UID check. */
int __wrap_getsockopt(int fd, int level, int option, void *value, socklen_t *length) {
    if (level == SOL_SOCKET && option == SO_PEERCRED && *length >= sizeof(struct ucred)) {
        struct ucred credentials = {.uid = getuid() + (wrong_peer ? 1 : 0), .pid = getpid(), .gid = getgid()};
        memcpy(value, &credentials, sizeof(credentials));
        *length = sizeof(credentials);
        return 0;
    }
    return __real_getsockopt(fd, level, option, value, length);
}
void tc_worker_pool_abort_batch(TcBatchContext *b){(void)b;}
bool tc_worker_pool_begin_batch(TcBatchContext *b,uint64_t id){b->batch_id=id;return true;}
bool tc_worker_pool_cancel_batch(uint64_t id){(void)id;cancelled++;return true;}
void tc_worker_pool_register_batch_listener(uint64_t id,int fd){(void)id;close(fd);}
void tc_worker_pool_cancel_active(void) { cancelled++; }
void tc_worker_pool_register_ui_listener(int fd) { close(fd); }
TcBatchContext *tc_batch_context_new(uint32_t total, int fd) {
    TcBatchContext *batch = g_new0(TcBatchContext, 1);
    batch->total_jobs = total;
    batch->client_notify_fd = fd;
    return batch;
}
void tc_batch_context_free(TcBatchContext *batch) {
    close(batch->client_notify_fd);
    g_free(batch);
}
void tc_worker_pool_submit_job(TcJobItem *job) {
    submitted++;
    g_free(job->src_path);
    g_free(job->target_format);
    if (job->merge_paths) g_ptr_array_unref(job->merge_paths);
    g_free(job);
}
static void pump(guint milliseconds) {
    gint64 end = g_get_monotonic_time() + milliseconds * 1000;
    do {
        while (g_main_context_iteration(NULL, FALSE)) {}
        g_usleep(1000);
    } while (g_get_monotonic_time() < end);
}

static int connect_pair(void) {
    int sockets[2];
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets), ==, 0);
    tc_dispatcher_handle_client(sockets[0]);
    return sockets[1];
}

static void test_slow_peer_and_fragmented_header(void) {
    guint before = cancelled;
    int stalled = connect_pair();
    TcIpcHeader header = {.magic = TC_IPC_MAGIC, .version = 1, .msg_type = TC_IPC_MSG_CANCEL_REQ};
    bool sent = tc_ipc_send_all(stalled, &header, 1, 20);
    if (!sent) g_test_message("send errno=%s fd=%d", g_strerror(errno), stalled);
    g_assert_true(sent);
    pump(5);
    int other = connect_pair();
    g_assert_true(tc_ipc_send_all(other, &header, sizeof(header), 20));
    pump(10);
    g_assert_cmpuint(cancelled, ==, before + 1); /* stalled peer did not block us */
    for (size_t i = 1; i < sizeof(header); i++) {
        g_assert_true(tc_ipc_send_all(stalled, (char *)&header + i, 1, 20));
        pump(1);
    }
    g_assert_cmpuint(cancelled, ==, before + 2);
    close(stalled);
    close(other);
    tc_dispatcher_shutdown();
}

static void test_deadline(void) {
    int fd = connect_pair();
    pump(1100);
    char byte;
    g_assert_cmpint(recv(fd, &byte, 1, MSG_DONTWAIT), ==, 0);
    close(fd);
    tc_dispatcher_shutdown();
}

static void test_fragmented_payload(void) {
    guint before = submitted;
    char *path = NULL;
    int file = g_file_open_tmp("tc-dispatch-XXXXXX", &path, NULL);
    g_assert_cmpint(file, >=, 0);
    close(file);
    g_autofree char *payload = g_strconcat(path, "\n", NULL);
    TcIpcHeader header = {.magic = TC_IPC_MAGIC, .version = 1,
        .msg_type = TC_IPC_MSG_CONVERT_REQ, .total_files = 1,
        .payload_size = strlen(payload), .target_format = "png"};
    int fd = connect_pair();
    g_assert_true(tc_ipc_send_all(fd, &header, sizeof(header), 20));
    pump(5);
    g_assert_cmpuint(submitted, ==, before);
    for (size_t i = 0; i < strlen(payload); i++) {
        g_assert_true(tc_ipc_send_all(fd, payload + i, 1, 20));
        pump(1);
    }
    g_assert_cmpuint(submitted, ==, before + 1);
    close(fd);
    g_unlink(path);
    g_free(path);
    tc_dispatcher_shutdown();
}

static void test_reject_wrong_memfd_size(void) {
    int fd = connect_pair();
    int memfd = memfd_create("tc-test", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    g_assert_cmpint(memfd, >=, 0);
    g_assert_cmpint(ftruncate(memfd, 1), ==, 0);
    g_assert_cmpint(fcntl(memfd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL), ==, 0);
    TcIpcHeader header = {.magic = TC_IPC_MAGIC, .version = 1, .msg_type = TC_IPC_MSG_CONVERT_REQ,
        .has_fd = 1, .total_files = 1, .payload_size = 4096, .target_format = "png"};
    g_assert_cmpint(tc_ipc_send_fd(fd, &header, sizeof(header), memfd), ==, sizeof(header));
    close(memfd);
    pump(10);
    char byte;
    g_assert_cmpint(recv(fd, &byte, 1, MSG_DONTWAIT), ==, 0);
    close(fd);
    tc_dispatcher_shutdown();
}

static void test_reject_wrong_peer(void) {
    wrong_peer = true;
    int fd = connect_pair();
    char byte;
    g_assert_cmpint(recv(fd, &byte, 1, MSG_DONTWAIT), ==, 0);
    close(fd);
    wrong_peer = false;
    tc_dispatcher_shutdown();
}

static void test_v2_control(void) {
    guint before=cancelled;int fd=connect_pair();
    TcIpcHeader h={.magic=TC_IPC_MAGIC,.version=2,.msg_type=TC_IPC_MSG_CANCEL_REQ,.payload_size=8};
    uint64_t id=123456;
    g_assert_true(tc_ipc_send_all(fd,&h,sizeof(h),20));
    g_assert_true(tc_ipc_send_all(fd,&id,3,20));pump(5);g_assert_cmpuint(cancelled,==,before);
    g_assert_true(tc_ipc_send_all(fd,(char*)&id+3,5,20));pump(5);g_assert_cmpuint(cancelled,==,before+1);
    close(fd);
}

static void test_v2_accept_and_reject(void) {
    guint before=submitted;
    char *path=NULL;int file=g_file_open_tmp("tc-dispatch-v2-XXXXXX",&path,NULL);
    g_assert_cmpint(file,>=,0);close(file);
    for(unsigned variant=0;variant<4;variant++){
        TcIpcRequestV2 request={.batch_id=9901+variant,.min_saved_bytes=1,.mode=TC_MODE_PRESERVE,.flags=1};
        if(variant==1)request.mode=TC_MODE_PERCEPTUAL;
        if(variant==2)request.flags=2;
        if(variant==3)request.batch_id=0;
        GString *payload=g_string_new_len((char*)&request,sizeof(request));g_string_append_printf(payload,"%s\n",path);
        TcIpcHeader header={.magic=TC_IPC_MAGIC,.version=2,.msg_type=TC_IPC_MSG_CONVERT_REQ,
            .action=TC_ACTION_COMPRESS,.total_files=1,.payload_size=payload->len,.target_format="comp-image"};
        int fd=connect_pair();
        g_assert_true(tc_ipc_send_all(fd,&header,sizeof(header),100));
        g_assert_true(tc_ipc_send_all(fd,payload->str,payload->len,100));pump(10);
        if(!variant){
            TcIpcHeader reply;TcIpcAckV2 ack;
            g_assert_true(tc_ipc_receive_all(fd,&reply,sizeof(reply),100));
            g_assert_cmpuint(reply.version,==,2);g_assert_cmpuint(reply.msg_type,==,TC_IPC_MSG_CONVERT_RESP);
            g_assert_true(tc_ipc_receive_all(fd,&ack,sizeof(ack),100));g_assert_cmpuint(ack.batch_id,==,request.batch_id);
            g_assert_cmpuint(ack.status,==,0);
        }else{char byte;g_assert_cmpint(recv(fd,&byte,1,MSG_DONTWAIT),==,0);}
        close(fd);g_string_free(payload,TRUE);
    }
    g_assert_cmpuint(submitted,==,before+1);g_unlink(path);g_free(path);tc_dispatcher_shutdown();
}
static void test_v2_negotiate(void) {
    int fd=connect_pair();TcIpcHeader request={.magic=TC_IPC_MAGIC,.version=2,.msg_type=TC_IPC_MSG_PING};
    g_assert_true(tc_ipc_send_all(fd,&request,sizeof(request),100));pump(5);
    TcIpcHeader reply;TcIpcAckV2 ack;
    g_assert_true(tc_ipc_receive_all(fd,&reply,sizeof(reply),100));
    g_assert_cmpuint(reply.msg_type,==,TC_IPC_MSG_PONG);g_assert_cmpuint(reply.payload_size,==,sizeof(ack));
    g_assert_true(tc_ipc_receive_all(fd,&ack,sizeof(ack),100));g_assert_cmpuint(ack.capabilities&3U,==,3U);
    close(fd);tc_dispatcher_shutdown();
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/dispatcher/v2-ack-and-contract",test_v2_accept_and_reject);
    g_test_add_func("/dispatcher/v2-negotiate",test_v2_negotiate);
    g_test_add_func("/dispatcher/slow-peer-and-header-fragments", test_slow_peer_and_fragmented_header);
    g_test_add_func("/dispatcher/deadline", test_deadline);
    g_test_add_func("/dispatcher/payload-fragments", test_fragmented_payload);
    g_test_add_func("/dispatcher/memfd-size", test_reject_wrong_memfd_size);
    g_test_add_func("/dispatcher/wrong-peer", test_reject_wrong_peer);
    g_test_add_func("/dispatcher/v2-control-fragments",test_v2_control);
    return g_test_run();
}
