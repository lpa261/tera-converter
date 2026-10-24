#ifndef TC_IPC_H
#define TC_IPC_H

#include "tc_common.h"
#include "tc_compression.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>

#define TC_IPC_MAGIC 0x54434F4E /* "TCON" in ASCII */
#define TC_IPC_VERSION 2
#define TC_IPC_MAX_FILES 10000U
static inline uint32_t tc_ipc_capabilities(void){
    uint32_t capabilities=1U|2U; /* batch routing and structured results */
#ifdef HAVE_IMAGE_PRESERVE
    capabilities|=4U;
#endif
#ifdef HAVE_POPPLER
    capabilities|=8U;
#endif
#ifdef HAVE_FFMPEG
    capabilities|=16U|32U;
#endif
#ifdef HAVE_LIBARCHIVE
    capabilities|=64U;
#endif
    return capabilities;
}

/* v1 header remains exactly 48 bytes. v2 adds explicit payload envelopes. */
typedef struct __attribute__((packed)) {
    uint64_t batch_id;
    uint64_t min_saved_bytes;
    uint32_t min_saved_ppm;
    uint32_t mode;
    uint32_t flags; /* bit 0: headless client; do not launch HUD */
} TcIpcRequestV2;
typedef struct __attribute__((packed)) {
    uint64_t batch_id;
    uint32_t status, capabilities;
    char reason[96];
} TcIpcAckV2;

static inline ssize_t tc_ipc_send_bytes(int fd, const void *bytes, size_t length, int flags) {
    struct iovec iov = {.iov_base = (void *)bytes, .iov_len = length};
    struct msghdr message = {.msg_iov = &iov, .msg_iovlen = 1};
    return sendmsg(fd, &message, flags);
}

typedef enum {
    TC_IPC_MSG_CONVERT_REQ      = 0x01,
    TC_IPC_MSG_CONVERT_RESP     = 0x02,
    TC_IPC_MSG_PROGRESS_EVENT   = 0x03,
    TC_IPC_MSG_CANCEL_REQ       = 0x04,
    TC_IPC_MSG_PING             = 0x05,
    TC_IPC_MSG_PONG             = 0x06,
    TC_IPC_MSG_REGISTER_UI_LISTENER = 0x10
} TcIpcMsgType;

/* Wire frame header: 48 bytes aligned */
typedef struct __attribute__((packed)) {
    uint32_t magic;             /* TC_IPC_MAGIC */
    uint16_t version;           /* Protocol version = 1 */
    uint16_t msg_type;          /* TcIpcMsgType */
    uint16_t action;            /* TcActionType */
    uint8_t  has_fd;            /* 1 if payload is passed via SCM_RIGHTS memfd */
    uint8_t  overwrite_policy;  /* TcOverwritePolicy */
    uint32_t total_files;       /* Count of files in request */
    uint32_t payload_size;      /* Size of inline payload or sealed memfd size */
    uint16_t target_width;      /* Target width for resize, 0 = keep original */
    uint16_t target_height;     /* Target height for resize, 0 = keep original */
    char     target_format[24]; /* e.g., "webp", "comp-web", "comp-saver" */
} TcIpcHeader;

/* Bound allocations before trusting lengths supplied over IPC. */
#define TC_IPC_MAX_PAYLOAD_BYTES (16U * 1024U * 1024U)

/* Syntactic validation alone would let an action accidentally select a lossy
 * backend under another action's policy. Keep the public action/format pairs
 * explicit; optional backends report unsupported after this contract check. */
