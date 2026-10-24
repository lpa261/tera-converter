#define _GNU_SOURCE
#include "tc_common.h"
#include "tc_ipc.h"
#include "tc_logging.h"
#include <assert.h>
#include <sys/mman.h>

static void test_ipc_header_size(void) {
    g_assert_cmpuint(sizeof(TcIpcHeader), ==, 48);
    tc_log_info("✓ Test Passed: TcIpcHeader wire size is exactly 48 bytes aligned");
}

static void test_temp_sibling_path(void) {
    const char *target = "/home/panh/Pictures/landscape.webp";
    int suffix_len = 0;
    char *temp_path = tc_create_temp_sibling_path(target, &suffix_len);
    g_assert_nonnull(temp_path);
    g_assert_true(g_str_has_prefix(temp_path, "/home/panh/Pictures/.landscape-"));
    g_assert_true(g_str_has_suffix(temp_path, ".webp"));
    g_assert_cmpint(suffix_len, ==, 5);
    g_free(temp_path);
    tc_log_info("✓ Test Passed: Two-phase atomic temp sibling path generation with extension");
}

static void test_sealed_memfd(void) {
    int fd = memfd_create(TC_MEMFD_SEAL_NAME, MFD_CLOEXEC | MFD_ALLOW_SEALING);
    g_assert_cmpint(fd, >=, 0);

    const char *payload = "/tmp/image1.jpg\n/tmp/image2.png\n";
    size_t len = strlen(payload);
    g_assert_cmpint(ftruncate(fd, len), ==, 0);
    g_assert_cmpint(write(fd, payload, len), ==, (ssize_t)len);

    int res = fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL);
    g_assert_cmpint(res, ==, 0);

    int seals = fcntl(fd, F_GET_SEALS);
    int required = F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL;
    g_assert_cmpint((seals & required), ==, required);

    close(fd);
    tc_log_info("✓ Test Passed: Sealed anonymous memfd creation and seal verification");
}

static void test_progress_payload_layout(void) {
    TcIpcProgressPayload p;
    memset(&p, 0, sizeof(p));
    p.completed_files = 3;
    p.total_files = 5;
    p.failed_files = 0;
    p.items_per_sec = 2.5f;
    p.eta_seconds = 12;
    p.progress_fraction = 0.60f;
    strncpy(p.current_filename, "video_clip.mp4", sizeof(p.current_filename) - 1);

    g_assert_cmpuint(p.completed_files, ==, 3);
    g_assert_cmpuint(p.total_files, ==, 5);
    g_assert_cmpfloat(p.progress_fraction, ==, 0.60f);
    g_assert_cmpstr(p.current_filename, ==, "video_clip.mp4");
    tc_log_info("✓ Test Passed: TcIpcProgressPayload wire alignment and data integrity");
}

static void test_socket_path_resolution(void) {
    char *sock_path = tc_ipc_get_socket_path();
    g_assert_nonnull(sock_path);
    g_assert_true(g_str_has_suffix(sock_path, TC_SOCKET_NAME));
    tc_log_info("✓ Test Passed: Socket path resolved to %s", sock_path);
    g_free(sock_path);
}

static void test_request_validation(void) {
    TcIpcHeader hdr = {
        .magic = TC_IPC_MAGIC, .version = 1,
        .msg_type = TC_IPC_MSG_CONVERT_REQ,
        .total_files = 1, .payload_size = 12,
        .target_format = "png"
    };
    g_assert_true(tc_ipc_header_is_valid(&hdr));
    hdr.payload_size = UINT32_MAX;
    g_assert_false(tc_ipc_header_is_valid(&hdr));
    hdr.payload_size = TC_INLINE_PAYLOAD_MAX_BYTES + 1;
    g_assert_false(tc_ipc_header_is_valid(&hdr));
    hdr.has_fd = 1;
    g_assert_true(tc_ipc_header_is_valid(&hdr));
    memset(hdr.target_format, 'x', sizeof(hdr.target_format));
    g_assert_false(tc_ipc_header_is_valid(&hdr));
    hdr.msg_type = TC_IPC_MSG_CANCEL_REQ;
    g_assert_false(tc_ipc_header_is_valid(&hdr));
    hdr.has_fd = 0;
    hdr.payload_size = 0;
    g_assert_true(tc_ipc_header_is_valid(&hdr));
    hdr.version = 2;
    g_assert_false(tc_ipc_header_is_valid(&hdr));
}

