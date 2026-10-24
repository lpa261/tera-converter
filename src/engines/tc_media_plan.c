#include "tc_media_plan.h"
#include "tc_logging.h"
#include "tc_runtime_limits.h"
#include <math.h>

GPtrArray *tc_media_plan_legacy(
    const char *src_path,
    const char *dst_path,
    const char *target_format,
    bool force_software,
    const TcMediaMeta *meta, TcGpuType detected_gpu, const char *detected_vaapi_node, const char *bin
) {
    enum AVCodecID v_codec = meta ? meta->v_codec : AV_CODEC_ID_NONE;
    enum AVCodecID a_codec = meta ? meta->a_codec : AV_CODEC_ID_NONE;
    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(argv, g_strdup(bin));
    g_ptr_array_add(argv, g_strdup("-y")); /* Overwrite temp destination */
    g_ptr_array_add(argv, g_strdup("-nostdin"));
    g_ptr_array_add(argv, g_strdup("-progress"));
    g_ptr_array_add(argv, g_strdup("pipe:1"));
    g_autofree char *threads_str = g_strdup_printf("%u", tc_engine_thread_limit());
    g_ptr_array_add(argv, g_strdup("-filter_threads"));
    g_ptr_array_add(argv, g_strdup(threads_str));
    g_ptr_array_add(argv, g_strdup("-filter_complex_threads"));
    g_ptr_array_add(argv, g_strdup(threads_str));
    g_ptr_array_add(argv, g_strdup("-threads")); /* Input decoder limit. */
    g_ptr_array_add(argv, g_strdup(threads_str));
    g_ptr_array_add(argv, g_strdup("-i"));
    g_ptr_array_add(argv, g_strdup(src_path));
    g_ptr_array_add(argv, g_strdup("-threads")); /* Output codecs, including audio. */
    g_ptr_array_add(argv, g_strdup(threads_str));
    TcGpuType gpu = force_software ? TC_GPU_NONE : detected_gpu;

    if (g_ascii_strcasecmp(target_format, "mp3") == 0) {
        g_ptr_array_add(argv, g_strdup("-vn"));
        g_ptr_array_add(argv, g_strdup("-c:a"));
        g_ptr_array_add(argv, g_strdup("libmp3lame"));
        g_ptr_array_add(argv, g_strdup("-b:a"));
        g_ptr_array_add(argv, g_strdup("320k"));
        g_ptr_array_add(argv, g_strdup("-f"));
        g_ptr_array_add(argv, g_strdup("mp3"));
    } else if (g_ascii_strcasecmp(target_format, "wav") == 0) {
        g_ptr_array_add(argv, g_strdup("-vn"));
        g_ptr_array_add(argv, g_strdup("-c:a"));
        g_ptr_array_add(argv, g_strdup("pcm_s16le"));
        g_ptr_array_add(argv, g_strdup("-f"));
        g_ptr_array_add(argv, g_strdup("wav"));
    } else if (g_ascii_strcasecmp(target_format, "flac") == 0) {
        g_ptr_array_add(argv, g_strdup("-vn"));
        if (a_codec == AV_CODEC_ID_FLAC) {
            g_ptr_array_add(argv, g_strdup("-c:a"));
            g_ptr_array_add(argv, g_strdup("copy"));
        } else {
            g_ptr_array_add(argv, g_strdup("-c:a"));
            g_ptr_array_add(argv, g_strdup("flac"));
        }
        g_ptr_array_add(argv, g_strdup("-f"));
        g_ptr_array_add(argv, g_strdup("flac"));
    } else if (g_ascii_strcasecmp(target_format, "m4a") == 0) {
        g_ptr_array_add(argv, g_strdup("-vn"));
        if (a_codec == AV_CODEC_ID_AAC) {
            g_ptr_array_add(argv, g_strdup("-c:a"));
            g_ptr_array_add(argv, g_strdup("copy"));
        } else {
            g_ptr_array_add(argv, g_strdup("-c:a"));
            g_ptr_array_add(argv, g_strdup("aac"));
            g_ptr_array_add(argv, g_strdup("-b:a"));
            g_ptr_array_add(argv, g_strdup("256k"));
        }
        g_ptr_array_add(argv, g_strdup("-f"));
        g_ptr_array_add(argv, g_strdup("ipod"));
    } else if (g_ascii_strcasecmp(target_format, "ogg") == 0) {
        g_ptr_array_add(argv, g_strdup("-vn"));
        g_ptr_array_add(argv, g_strdup("-c:a"));
        g_ptr_array_add(argv, g_strdup("libvorbis"));
        g_ptr_array_add(argv, g_strdup("-q:a"));
        g_ptr_array_add(argv, g_strdup("5"));
        g_ptr_array_add(argv, g_strdup("-f"));
        g_ptr_array_add(argv, g_strdup("ogg"));
    } else if (g_ascii_strcasecmp(target_format, "opus") == 0) {
        g_ptr_array_add(argv, g_strdup("-vn"));
        g_ptr_array_add(argv, g_strdup("-c:a"));
        g_ptr_array_add(argv, g_strdup("libopus"));
        g_ptr_array_add(argv, g_strdup("-b:a"));
        g_ptr_array_add(argv, g_strdup("128k"));
        g_ptr_array_add(argv, g_strdup("-f"));
        g_ptr_array_add(argv, g_strdup("opus"));
    } else if (g_ascii_strcasecmp(target_format, "mkv") == 0) {
        g_ptr_array_add(argv, g_strdup("-c"));
        g_ptr_array_add(argv, g_strdup("copy"));
        g_ptr_array_add(argv, g_strdup("-f"));
        g_ptr_array_add(argv, g_strdup("matroska"));
    } else if (g_ascii_strcasecmp(target_format, "avi") == 0) {
        g_ptr_array_add(argv, g_strdup("-c:v"));
        g_ptr_array_add(argv, g_strdup("mpeg4"));
        g_ptr_array_add(argv, g_strdup("-vtag"));
        g_ptr_array_add(argv, g_strdup("xvid"));
        g_ptr_array_add(argv, g_strdup("-qscale:v"));
        g_ptr_array_add(argv, g_strdup("3"));
        g_ptr_array_add(argv, g_strdup("-c:a"));
        g_ptr_array_add(argv, g_strdup("libmp3lame"));
        g_ptr_array_add(argv, g_strdup("-f"));
        g_ptr_array_add(argv, g_strdup("avi"));
    } else if (g_ascii_strcasecmp(target_format, "webm") == 0) {
        if (v_codec == AV_CODEC_ID_VP9 || v_codec == AV_CODEC_ID_VP8) {
            /* Smart Remuxing: Source is already VP9/VP8, copy video stream! */
            g_ptr_array_add(argv, g_strdup("-c:v"));
            g_ptr_array_add(argv, g_strdup("copy"));
            if (a_codec == AV_CODEC_ID_OPUS || a_codec == AV_CODEC_ID_VORBIS) {
                g_ptr_array_add(argv, g_strdup("-c:a"));
                g_ptr_array_add(argv, g_strdup("copy"));
            } else {
                g_ptr_array_add(argv, g_strdup("-c:a"));
                g_ptr_array_add(argv, g_strdup("libopus"));
                g_ptr_array_add(argv, g_strdup("-b:a"));
                g_ptr_array_add(argv, g_strdup("128k"));
            }
        } else {
            /* High-Performance Realtime Multi-Threaded VP9 Transcoding */
            g_ptr_array_add(argv, g_strdup("-c:v"));
            g_ptr_array_add(argv, g_strdup("libvpx-vp9"));
            g_ptr_array_add(argv, g_strdup("-deadline"));
            g_ptr_array_add(argv, g_strdup("realtime"));
            g_ptr_array_add(argv, g_strdup("-cpu-used"));
            g_ptr_array_add(argv, g_strdup("5"));
            g_ptr_array_add(argv, g_strdup("-row-mt"));
            g_ptr_array_add(argv, g_strdup("1"));
            g_ptr_array_add(argv, g_strdup("-crf"));
            g_ptr_array_add(argv, g_strdup("30"));
            g_ptr_array_add(argv, g_strdup("-b:v"));
            g_ptr_array_add(argv, g_strdup("0"));
            if (a_codec == AV_CODEC_ID_OPUS || a_codec == AV_CODEC_ID_VORBIS) {
                g_ptr_array_add(argv, g_strdup("-c:a"));
                g_ptr_array_add(argv, g_strdup("copy"));
            } else {
                g_ptr_array_add(argv, g_strdup("-c:a"));
                g_ptr_array_add(argv, g_strdup("libopus"));
                g_ptr_array_add(argv, g_strdup("-b:a"));
                g_ptr_array_add(argv, g_strdup("128k"));
            }
        }
        g_ptr_array_add(argv, g_strdup("-f"));
        g_ptr_array_add(argv, g_strdup("webm"));
    } else if (g_ascii_strcasecmp(target_format, "gif") == 0) {
        g_ptr_array_add(argv, g_strdup("-vf"));
        g_ptr_array_add(argv, g_strdup("fps=15,scale=480:-1:flags=lanczos"));
        g_ptr_array_add(argv, g_strdup("-f"));
        g_ptr_array_add(argv, g_strdup("gif"));
    } else if (g_ascii_strcasecmp(target_format, "comp-saver") == 0) {
        /* Max Space Saving: H.265 / HEVC (~70% Space Saving) */
        int src_kbps = (meta && meta->bitrate_bps > 0) ? (int)MIN(meta->bitrate_bps / 1000, (int64_t)G_MAXINT) : 2500;
        int target_kbps = (int)(src_kbps * 0.38);
        if (target_kbps > 1400) target_kbps = 1400;
        if (meta && meta->bitrate_bps > 0 && target_kbps >= (int)(src_kbps * 0.65)) {
            target_kbps = (int)(src_kbps * 0.65);
        }
        if (target_kbps < 50) target_kbps = 50;
        g_autofree char *b_str = g_strdup_printf("%dk", target_kbps);
        g_autofree char *max_str = g_strdup_printf("%dk", (int)(target_kbps * 1.2));
        g_autofree char *buf_str = g_strdup_printf("%dk", target_kbps * 2);

        if (gpu == TC_GPU_VAAPI) {
            double ratio = 1100.0 / (double)target_kbps;
            int qp = 30 + (int)round(5.5 * (log(ratio) / log(2.0)));
            if (qp < 28) qp = 28;
            if (qp > 44) qp = 44;
            g_autofree char *qp_str = g_strdup_printf("%d", qp);

            g_ptr_array_add(argv, g_strdup("-vaapi_device"));
            g_ptr_array_add(argv, g_strdup(detected_vaapi_node));
            g_ptr_array_add(argv, g_strdup("-vf"));
            g_ptr_array_add(argv, g_strdup("scale='trunc(min(1920,iw)/2)*2':-2,format=nv12,hwupload"));
            g_ptr_array_add(argv, g_strdup("-c:v"));
            g_ptr_array_add(argv, g_strdup("hevc_vaapi"));
            g_ptr_array_add(argv, g_strdup("-qp"));
            g_ptr_array_add(argv, g_strdup(qp_str));
            g_ptr_array_add(argv, g_strdup("-tag:v"));
            g_ptr_array_add(argv, g_strdup("hvc1"));
        } else if (gpu == TC_GPU_NVENC) {
            g_ptr_array_add(argv, g_strdup("-vf"));
            g_ptr_array_add(argv, g_strdup("scale='trunc(min(1920,iw)/2)*2':-2"));
            g_ptr_array_add(argv, g_strdup("-c:v"));
            g_ptr_array_add(argv, g_strdup("hevc_nvenc"));
            g_ptr_array_add(argv, g_strdup("-preset"));
            g_ptr_array_add(argv, g_strdup("p4"));
            g_ptr_array_add(argv, g_strdup("-rc"));
            g_ptr_array_add(argv, g_strdup("vbr"));
            g_ptr_array_add(argv, g_strdup("-b:v"));
            g_ptr_array_add(argv, g_strdup(b_str));
            g_ptr_array_add(argv, g_strdup("-maxrate"));
            g_ptr_array_add(argv, g_strdup(max_str));
            g_ptr_array_add(argv, g_strdup("-tag:v"));
            g_ptr_array_add(argv, g_strdup("hvc1"));
        } else {
            g_ptr_array_add(argv, g_strdup("-vf"));
            g_ptr_array_add(argv, g_strdup("scale='trunc(min(1920,iw)/2)*2':-2"));
            g_ptr_array_add(argv, g_strdup("-c:v"));
            g_ptr_array_add(argv, g_strdup("libx265"));
            g_ptr_array_add(argv, g_strdup("-preset"));
            g_ptr_array_add(argv, g_strdup("veryfast"));
            g_ptr_array_add(argv, g_strdup("-b:v"));
            g_ptr_array_add(argv, g_strdup(b_str));
            g_ptr_array_add(argv, g_strdup("-maxrate"));
            g_ptr_array_add(argv, g_strdup(max_str));
            g_ptr_array_add(argv, g_strdup("-bufsize"));
            g_ptr_array_add(argv, g_strdup(buf_str));
            g_ptr_array_add(argv, g_strdup("-tag:v"));
            g_ptr_array_add(argv, g_strdup("hvc1"));
        }

        if (a_codec == AV_CODEC_ID_AAC) {
            g_ptr_array_add(argv, g_strdup("-c:a"));
            g_ptr_array_add(argv, g_strdup("copy"));
        } else {
            g_ptr_array_add(argv, g_strdup("-c:a"));
            g_ptr_array_add(argv, g_strdup("aac"));
            g_ptr_array_add(argv, g_strdup("-b:a"));
            g_ptr_array_add(argv, g_strdup("128k"));
        }
        g_ptr_array_add(argv, g_strdup("-movflags"));
        g_ptr_array_add(argv, g_strdup("+faststart"));
        g_ptr_array_add(argv, g_strdup("-f"));
        g_ptr_array_add(argv, g_strdup("mp4"));
    } else if (g_ascii_strcasecmp(target_format, "comp-web") == 0) {
        /* Web/Discord Optimized: H.264 (720p HD, Adaptive Bitrate) */
        int src_kbps = (meta && meta->bitrate_bps > 0) ? (int)MIN(meta->bitrate_bps / 1000, (int64_t)G_MAXINT) : 2500;
        int target_kbps = (int)(src_kbps * 0.55);
        if (target_kbps > 1800) target_kbps = 1800;
        if (meta && meta->bitrate_bps > 0 && target_kbps >= (int)(src_kbps * 0.75)) {
            target_kbps = (int)(src_kbps * 0.75);
        }
        if (target_kbps < 60) target_kbps = 60;
        g_autofree char *b_str = g_strdup_printf("%dk", target_kbps);
        g_autofree char *max_str = g_strdup_printf("%dk", (int)(target_kbps * 1.2));
        g_autofree char *buf_str = g_strdup_printf("%dk", target_kbps * 2);

        if (gpu == TC_GPU_VAAPI) {
            double ratio = 1600.0 / (double)target_kbps;
            int qp = 28 + (int)round(5.5 * (log(ratio) / log(2.0)));
            if (qp < 26) qp = 26;
            if (qp > 42) qp = 42;
            g_autofree char *qp_str = g_strdup_printf("%d", qp);

            g_ptr_array_add(argv, g_strdup("-vaapi_device"));
            g_ptr_array_add(argv, g_strdup(detected_vaapi_node));
            g_ptr_array_add(argv, g_strdup("-vf"));
            g_ptr_array_add(argv, g_strdup("scale='trunc(min(1280,iw)/2)*2':-2,format=nv12,hwupload"));
            g_ptr_array_add(argv, g_strdup("-c:v"));
            g_ptr_array_add(argv, g_strdup("h264_vaapi"));
            g_ptr_array_add(argv, g_strdup("-qp"));
            g_ptr_array_add(argv, g_strdup(qp_str));
        } else if (gpu == TC_GPU_NVENC) {
            g_ptr_array_add(argv, g_strdup("-vf"));
            g_ptr_array_add(argv, g_strdup("scale='trunc(min(1280,iw)/2)*2':-2"));
            g_ptr_array_add(argv, g_strdup("-c:v"));
            g_ptr_array_add(argv, g_strdup("h264_nvenc"));
            g_ptr_array_add(argv, g_strdup("-preset"));
            g_ptr_array_add(argv, g_strdup("p4"));
            g_ptr_array_add(argv, g_strdup("-rc"));
            g_ptr_array_add(argv, g_strdup("vbr"));
            g_ptr_array_add(argv, g_strdup("-b:v"));
            g_ptr_array_add(argv, g_strdup(b_str));
            g_ptr_array_add(argv, g_strdup("-maxrate"));
            g_ptr_array_add(argv, g_strdup(max_str));
        } else {
            g_ptr_array_add(argv, g_strdup("-vf"));
            g_ptr_array_add(argv, g_strdup("scale='trunc(min(1280,iw)/2)*2':-2"));
            g_ptr_array_add(argv, g_strdup("-c:v"));
            g_ptr_array_add(argv, g_strdup("libx264"));
            g_ptr_array_add(argv, g_strdup("-preset"));
            g_ptr_array_add(argv, g_strdup("veryfast"));
            g_ptr_array_add(argv, g_strdup("-b:v"));
            g_ptr_array_add(argv, g_strdup(b_str));
            g_ptr_array_add(argv, g_strdup("-maxrate"));
            g_ptr_array_add(argv, g_strdup(max_str));
            g_ptr_array_add(argv, g_strdup("-bufsize"));
            g_ptr_array_add(argv, g_strdup(buf_str));
            g_ptr_array_add(argv, g_strdup("-pix_fmt"));
            g_ptr_array_add(argv, g_strdup("yuv420p"));
        }

        if (a_codec == AV_CODEC_ID_AAC) {
            g_ptr_array_add(argv, g_strdup("-c:a"));
            g_ptr_array_add(argv, g_strdup("copy"));
        } else {
            g_ptr_array_add(argv, g_strdup("-c:a"));
            g_ptr_array_add(argv, g_strdup("aac"));
            g_ptr_array_add(argv, g_strdup("-b:a"));
            g_ptr_array_add(argv, g_strdup("128k"));
        }
        g_ptr_array_add(argv, g_strdup("-movflags"));
        g_ptr_array_add(argv, g_strdup("+faststart"));
        g_ptr_array_add(argv, g_strdup("-f"));
        g_ptr_array_add(argv, g_strdup("mp4"));
    } else if (g_ascii_strcasecmp(target_format, "comp-hq") == 0) {
        /* High Quality: (1080p FHD, Adaptive Rate, Preserves AV1/HEVC density) */
        int src_kbps = (meta && meta->bitrate_bps > 0) ? (int)MIN(meta->bitrate_bps / 1000, (int64_t)G_MAXINT) : 3500;
        int target_kbps = (int)(src_kbps * 0.68);
        if (target_kbps > 3200) target_kbps = 3200;
        if (meta && meta->bitrate_bps > 0 && target_kbps >= (int)(src_kbps * 0.82)) {
            target_kbps = (int)(src_kbps * 0.82);
        }
        if (target_kbps < 60) target_kbps = 60;
        g_autofree char *b_str = g_strdup_printf("%dk", target_kbps);
        g_autofree char *max_str = g_strdup_printf("%dk", (int)(target_kbps * 1.2));
        g_autofree char *buf_str = g_strdup_printf("%dk", target_kbps * 2);

        /* If input is already modern high-efficiency codec (AV1 or HEVC), using H.264
         * would require ~2.5x higher bitrate and bloat the file! Must use HEVC! */
        bool use_hevc = (v_codec == AV_CODEC_ID_AV1 || v_codec == AV_CODEC_ID_HEVC);

        if (use_hevc) {
            if (gpu == TC_GPU_VAAPI) {
                double ratio = 1500.0 / (double)target_kbps;
                int qp = 27 + (int)round(5.5 * (log(ratio) / log(2.0)));
                if (qp < 24) qp = 24;
                if (qp > 42) qp = 42;
                g_autofree char *qp_str = g_strdup_printf("%d", qp);

                g_ptr_array_add(argv, g_strdup("-vaapi_device"));
                g_ptr_array_add(argv, g_strdup(detected_vaapi_node));
                g_ptr_array_add(argv, g_strdup("-vf"));
                g_ptr_array_add(argv, g_strdup("scale='trunc(min(1920,iw)/2)*2':-2,format=nv12,hwupload"));
                g_ptr_array_add(argv, g_strdup("-c:v"));
                g_ptr_array_add(argv, g_strdup("hevc_vaapi"));
                g_ptr_array_add(argv, g_strdup("-qp"));
                g_ptr_array_add(argv, g_strdup(qp_str));
                g_ptr_array_add(argv, g_strdup("-tag:v"));
                g_ptr_array_add(argv, g_strdup("hvc1"));
            } else if (gpu == TC_GPU_NVENC) {
                g_ptr_array_add(argv, g_strdup("-vf"));
                g_ptr_array_add(argv, g_strdup("scale='trunc(min(1920,iw)/2)*2':-2"));
                g_ptr_array_add(argv, g_strdup("-c:v"));
                g_ptr_array_add(argv, g_strdup("hevc_nvenc"));
                g_ptr_array_add(argv, g_strdup("-preset"));
                g_ptr_array_add(argv, g_strdup("p4"));
                g_ptr_array_add(argv, g_strdup("-rc"));
                g_ptr_array_add(argv, g_strdup("vbr"));
                g_ptr_array_add(argv, g_strdup("-b:v"));
                g_ptr_array_add(argv, g_strdup(b_str));
                g_ptr_array_add(argv, g_strdup("-maxrate"));
                g_ptr_array_add(argv, g_strdup(max_str));
                g_ptr_array_add(argv, g_strdup("-tag:v"));
                g_ptr_array_add(argv, g_strdup("hvc1"));
            } else {
                g_ptr_array_add(argv, g_strdup("-vf"));
                g_ptr_array_add(argv, g_strdup("scale='trunc(min(1920,iw)/2)*2':-2"));
                g_ptr_array_add(argv, g_strdup("-c:v"));
                g_ptr_array_add(argv, g_strdup("libx265"));
                g_ptr_array_add(argv, g_strdup("-preset"));
                g_ptr_array_add(argv, g_strdup("veryfast"));
                g_ptr_array_add(argv, g_strdup("-b:v"));
                g_ptr_array_add(argv, g_strdup(b_str));
                g_ptr_array_add(argv, g_strdup("-maxrate"));
                g_ptr_array_add(argv, g_strdup(max_str));
                g_ptr_array_add(argv, g_strdup("-bufsize"));
                g_ptr_array_add(argv, g_strdup(buf_str));
                g_ptr_array_add(argv, g_strdup("-tag:v"));
                g_ptr_array_add(argv, g_strdup("hvc1"));
            }
        } else {
            /* Standard H.264 high quality */
            if (gpu == TC_GPU_VAAPI) {
                double ratio = 3400.0 / (double)target_kbps;
                int qp = 26 + (int)round(5.5 * (log(ratio) / log(2.0)));
                if (qp < 22) qp = 22;
                if (qp > 42) qp = 42;
                g_autofree char *qp_str = g_strdup_printf("%d", qp);

                g_ptr_array_add(argv, g_strdup("-vaapi_device"));
                g_ptr_array_add(argv, g_strdup(detected_vaapi_node));
                g_ptr_array_add(argv, g_strdup("-vf"));
                g_ptr_array_add(argv, g_strdup("scale='trunc(min(1920,iw)/2)*2':-2,format=nv12,hwupload"));
                g_ptr_array_add(argv, g_strdup("-c:v"));
                g_ptr_array_add(argv, g_strdup("h264_vaapi"));
                g_ptr_array_add(argv, g_strdup("-qp"));
                g_ptr_array_add(argv, g_strdup(qp_str));
            } else if (gpu == TC_GPU_NVENC) {
                g_ptr_array_add(argv, g_strdup("-vf"));
                g_ptr_array_add(argv, g_strdup("scale='trunc(min(1920,iw)/2)*2':-2"));
                g_ptr_array_add(argv, g_strdup("-c:v"));
                g_ptr_array_add(argv, g_strdup("h264_nvenc"));
                g_ptr_array_add(argv, g_strdup("-preset"));
                g_ptr_array_add(argv, g_strdup("p4"));
                g_ptr_array_add(argv, g_strdup("-rc"));
                g_ptr_array_add(argv, g_strdup("vbr"));
                g_ptr_array_add(argv, g_strdup("-b:v"));
                g_ptr_array_add(argv, g_strdup(b_str));
                g_ptr_array_add(argv, g_strdup("-maxrate"));
                g_ptr_array_add(argv, g_strdup(max_str));
            } else {
                g_ptr_array_add(argv, g_strdup("-vf"));
                g_ptr_array_add(argv, g_strdup("scale='trunc(min(1920,iw)/2)*2':-2"));
                g_ptr_array_add(argv, g_strdup("-c:v"));
                g_ptr_array_add(argv, g_strdup("libx264"));
                g_ptr_array_add(argv, g_strdup("-preset"));
                g_ptr_array_add(argv, g_strdup("veryfast"));
                g_ptr_array_add(argv, g_strdup("-b:v"));
                g_ptr_array_add(argv, g_strdup(b_str));
                g_ptr_array_add(argv, g_strdup("-maxrate"));
                g_ptr_array_add(argv, g_strdup(max_str));
                g_ptr_array_add(argv, g_strdup("-bufsize"));
                g_ptr_array_add(argv, g_strdup(buf_str));
                g_ptr_array_add(argv, g_strdup("-pix_fmt"));
                g_ptr_array_add(argv, g_strdup("yuv420p"));
            }
        }

        if (a_codec == AV_CODEC_ID_AAC) {
            g_ptr_array_add(argv, g_strdup("-c:a"));
            g_ptr_array_add(argv, g_strdup("copy"));
        } else {
            g_ptr_array_add(argv, g_strdup("-c:a"));
            g_ptr_array_add(argv, g_strdup("aac"));
            g_ptr_array_add(argv, g_strdup("-b:a"));
            g_ptr_array_add(argv, g_strdup("160k"));
        }
        g_ptr_array_add(argv, g_strdup("-movflags"));
        g_ptr_array_add(argv, g_strdup("+faststart"));
        g_ptr_array_add(argv, g_strdup("-f"));
        g_ptr_array_add(argv, g_strdup("mp4"));
    } else if (g_ascii_strcasecmp(target_format, "mov") == 0) {
        /* MOV Conversion */
        if (v_codec == AV_CODEC_ID_H264 || v_codec == AV_CODEC_ID_HEVC || v_codec == AV_CODEC_ID_PRORES) {
            g_ptr_array_add(argv, g_strdup("-c:v"));
            g_ptr_array_add(argv, g_strdup("copy"));
            if (a_codec == AV_CODEC_ID_AAC || a_codec == AV_CODEC_ID_PCM_S16LE) {
                g_ptr_array_add(argv, g_strdup("-c:a"));
                g_ptr_array_add(argv, g_strdup("copy"));
            } else {
                g_ptr_array_add(argv, g_strdup("-c:a"));
                g_ptr_array_add(argv, g_strdup("aac"));
                g_ptr_array_add(argv, g_strdup("-b:a"));
                g_ptr_array_add(argv, g_strdup("160k"));
            }
        } else if (gpu == TC_GPU_VAAPI) {
            g_ptr_array_add(argv, g_strdup("-vaapi_device"));
            g_ptr_array_add(argv, g_strdup(detected_vaapi_node));
            g_ptr_array_add(argv, g_strdup("-async_depth"));
            g_ptr_array_add(argv, g_strdup("4"));
            g_ptr_array_add(argv, g_strdup("-vf"));
            g_ptr_array_add(argv, g_strdup("scale='trunc(iw/2)*2:trunc(ih/2)*2',format=nv12,hwupload"));
            g_ptr_array_add(argv, g_strdup("-c:v"));
            g_ptr_array_add(argv, g_strdup("h264_vaapi"));
            g_ptr_array_add(argv, g_strdup("-qp"));
            g_ptr_array_add(argv, g_strdup("23"));
            if (a_codec == AV_CODEC_ID_AAC || a_codec == AV_CODEC_ID_PCM_S16LE) {
                g_ptr_array_add(argv, g_strdup("-c:a"));
                g_ptr_array_add(argv, g_strdup("copy"));
            } else {
                g_ptr_array_add(argv, g_strdup("-c:a"));
                g_ptr_array_add(argv, g_strdup("aac"));
                g_ptr_array_add(argv, g_strdup("-b:a"));
                g_ptr_array_add(argv, g_strdup("160k"));
            }
        } else {
            g_ptr_array_add(argv, g_strdup("-c:v"));
            g_ptr_array_add(argv, g_strdup("libx264"));
            g_ptr_array_add(argv, g_strdup("-preset"));
            g_ptr_array_add(argv, g_strdup("veryfast"));
            g_ptr_array_add(argv, g_strdup("-crf"));
            g_ptr_array_add(argv, g_strdup("23"));
            g_ptr_array_add(argv, g_strdup("-pix_fmt"));
            g_ptr_array_add(argv, g_strdup("yuv420p"));
            if (a_codec == AV_CODEC_ID_AAC || a_codec == AV_CODEC_ID_PCM_S16LE) {
                g_ptr_array_add(argv, g_strdup("-c:a"));
                g_ptr_array_add(argv, g_strdup("copy"));
            } else {
                g_ptr_array_add(argv, g_strdup("-c:a"));
                g_ptr_array_add(argv, g_strdup("aac"));
                g_ptr_array_add(argv, g_strdup("-b:a"));
                g_ptr_array_add(argv, g_strdup("160k"));
            }
        }
        g_ptr_array_add(argv, g_strdup("-movflags"));
        g_ptr_array_add(argv, g_strdup("+faststart"));
        g_ptr_array_add(argv, g_strdup("-f"));
        g_ptr_array_add(argv, g_strdup("mov"));
    } else {
        /* Default MP4 conversion */
        /* Smart Remuxing: If source video is already H.264 or HEVC, copy stream! */
        if (v_codec == AV_CODEC_ID_H264 || v_codec == AV_CODEC_ID_HEVC) {
            g_ptr_array_add(argv, g_strdup("-c:v"));
            g_ptr_array_add(argv, g_strdup("copy"));
            if (a_codec == AV_CODEC_ID_AAC || a_codec == AV_CODEC_ID_MP3) {
                g_ptr_array_add(argv, g_strdup("-c:a"));
                g_ptr_array_add(argv, g_strdup("copy"));
            } else {
                g_ptr_array_add(argv, g_strdup("-c:a"));
                g_ptr_array_add(argv, g_strdup("aac"));
                g_ptr_array_add(argv, g_strdup("-b:a"));
                g_ptr_array_add(argv, g_strdup("160k"));
            }
        } else if (gpu == TC_GPU_VAAPI) {
            g_ptr_array_add(argv, g_strdup("-vaapi_device"));
            g_ptr_array_add(argv, g_strdup(detected_vaapi_node));
            g_ptr_array_add(argv, g_strdup("-async_depth"));
            g_ptr_array_add(argv, g_strdup("4"));
            g_ptr_array_add(argv, g_strdup("-vf"));
            g_ptr_array_add(argv, g_strdup("scale='trunc(iw/2)*2:trunc(ih/2)*2',format=nv12,hwupload"));
            g_ptr_array_add(argv, g_strdup("-c:v"));
            g_ptr_array_add(argv, g_strdup("h264_vaapi"));
            g_ptr_array_add(argv, g_strdup("-qp"));
            g_ptr_array_add(argv, g_strdup("23"));
            if (a_codec == AV_CODEC_ID_AAC || a_codec == AV_CODEC_ID_MP3) {
                g_ptr_array_add(argv, g_strdup("-c:a"));
                g_ptr_array_add(argv, g_strdup("copy"));
            } else {
                g_ptr_array_add(argv, g_strdup("-c:a"));
                g_ptr_array_add(argv, g_strdup("aac"));
                g_ptr_array_add(argv, g_strdup("-b:a"));
                g_ptr_array_add(argv, g_strdup("128k"));
            }
        } else if (gpu == TC_GPU_NVENC) {
            g_ptr_array_add(argv, g_strdup("-c:v"));
            g_ptr_array_add(argv, g_strdup("h264_nvenc"));
            g_ptr_array_add(argv, g_strdup("-preset"));
            g_ptr_array_add(argv, g_strdup("p4"));
            g_ptr_array_add(argv, g_strdup("-cq"));
            g_ptr_array_add(argv, g_strdup("23"));
            if (a_codec == AV_CODEC_ID_AAC || a_codec == AV_CODEC_ID_MP3) {
                g_ptr_array_add(argv, g_strdup("-c:a"));
                g_ptr_array_add(argv, g_strdup("copy"));
            } else {
                g_ptr_array_add(argv, g_strdup("-c:a"));
                g_ptr_array_add(argv, g_strdup("aac"));
                g_ptr_array_add(argv, g_strdup("-b:a"));
                g_ptr_array_add(argv, g_strdup("128k"));
            }
        } else {
            g_ptr_array_add(argv, g_strdup("-c:v"));
            g_ptr_array_add(argv, g_strdup("libx264"));
            g_ptr_array_add(argv, g_strdup("-preset"));
            g_ptr_array_add(argv, g_strdup("veryfast"));
            g_ptr_array_add(argv, g_strdup("-crf"));
            g_ptr_array_add(argv, g_strdup("23"));
            g_ptr_array_add(argv, g_strdup("-pix_fmt"));
            g_ptr_array_add(argv, g_strdup("yuv420p"));
            if (a_codec == AV_CODEC_ID_AAC || a_codec == AV_CODEC_ID_MP3) {
                g_ptr_array_add(argv, g_strdup("-c:a"));
                g_ptr_array_add(argv, g_strdup("copy"));
            } else {
                g_ptr_array_add(argv, g_strdup("-c:a"));
                g_ptr_array_add(argv, g_strdup("aac"));
                g_ptr_array_add(argv, g_strdup("-b:a"));
                g_ptr_array_add(argv, g_strdup("128k"));
            }
        }
        g_ptr_array_add(argv, g_strdup("-movflags"));
        g_ptr_array_add(argv, g_strdup("+faststart"));
        g_ptr_array_add(argv, g_strdup("-f"));
        g_ptr_array_add(argv, g_strdup("mp4"));
    }

    /* libx265's worker pool is independent of FFmpeg's generic threads option. */
    for (guint i = 0; i + 1 < argv->len; i++) {
        if (!strcmp(argv->pdata[i], "-c:v") && !strcmp(argv->pdata[i + 1], "libx265")) {
            g_ptr_array_add(argv, g_strdup("-x265-params"));
            g_ptr_array_add(argv, g_strdup(tc_engine_thread_limit() > 1 ?
                "pools=1:frame-threads=1" : "pools=none:frame-threads=1"));
            break;
        }
    }
    g_ptr_array_add(argv, g_strdup(dst_path));
    g_ptr_array_add(argv, NULL);
    return argv;
}