static inline bool tc_ipc_action_format_valid(const TcIpcHeader *hdr) {
    static const char *images[]={"png","jpg","jpeg","webp","avif","bmp","tiff","tif","ico","gif",NULL};
    static const char *conversions[]={"png","jpg","jpeg","webp","avif","bmp","tiff","tif","ico","gif","pdf","txt","html","extract-images","mp4","mov","mkv","avi","webm","mp3","wav","flac","m4a","ogg","opus","safe-flac",NULL};
    static const char *compression[]={"comp-padding","comp-remux","comp-image","comp-office","comp-pdf-safe","comp-pdf","comp-pdf-web","comp-pdf-print","comp-web","comp-hq","comp-saver",NULL};
    const char *const *formats=NULL;
    if(hdr->action==TC_ACTION_RESIZE){
        if(!hdr->target_width || !hdr->target_height || hdr->target_width>TC_MAX_IMAGE_DIMENSION || hdr->target_height>TC_MAX_IMAGE_DIMENSION)return false;
        if(!strcmp(hdr->target_format,"resize"))return true;
        formats=images;
    }else{
        if(hdr->target_width || hdr->target_height)return false;
        if(hdr->action==TC_ACTION_REMOVE_BG)return !strcmp(hdr->target_format,"rmbg");
        if(hdr->action==TC_ACTION_MERGE)return !strcmp(hdr->target_format,"pdf");
        if(hdr->action==TC_ACTION_COMPRESS)formats=compression;
        else if(hdr->action==TC_ACTION_CONVERT || hdr->action==TC_ACTION_BATCH)formats=conversions;
        else return false;
    }
    for(unsigned i=0;formats[i];i++)if(!strcmp(hdr->target_format,formats[i]))return true;
    return false;
}

static inline bool tc_ipc_header_is_valid(const TcIpcHeader *hdr) {
    if (!hdr || hdr->magic != TC_IPC_MAGIC || (hdr->version != 1 && hdr->version != 2) || hdr->has_fd > 1)
        return false;
    if(hdr->version==2 && hdr->msg_type==TC_IPC_MSG_PING)return !hdr->has_fd&&!hdr->payload_size;
    if (hdr->msg_type == TC_IPC_MSG_CANCEL_REQ ||
        hdr->msg_type == TC_IPC_MSG_REGISTER_UI_LISTENER)
        return !hdr->has_fd && hdr->payload_size == (hdr->version == 2 ? sizeof(uint64_t) : 0);
    if (hdr->msg_type == TC_IPC_MSG_CONVERT_REQ) {
        if (!memchr(hdr->target_format, '\0', sizeof(hdr->target_format))) return false;
        if(!tc_ipc_action_format_valid(hdr))return false;
        for (const char *p = hdr->target_format; *p; p++)
            if (!g_ascii_isalnum(*p) && *p != '-') return false;
    }
    return hdr->msg_type == TC_IPC_MSG_CONVERT_REQ &&
        hdr->action <= TC_ACTION_COMPRESS &&
        hdr->overwrite_policy <= TC_OVERWRITE_REPLACE &&
        hdr->total_files > 0 && hdr->total_files <= TC_IPC_MAX_FILES && hdr->total_files <= hdr->payload_size &&
        hdr->payload_size > 0 && hdr->payload_size <= TC_IPC_MAX_PAYLOAD_BYTES &&
        (hdr->has_fd || hdr->payload_size <= TC_INLINE_PAYLOAD_MAX_BYTES) &&
        hdr->target_format[0] != '\0' &&
        memchr(hdr->target_format, '\0', sizeof(hdr->target_format)) != NULL;
}

/* Wire progress event payload */
typedef struct __attribute__((packed)) {
    uint32_t completed_files;
    uint32_t total_files;
    uint32_t failed_files;
    float    items_per_sec;
    uint32_t eta_seconds;
    uint32_t elapsed_seconds;
    float    progress_fraction; /* 0.0f to 1.0f for real smooth progress bar */
    char     current_filename[128];
} TcIpcProgressPayload;

typedef struct __attribute__((packed)) {
    TcIpcProgressPayload progress;
    uint64_t batch_id, input_bytes, output_bytes, saved_bytes;
    uint32_t phase, status, mode, saved_files, no_benefit_files, unsupported_files, cancelled_files, existing_files;
    char reason[256];
} TcIpcProgressV2;
typedef struct {
    unsigned char data[sizeof(TcIpcHeader) + sizeof(TcIpcProgressV2)];
    size_t used;
} TcIpcProgressReaderV2;

