#include "tc_engine.h"
#include "tc_logging.h"
#include "tc_process_runner.h"
#include "tc_runtime_limits.h"
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <math.h>
#ifdef HAVE_FFMPEG
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include "tc_media_probe.h"
#include "tc_media_padding.h"
#include "tc_media_plan.h"


static TcGpuType detected_gpu = TC_GPU_NONE;
static char *detected_vaapi_node = NULL;
static const char *active_ffmpeg_bin = NULL;

static const char *get_ffmpeg_binary(void) {
    if (active_ffmpeg_bin) return active_ffmpeg_bin;
    if (g_file_test("/usr/bin/ffmpeg", G_FILE_TEST_IS_EXECUTABLE)) {
        active_ffmpeg_bin = "/usr/bin/ffmpeg";
    } else {
        active_ffmpeg_bin = "ffmpeg";
    }
    return active_ffmpeg_bin;
}

static void ffmpeg_init_engine(void) {
    const char *bin = get_ffmpeg_binary();
    const TcProcessOptions probe_options = { .timeout_ms = 5000, .terminate_grace_ms = 250 };

    /* 1. Universal Probe: NVIDIA NVENC */
    if (g_file_test("/dev/nvidiactl", G_FILE_TEST_EXISTS) || g_file_test("/dev/nvidia0", G_FILE_TEST_EXISTS)) {
        const char *nv_probe[] = {bin, "-nostdin", "-filter_threads", "1", "-threads", "1", "-f", "lavfi", "-i", "nullsrc=s=64x64:d=0.05",
                                  "-threads", "1", "-c:v", "h264_nvenc", "-f", "null", "-", NULL};
        if (tc_process_run(nv_probe, &probe_options, NULL, NULL, NULL, NULL, NULL) == TC_STATUS_OK) {
            detected_gpu = TC_GPU_NVENC;
            tc_log_info("Universal GPU Auto-Detection: NVIDIA NVENC Hardware Acceleration ACTIVE");
            return;
        }
    }

    /* 2. Universal Probe: Intel and AMD VAAPI via standard Linux DRM render nodes */
    for (int i = 128; i <= 136; i++) {
        g_autofree char *node = g_strdup_printf("/dev/dri/renderD%d", i);
        if (access(node, R_OK | W_OK) == 0) {
            const char *va_probe[] = {bin, "-nostdin", "-filter_threads", "1", "-threads", "1", "-vaapi_device", node, "-f", "lavfi", "-i",
                                      "nullsrc=s=64x64:d=0.05", "-vf", "format=nv12,hwupload",
                                      "-threads", "1", "-c:v", "h264_vaapi", "-f", "null", "-", NULL};
            if (tc_process_run(va_probe, &probe_options, NULL, NULL, NULL, NULL, NULL) == TC_STATUS_OK) {
                detected_gpu = TC_GPU_VAAPI;
                detected_vaapi_node = g_strdup(node);
                tc_log_info("Universal GPU Auto-Detection: VAAPI Hardware Acceleration ACTIVE (Node: %s)", node);
                return;
            }
        }
    }

    detected_gpu = TC_GPU_NONE;
    tc_log_info("Universal GPU Auto-Detection: No compatible GPU hardware encoder found. Using CPU Software Encoding (veryfast preset)");
}

static void ffmpeg_cleanup_engine(void) {
    g_free(detected_vaapi_node);
    detected_vaapi_node = NULL;
    detected_gpu = TC_GPU_NONE;
}

