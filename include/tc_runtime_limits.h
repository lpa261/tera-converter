#ifndef TC_RUNTIME_LIMITS_H
#define TC_RUNTIME_LIMITS_H

#include "tc_common.h"

/* Bound configured worker pools, not internal codec threads or actual CPU/RSS. */
static inline guint tc_engine_thread_limit(void) {
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 1 ? (guint)MIN(count / 2, 2) : 1;
}

static inline guint tc_worker_thread_limit(void) {
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 1 ? (guint)MIN(count - 1, 4) : 1;
}

#endif
