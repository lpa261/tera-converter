#ifndef TC_PROCESS_RUNNER_H
#define TC_PROCESS_RUNNER_H

#include "tc_common.h"

typedef void (*TcProcessOutputFunc)(const char *data, gsize length, gpointer user_data);

typedef struct {
    gint64 timeout_ms;          /* Required, positive wall-clock limit. */
    guint terminate_grace_ms;   /* SIGTERM grace before SIGKILL, at most 10 s. */
    uint64_t max_file_bytes;    /* Optional hard per-file RLIMIT_FSIZE for child. */
    uint64_t max_address_bytes; /* Optional hard RLIMIT_AS for child. */
} TcProcessOptions;

typedef struct {
    GPid pid;
    gboolean exited;
    int exit_code;              /* -1 unless exited normally. */
    int term_signal;            /* 0 unless terminated by a signal. */
    gboolean stderr_truncated;
    char *stderr_tail;          /* Owned UTF-8, bounded to the last 16 KiB of input. */
} TcProcessResult;

/* argv is borrowed and NULL-terminated; no shell is involved. Callback runs on
 * the calling thread and must return promptly. A fresh result is initialized by
 * this call and must be cleared afterwards. NULL cancellable/result is allowed.
 * The runner owns/reaps its direct child and terminates remaining members of its
 * process group before returning, even when the group leader exits first. */
TcStatusCode tc_process_run(const char *const *argv,
                            const TcProcessOptions *options,
                            GCancellable *cancellable,
                            TcProcessOutputFunc stdout_cb,
                            gpointer user_data,
                            TcProcessResult *result,
                            char **error_out);

void tc_process_result_clear(TcProcessResult *result);

/* Borrowed job limits on the current worker thread, reset before job cleanup. */
void tc_process_set_budget(const char *output_path, uint64_t max_output_bytes,
                           uint64_t max_address_bytes);
gboolean tc_run_helper(const char *const *argv, GCancellable *cancel, GError **error);

#endif