static void test_fragmented_progress(void) {
    int sockets[2];
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets), ==, 0);
    struct __attribute__((packed)) { TcIpcHeader header; TcIpcProgressPayload payload; } frame = {0};
    frame.header.magic = TC_IPC_MAGIC;
    frame.header.version = 1;
    frame.header.msg_type = TC_IPC_MSG_PROGRESS_EVENT;
    frame.header.payload_size = sizeof(frame.payload);
    frame.payload.total_files = 1;
    frame.payload.progress_fraction = 0.25f;
    TcIpcProgressReader reader = {0};
    TcIpcProgressPayload payload = {0};
    for (size_t i = 0; i < sizeof(frame); i++) {
        g_assert_true(tc_ipc_send_all(sockets[0], (char *)&frame + i, 1, 20));
        int result = tc_ipc_read_progress(sockets[1], &reader, &payload);
        g_assert_cmpint(result, ==, i + 1 == sizeof(frame) ? 1 : 0);
    }
    g_assert_cmpfloat(payload.progress_fraction, ==, 0.25f);
    frame.header.payload_size++;
    g_assert_true(tc_ipc_send_all(sockets[0], &frame, sizeof(frame), 20));
    g_assert_cmpint(tc_ipc_read_progress(sockets[1], &reader, &payload), ==, -1);
    close(sockets[0]);
    close(sockets[1]);
}

static void test_reject_multiple_fds(void) {
    int sockets[2];
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), ==, 0);
    int fds[2] = {open("/dev/null", O_RDONLY), open("/dev/null", O_RDONLY)};
    char byte = 'x';
    struct iovec iov = {.iov_base = &byte, .iov_len = 1};
    union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(fds))]; } control = {0};
    struct msghdr message = {.msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control.bytes, .msg_controllen = sizeof(control.bytes)};
    struct cmsghdr *c = CMSG_FIRSTHDR(&message);
    c->cmsg_len = CMSG_LEN(sizeof(fds));
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    memcpy(CMSG_DATA(c), fds, sizeof(fds));
    g_assert_cmpint(sendmsg(sockets[0], &message, MSG_NOSIGNAL), ==, 1);
    int received = -1;
    g_assert_cmpint(tc_ipc_recv_fd(sockets[1], &byte, 1, &received), ==, -1);
    g_assert_cmpint(errno, ==, EPROTO);
    g_assert_cmpint(received, ==, -1);
    close(fds[0]); close(fds[1]); close(sockets[0]); close(sockets[1]);
}