static TcStatusCode probe_media_meta(const char *path, TcMediaMeta *meta,
                                       GCancellable *cancellable, char **error_out) {
    memset(meta, 0, sizeof(*meta));
    g_autoptr(TcMediaInfo) info = NULL;
    TcStatusCode status = tc_media_probe_file(path, NULL, cancellable, &info, error_out);
    if (status != TC_STATUS_OK) return status;
    meta->file_size = info->source_stat.st_size;
    meta->duration_us = info->duration_us > 0 ? info->duration_us : 0;
    for (guint i = 0; i < info->streams->len; i++) {
        const TcMediaStream *stream = g_ptr_array_index(info->streams, i);
        const AVCodecParameters *codec = stream->codec;
        if (codec->codec_type == AVMEDIA_TYPE_VIDEO && meta->v_codec == AV_CODEC_ID_NONE &&
            !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            meta->v_codec = codec->codec_id;
            meta->width = codec->width;
            meta->height = codec->height;
        } else if (codec->codec_type == AVMEDIA_TYPE_AUDIO && meta->a_codec == AV_CODEC_ID_NONE) {
            meta->a_codec = codec->codec_id;
        }
    }
    if (meta->duration_us > 100000) {
        double bitrate = meta->file_size * 8000000.0 / meta->duration_us;
        meta->bitrate_bps = bitrate >= G_MAXINT64 ? G_MAXINT64 : (int64_t)bitrate;
    }
    if (meta->bitrate_bps <= 0) meta->bitrate_bps = info->bitrate_bps > 0 ? info->bitrate_bps : 2500000;
    return TC_STATUS_OK;
}

static bool ffmpeg_supports_format(const char *format) {
    if (!format) return false;
    return (g_ascii_strcasecmp(format, "mp4") == 0 ||
            g_ascii_strcasecmp(format, "webm") == 0 ||
            g_ascii_strcasecmp(format, "mkv") == 0 ||
            g_ascii_strcasecmp(format, "avi") == 0 ||
            g_ascii_strcasecmp(format, "mov") == 0 ||
            g_ascii_strcasecmp(format, "mp3") == 0 ||
            g_ascii_strcasecmp(format, "wav") == 0 ||
            g_ascii_strcasecmp(format, "flac") == 0 ||
            g_ascii_strcasecmp(format, "m4a") == 0 ||
            g_ascii_strcasecmp(format, "ogg") == 0 ||
            g_ascii_strcasecmp(format, "opus") == 0 ||
            g_ascii_strcasecmp(format, "gif") == 0 ||
            g_ascii_strcasecmp(format, "comp-web") == 0 ||
            g_ascii_strcasecmp(format, "comp-saver") == 0 ||
            g_ascii_strcasecmp(format, "comp-hq") == 0 ||
            g_ascii_strcasecmp(format, "comp-padding") == 0);
}

typedef struct {
    int64_t duration_us, started_us;
    char line[512];
    gsize line_length;
    gboolean discard_line;
    TcProgressCallback callback;
    void *user_data;
} FfmpegProgress;

static void ffmpeg_progress_output(const char *data, gsize length, gpointer user_data) {
    FfmpegProgress *progress = user_data;
    for (gsize i = 0; i < length; i++) {
        if (data[i] == '\n' || data[i] == '\r') {
            progress->line[progress->line_length] = '\0';
            if (!progress->discard_line && progress->duration_us > 0 &&
                g_str_has_prefix(progress->line, "out_time_us=")) {
                char *end = NULL;
                errno = 0;
                int64_t current = g_ascii_strtoll(progress->line + 12, &end, 10);
                if (!errno && end != progress->line + 12 && *end == '\0' && current >= 0) {
                    float fraction = CLAMP((double)current / progress->duration_us, 0.0, 0.99);
                    double elapsed = (g_get_monotonic_time() - progress->started_us) / 1000000.0;
                    double remaining = fraction > 0.02 ? elapsed / fraction - elapsed : 0;
                    uint32_t eta = (uint32_t)CLAMP(remaining, 0, G_MAXUINT32);
                    if (progress->callback) progress->callback(fraction, eta, progress->user_data);
                }
            }
            progress->line_length = 0;
            progress->discard_line = FALSE;
        } else if (progress->line_length < sizeof(progress->line) - 1) {
            progress->line[progress->line_length++] = data[i];
        } else {
            progress->discard_line = TRUE;
        }
    }
}

