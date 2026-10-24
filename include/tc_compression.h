#ifndef TC_COMPRESSION_H
#define TC_COMPRESSION_H
#include "tc_common.h"
typedef enum {
    TC_MODE_CONVERT, TC_MODE_PRESERVE, TC_MODE_PERCEPTUAL
} TcCompressionMode;
typedef enum {
    TC_PHASE_QUEUED, TC_PHASE_WAITING, TC_PHASE_PROBING, TC_PHASE_RUNNING,
    TC_PHASE_VALIDATING, TC_PHASE_COMMITTING, TC_PHASE_DONE
} TcCompressionPhase;
typedef enum {
    TC_RESULT_PENDING, TC_RESULT_SAVED, TC_RESULT_NO_BENEFIT,
    TC_RESULT_UNSUPPORTED_SAFE_MODE, TC_RESULT_QUALITY_REJECTED,
    TC_RESULT_CANCELLED, TC_RESULT_FAILED, TC_RESULT_EXISTS
} TcCompressionStatus;
typedef struct {
    TcCompressionMode mode;
    uint64_t min_saved_bytes;
    uint32_t min_saved_ppm;
    /* parts per million, 0..1,000,000 */
} TcCompressionPolicy;
typedef struct {
    TcCompressionStatus status;
    uint64_t input_bytes, output_bytes, saved_bytes;
    char reason[256];
} TcCompressionResult;
static inline TcCompressionPolicy tc_compression_policy(TcActionType action, const char *format) {
    TcCompressionPolicy p = {
        .mode = TC_MODE_CONVERT, .min_saved_bytes = 1
    };
    if (action == TC_ACTION_COMPRESS) {
        p.mode = (!g_strcmp0(format, "comp-web") || !g_strcmp0(format, "comp-hq") ||
        !g_strcmp0(format, "comp-saver") || !g_strcmp0(format, "comp-pdf") || (g_str_has_prefix(format ? format : "", "comp-pdf-") && g_strcmp0(format,"comp-pdf-safe")))
        ? TC_MODE_PERCEPTUAL : TC_MODE_PRESERVE;
    }
    if(!g_strcmp0(format,"safe-flac"))p.mode=TC_MODE_PRESERVE;
    return p;
}
static inline bool tc_compression_has_benefit(const TcCompressionPolicy *p, uint64_t in, uint64_t out) {
    if (!p || p->min_saved_ppm > 1000000 || out >= in || in - out < p->min_saved_bytes) return false;
    /* Avoid multiplying byte counts and overflowing uint64_t. */
    uint64_t threshold = (in / 1000000) * p->min_saved_ppm +
    ((in % 1000000) * p->min_saved_ppm + 999999) / 1000000;
    return in - out >= threshold;
}
#endif
