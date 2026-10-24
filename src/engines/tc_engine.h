#ifndef TC_ENGINE_H
#define TC_ENGINE_H

#include "tc_common.h"

typedef void (*TcProgressCallback)(float fraction, uint32_t eta_sec, void *user_data);
typedef struct {uint64_t memory_bytes,output_bytes;} TcEngineEstimate;

typedef struct {
    const char *name;
    TcCategory category;
    bool (*supports_format)(const char *format);
    TcStatusCode (*convert)(
        const char *src_path,
        const char *dst_path,
        const char *target_format,
        uint16_t target_width,
        uint16_t target_height,
        GCancellable *cancellable,
        char **error_out,
        TcProgressCallback progress_cb,
        void *user_data
    );
    void (*init)(void);
    void (*cleanup)(void);
    TcStatusCode (*estimate)(const char *source,const char *format,uint16_t width,uint16_t height,
        GCancellable *cancel,TcEngineEstimate *estimate,char **error);
} TcEngine;

/* Engine Registry Functions */
void tc_engines_init_registry(void);
TcEngine *tc_engines_find_for_task(TcCategory category, const char *target_format);
void tc_engines_cleanup_all(void);
#ifdef HAVE_FFMPEG
TcStatusCode tc_media_estimate(const char *source,const char *format,uint16_t width,uint16_t height,
    GCancellable *cancel,TcEngineEstimate *estimate,char **error);
#endif

#endif /* TC_ENGINE_H */