static TcStatusCode ffmpeg_run_pipeline(
    GPtrArray *argv,
    int64_t duration_us,
    GCancellable *cancellable,
    char **error_out,
    TcProgressCallback progress_cb,
    void *user_data
) {
    const TcProcessOptions options = { .timeout_ms = 6 * 60 * 60 * 1000, .terminate_grace_ms = 500 };
    FfmpegProgress progress = { .duration_us = duration_us, .started_us = g_get_monotonic_time(),
                               .callback = progress_cb, .user_data = user_data };
    TcProcessResult result = {0};
    TcStatusCode status = tc_process_run((const char *const *)argv->pdata, &options,
                                          cancellable, ffmpeg_progress_output, &progress,
                                          &result, error_out);
    tc_process_result_clear(&result);
    /* progress=end is not proof that the process exited successfully. */
    if (status == TC_STATUS_OK && progress_cb) progress_cb(1.0f, 0, user_data);
    return status;
}

static TcStatusCode ffmpeg_convert(
    const char *src_path,
    const char *dst_path,
    const char *target_format,
    uint16_t target_width,
    uint16_t target_height,
    GCancellable *cancellable,
    char **error_out,
    TcProgressCallback progress_cb,
    void *user_data
) {
    (void)target_width;
    (void)target_height;

    if (!ffmpeg_supports_format(target_format)) {
        if (error_out) *error_out = g_strdup("Unsupported media target format");
        return TC_STATUS_ERROR_UNSUPPORTED;
    }

    /* Byte-preserving optimization has its own parser/validator and never
     * enters the legacy encoder, GPU fallback or lossy bitrate policy. */
    if (g_ascii_strcasecmp(target_format, "comp-padding") == 0)
        return tc_media_optimize_padding(src_path, dst_path, cancellable, error_out, progress_cb, user_data);

    TcMediaMeta meta;
    TcStatusCode probe_status = probe_media_meta(src_path, &meta, cancellable, error_out);
    if (probe_status != TC_STATUS_OK) return probe_status;

    int64_t duration_us = meta.duration_us;

    /* 1. First attempt: with GPU acceleration (if detected) or Smart Remuxing */
    GPtrArray *argv = tc_media_plan_legacy(src_path,dst_path,target_format,false,&meta,detected_gpu,detected_vaapi_node,get_ffmpeg_binary());
    TcStatusCode status = ffmpeg_run_pipeline(argv, duration_us, cancellable, error_out, progress_cb, user_data);
    g_ptr_array_free(argv, TRUE);

    /* 2. Resilient Automatic Fallback: If hardware acceleration failed, retry with CPU software encoding */
    if (status == TC_STATUS_ERROR && detected_gpu != TC_GPU_NONE) {
        tc_log_warn("Hardware acceleration failed for %s, automatically falling back to multi-threaded CPU software encoding...", src_path);
        unlink(dst_path);
        if (error_out && *error_out) {
            g_free(*error_out);
            *error_out = NULL;
        }
        GPtrArray *fallback_argv = tc_media_plan_legacy(src_path,dst_path,target_format,true,&meta,detected_gpu,detected_vaapi_node,get_ffmpeg_binary());
        status = ffmpeg_run_pipeline(fallback_argv, duration_us, cancellable, error_out, progress_cb, user_data);
        g_ptr_array_free(fallback_argv, TRUE);
    }

    return status;
}

TcEngine tc_engine_ffmpeg = {
    .name = "ffmpeg_media",
    .category = TC_CATEGORY_MEDIA,
    .supports_format = ffmpeg_supports_format,
    .convert = ffmpeg_convert,
    .init = ffmpeg_init_engine,
    .cleanup = ffmpeg_cleanup_engine,
    .estimate = tc_media_estimate
};

#endif