static inline int tc_ipc_read_progress_v2(int fd, TcIpcProgressReaderV2 *reader, TcIpcProgressV2 *payload) {
    while (reader->used < sizeof(reader->data)) {
        ssize_t count = recv(fd, reader->data + reader->used, sizeof(reader->data) - reader->used, MSG_DONTWAIT);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (count <= 0) return -1;
        reader->used += (size_t)count;
        if (reader->used >= sizeof(TcIpcHeader)) {
            TcIpcHeader header;
            memcpy(&header, reader->data, sizeof(header));
            if (header.magic != TC_IPC_MAGIC || header.version != 2 || header.has_fd ||
                header.msg_type != TC_IPC_MSG_PROGRESS_EVENT || header.payload_size != sizeof(*payload)) return -1;
        }
    }
    memcpy(payload, reader->data + sizeof(TcIpcHeader), sizeof(*payload));
    reader->used = 0;
    return payload->batch_id && payload->phase <= TC_PHASE_DONE && payload->status <= TC_RESULT_EXISTS && payload->mode <= TC_MODE_PERCEPTUAL &&
        payload->progress.total_files <= TC_IPC_MAX_FILES &&
        (uint64_t)payload->saved_files+payload->no_benefit_files+payload->unsupported_files+payload->cancelled_files+payload->existing_files+payload->progress.failed_files==payload->progress.completed_files &&
        memchr(payload->reason, 0, sizeof(payload->reason)) &&
        memchr(payload->progress.current_filename, 0, sizeof(payload->progress.current_filename)) &&
        g_utf8_validate(payload->reason,-1,NULL) && g_utf8_validate(payload->progress.current_filename,-1,NULL) &&
        payload->progress.progress_fraction >= 0 && payload->progress.progress_fraction <= 1 &&
        payload->progress.completed_files <= payload->progress.total_files &&
        payload->progress.failed_files <= payload->progress.completed_files ? 1 : -1;
}

typedef struct {
    unsigned char data[sizeof(TcIpcHeader) + sizeof(TcIpcProgressPayload)];
    size_t used;
} TcIpcProgressReader;

/* 1 = complete validated frame, 0 = need more bytes, -1 = disconnected/invalid. */
static inline int tc_ipc_read_progress(int fd, TcIpcProgressReader *reader,
                                       TcIpcProgressPayload *payload) {
    while (reader->used < sizeof(reader->data)) {
        ssize_t bytes = recv(fd, reader->data + reader->used,
                             sizeof(reader->data) - reader->used, MSG_DONTWAIT);
        if (bytes < 0 && errno == EINTR) continue;
        if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (bytes <= 0) return -1;
        reader->used += bytes;
        if (reader->used >= sizeof(TcIpcHeader)) {
            TcIpcHeader header;
            memcpy(&header, reader->data, sizeof(header));
            if (header.magic != TC_IPC_MAGIC || header.version != 1 ||
                header.msg_type != TC_IPC_MSG_PROGRESS_EVENT || header.has_fd ||
                header.payload_size != sizeof(*payload)) return -1;
        }
    }
    memcpy(payload, reader->data + sizeof(TcIpcHeader), sizeof(*payload));
    reader->used = 0;
    if (!memchr(payload->current_filename, '\0', sizeof(payload->current_filename)) ||
        !(payload->progress_fraction >= 0.0f && payload->progress_fraction <= 1.0f) ||
        payload->completed_files > payload->total_files || payload->failed_files > payload->completed_files)
        return -1;
    return 1;
}

/* Bounded complete writes on the nonblocking client socket. The deadline is
 * absolute, so EINTR and partial writes cannot extend the caller's budget. */
static inline bool tc_ipc_send_all(int fd, const void *data, size_t length, int timeout_ms) {
    const unsigned char *bytes = data;
    size_t offset = 0;
    gint64 deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
    while (offset < length) {
        ssize_t sent = tc_ipc_send_bytes(fd, bytes + offset, length - offset, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent > 0) offset += sent;
        else if (sent < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) return false;
        else if (sent == 0) return false;
        if (offset == length) return true;
        gint64 remaining = deadline - g_get_monotonic_time();
        if (remaining <= 0) return false;
        struct pollfd pfd = {.fd = fd, .events = POLLOUT};
        int result = poll(&pfd, 1, (int)((remaining + 999) / 1000));
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0 || (pfd.revents & (POLLHUP | POLLERR | POLLNVAL))) return false;
    }
    return true;
}

