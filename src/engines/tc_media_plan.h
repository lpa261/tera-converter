#ifndef TC_MEDIA_PLAN_H
#define TC_MEDIA_PLAN_H
#include "tc_engine.h"
#include <libavcodec/avcodec.h>
typedef enum {
    TC_GPU_NONE = 0,
    TC_GPU_VAAPI,  /* Intel (Gen9+) & AMD (Radeon/Ryzen APU) via Linux DRM render nodes */
    TC_GPU_NVENC   /* NVIDIA GPUs */
} TcGpuType;

typedef struct {
    enum AVCodecID v_codec;
    enum AVCodecID a_codec;
    int width;
    int height;
    int64_t duration_us;
    int64_t file_size;
    int64_t bitrate_bps;
} TcMediaMeta;

/* Pure argv construction: all capability and metadata inputs are explicit.
 * Legacy lossy/conversion modes only; preservation engines bypass this planner. */
GPtrArray *tc_media_plan_legacy(const char *src,const char *dst,const char *format,
    bool force_software,const TcMediaMeta *meta,TcGpuType gpu,const char *node,const char *binary);
#endif