static void test_compression_policy(void) {
    TcCompressionPolicy p=tc_compression_policy(TC_ACTION_COMPRESS,"comp-padding");
    g_assert_cmpint(p.mode,==,TC_MODE_PRESERVE);
    g_assert_true(tc_compression_has_benefit(&p,100,99));
    g_assert_false(tc_compression_has_benefit(&p,100,100));
    p.min_saved_ppm=100000;
    g_assert_false(tc_compression_has_benefit(&p,101,91));
    g_assert_true(tc_compression_has_benefit(&p,101,90));
    g_assert_true(tc_compression_has_benefit(&p,UINT64_MAX,0));
    p.min_saved_ppm=1000001;g_assert_false(tc_compression_has_benefit(&p,100,0));
    g_assert_cmpint(tc_compression_policy(TC_ACTION_COMPRESS,"comp-web").mode,==,TC_MODE_PERCEPTUAL);
    g_assert_cmpint(tc_compression_policy(TC_ACTION_COMPRESS,"comp-pdf").mode,==,TC_MODE_PERCEPTUAL);
    g_assert_cmpint(tc_compression_policy(TC_ACTION_COMPRESS,"comp-pdf-safe").mode,==,TC_MODE_PRESERVE);
}
static void test_v2_progress(void) {
    int pair[2];g_assert_cmpint(socketpair(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0,pair),==,0);
    struct __attribute__((packed)){TcIpcHeader h;TcIpcProgressV2 p;} frame={
        .h={.magic=TC_IPC_MAGIC,.version=2,.msg_type=TC_IPC_MSG_PROGRESS_EVENT,.payload_size=sizeof(TcIpcProgressV2)},
        .p={.batch_id=987,.phase=TC_PHASE_DONE,.status=TC_RESULT_NO_BENEFIT,.no_benefit_files=1,
            .progress={.completed_files=1,.total_files=1,.progress_fraction=1}}};
    TcIpcProgressReaderV2 reader={0};TcIpcProgressV2 payload={0};
    g_assert_true(tc_ipc_send_all(pair[0],&frame,13,100));
    g_assert_cmpint(tc_ipc_read_progress_v2(pair[1],&reader,&payload),==,0);
    g_assert_true(tc_ipc_send_all(pair[0],(char*)&frame+13,sizeof(frame)-13,100));
    g_assert_cmpint(tc_ipc_read_progress_v2(pair[1],&reader,&payload),==,1);
    g_assert_cmpuint(payload.batch_id,==,987);g_assert_cmpuint(payload.no_benefit_files,==,1);
    frame.h.version=99;g_assert_true(tc_ipc_send_all(pair[0],&frame,sizeof(frame),100));
    g_assert_cmpint(tc_ipc_read_progress_v2(pair[1],&reader,&payload),==,-1);
    close(pair[0]);close(pair[1]);
}

static void test_contract_and_unterminated_progress(void) {
    TcIpcHeader h={.magic=TC_IPC_MAGIC,.version=2,.msg_type=TC_IPC_MSG_CONVERT_REQ,
        .total_files=1,.payload_size=100,.target_format="comp-web"};
    g_assert_false(tc_ipc_header_is_valid(&h)); /* convert may not disguise compression */
    h.action=TC_ACTION_COMPRESS;g_assert_true(tc_ipc_header_is_valid(&h));
    h.target_width=100;g_assert_false(tc_ipc_header_is_valid(&h));
    h.action=TC_ACTION_RESIZE;g_strlcpy(h.target_format,"png",sizeof(h.target_format));
    g_assert_false(tc_ipc_header_is_valid(&h));h.target_height=100;g_assert_true(tc_ipc_header_is_valid(&h));
    int pair[2];g_assert_cmpint(socketpair(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0,pair),==,0);
    struct __attribute__((packed)){TcIpcHeader h;TcIpcProgressV2 p;} frame={
        .h={.magic=TC_IPC_MAGIC,.version=2,.msg_type=TC_IPC_MSG_PROGRESS_EVENT,.payload_size=sizeof(TcIpcProgressV2)},
        .p={.batch_id=12,.progress={.total_files=1}}};
    memset(frame.p.reason,'x',sizeof(frame.p.reason));
    TcIpcProgressReaderV2 reader={0};TcIpcProgressV2 result;
    g_assert_true(tc_ipc_send_all(pair[0],&frame,sizeof(frame),100));
    g_assert_cmpint(tc_ipc_read_progress_v2(pair[1],&reader,&result),==,-1);
    close(pair[0]);close(pair[1]);
}

int main(int argc, char *argv[]) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/ipc/action-contract-and-string-bounds",test_contract_and_unterminated_progress);
    g_test_add_func("/compression/policy-thresholds",test_compression_policy);
    g_test_add_func("/ipc/v2-progress-fragments",test_v2_progress);
    g_test_add_func("/ipc/request-validation", test_request_validation);
    g_test_add_func("/ipc/header-layout", test_ipc_header_size);
    g_test_add_func("/ipc/temp-sibling", test_temp_sibling_path);
    g_test_add_func("/ipc/sealed-memfd", test_sealed_memfd);
    g_test_add_func("/ipc/progress-layout", test_progress_payload_layout);
    g_test_add_func("/ipc/socket-path", test_socket_path_resolution);
    g_test_add_func("/ipc/progress-fragments", test_fragmented_progress);
    g_test_add_func("/ipc/reject-multiple-fds", test_reject_multiple_fds);
    return g_test_run();
}
