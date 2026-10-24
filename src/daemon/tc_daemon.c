#define _GNU_SOURCE
#include "tc_common.h"
#include "tc_ipc.h"
#include "tc_logging.h"
#include "tc_governor.h"
#include "tc_worker_pool.h"
#include "tc_dispatcher.h"
#include "tc_engine.h"
#include "tc_disk_guard.h"
#include <signal.h>
#include <glib-unix.h>

#define SD_LISTEN_FDS_START 3

static GMainLoop *main_loop = NULL;

static gboolean on_signal(gpointer data) {
    (void)data;
    tc_log_info("Received termination signal, quitting main loop...");
    if (main_loop) {
        g_main_loop_quit(main_loop);
    }
    return G_SOURCE_CONTINUE;
}

static gboolean on_client_incoming(GIOChannel *source, GIOCondition condition, gpointer user_data) {
    (void)user_data;
    if (condition & (G_IO_ERR | G_IO_HUP)) {
        return FALSE;
    }

    int listen_fd = g_io_channel_unix_get_fd(source);
    struct sockaddr_un addr;
    socklen_t addr_len = sizeof(addr);

    int client_fd = accept4(listen_fd, (struct sockaddr *)&addr, &addr_len, SOCK_CLOEXEC);
    if (client_fd >= 0) {
        tc_dispatcher_handle_client(client_fd);
    }

    return TRUE;
}

static int get_listen_socket(void) {
    /* 1. Check systemd socket activation */
    const char *listen_pid_str = g_getenv("LISTEN_PID");
    const char *listen_fds_str = g_getenv("LISTEN_FDS");

    if (listen_pid_str && listen_fds_str) {
        pid_t pid = (pid_t)atoi(listen_pid_str);
        int fds = atoi(listen_fds_str);
        if (pid == getpid() && fds > 0) {
            tc_log_info("Activated via systemd user socket (fd=%d)", SD_LISTEN_FDS_START);
            return SD_LISTEN_FDS_START;
        }
    }

    /* 2. Standalone bind if systemd socket not present */
    char *sock_path = tc_ipc_get_socket_path();
    unlink(sock_path);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        tc_log_error("Failed to create socket: %s", g_strerror(errno));
        g_free(sock_path);
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        tc_log_error("Failed to bind socket at %s: %s", sock_path, g_strerror(errno));
        close(fd);
        g_free(sock_path);
        return -1;
    }

    if (listen(fd, 64) != 0) {
        tc_log_error("Failed to listen on socket: %s", g_strerror(errno));
        close(fd);
        g_free(sock_path);
        return -1;
    }

    /* Restrict socket permissions to user only (0600) */
    chmod(sock_path, 0600);
    tc_log_info("Standalone socket bound at %s (0600)", sock_path);
    g_free(sock_path);
    return fd;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    guint interrupt_source=g_unix_signal_add(SIGINT,on_signal,NULL);
    guint terminate_source=g_unix_signal_add(SIGTERM,on_signal,NULL);
    signal(SIGPIPE, SIG_IGN); /* Prevent SIGPIPE from crashing daemon */

    tc_log_info("TeraConvert-C Daemon v%s starting...", TC_VERSION);

    /* Initialize core subsystems */
    tc_governor_init();
    tc_worker_pool_init();
    tc_engines_init_registry();
    tc_disk_guard_cleanup_stale_temps();

    int listen_fd = get_listen_socket();
    if (listen_fd < 0) {
        tc_log_error("Could not obtain listening socket, aborting.");
        return 1;
    }

    GIOChannel *channel = g_io_channel_unix_new(listen_fd);
    g_io_channel_set_encoding(channel, NULL, NULL);
    g_io_channel_set_buffered(channel, FALSE);
    g_io_add_watch(channel, G_IO_IN | G_IO_ERR | G_IO_HUP, on_client_incoming, NULL);

    main_loop = g_main_loop_new(NULL, FALSE);
    tc_log_info("TeraConvert Daemon event loop running");
    g_main_loop_run(main_loop);

    /* Clean shutdown */
    tc_log_info("Shutting down TeraConvert Daemon...");
    g_io_channel_unref(channel);
    close(listen_fd);
    g_main_loop_unref(main_loop);

    g_source_remove(interrupt_source);g_source_remove(terminate_source);
    tc_dispatcher_shutdown();
    tc_worker_pool_cancel_all();
    tc_worker_pool_shutdown();
    tc_engines_cleanup_all();

    tc_log_info("TeraConvert Daemon terminated cleanly.");
    return 0;
}