static inline bool tc_ipc_receive_all(int fd,void *data,size_t length,int timeout_ms) {
    size_t used=0;gint64 deadline=g_get_monotonic_time()+(gint64)timeout_ms*1000;
    while(used<length){
        ssize_t n=recv(fd,(char*)data+used,length-used,MSG_DONTWAIT);
        if(n>0){used+=(size_t)n;continue;}
        if(n==0 || (errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR))return false;
        gint64 left=deadline-g_get_monotonic_time();if(left<=0)return false;
        struct pollfd pollfd={.fd=fd,.events=POLLIN};
        int rc=poll(&pollfd,1,(int)((left+999)/1000));
        if(rc<0&&errno==EINTR)continue;
        if(rc<=0)return false;
    }
    return true;
}

/* Helpers to get user socket path */
static inline char *tc_ipc_get_socket_path(void) {
    const char *runtime_dir = g_get_user_runtime_dir();
    return g_build_filename(runtime_dir, TC_SOCKET_NAME, NULL);
}

/* Helper to safely connect client to daemon socket with 20ms poll non-blocking */
static inline int tc_ipc_connect_client(int timeout_ms) {
    char *sock_path = tc_ipc_get_socket_path();
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        g_free(sock_path);
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
    g_free(sock_path);

    int res = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (res < 0) {
        if (errno == EINPROGRESS) {
            struct pollfd pfd = { .fd = fd, .events = POLLOUT };
            int pret = poll(&pfd, 1, timeout_ms);
            if (pret > 0 && (pfd.revents & POLLOUT)) {
                int so_error = 0;
                socklen_t len = sizeof(so_error);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) == 0 && so_error == 0) {
                    return fd;
                }
            }
        }
        close(fd);
        return -1;
    }

    return fd;
}

/* Helper to send file descriptor via SCM_RIGHTS */
static inline ssize_t tc_ipc_send_fd(int sock_fd, const void *buf, size_t len, int fd_to_send) {
    struct msghdr msg = {0};
    struct iovec iov[1];
    union {
        struct cmsghdr cm;
        char control[CMSG_SPACE(sizeof(int))];
    } control_un = {0};

    iov[0].iov_base = (void *)buf;
    iov[0].iov_len = len;
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;

    if (fd_to_send >= 0) {
        msg.msg_control = control_un.control;
        msg.msg_controllen = sizeof(control_un.control);
        struct cmsghdr *cmptr = CMSG_FIRSTHDR(&msg);
        cmptr->cmsg_len = CMSG_LEN(sizeof(int));
        cmptr->cmsg_level = SOL_SOCKET;
        cmptr->cmsg_type = SCM_RIGHTS;
        *((int *)CMSG_DATA(cmptr)) = fd_to_send;
    }

    return sendmsg(sock_fd, &msg, MSG_NOSIGNAL);
}

/* Helper to receive file descriptor via SCM_RIGHTS */
static inline ssize_t tc_ipc_recv_fd(int sock_fd, void *buf, size_t len, int *received_fd) {
    struct msghdr msg = {0};
    struct iovec iov[1];
    union {
        struct cmsghdr cm;
        char control[CMSG_SPACE(16 * sizeof(int))];
    } control_un = {0};

    if (received_fd) *received_fd = -1;

    iov[0].iov_base = buf;
    iov[0].iov_len = len;
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control_un.control;
    msg.msg_controllen = sizeof(control_un.control);

    ssize_t ret = recvmsg(sock_fd, &msg, MSG_CMSG_CLOEXEC);
    if (ret <= 0) return ret;

    int owned_fd = -1;
    bool invalid = (msg.msg_flags & MSG_CTRUNC) != 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (size_t i = 0; i < count; i++) {
                int fd;
                memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof(fd));
                if (owned_fd >= 0) { close(fd); invalid = true; }
                else owned_fd = fd;
            }
        }
    }
    if (invalid) {
        if (owned_fd >= 0) close(owned_fd);
        errno = EPROTO;
        return -1;
    }
    if (received_fd) *received_fd = owned_fd;
    else if (owned_fd >= 0) close(owned_fd);
    return ret;
}

#endif /* TC_IPC_H */
