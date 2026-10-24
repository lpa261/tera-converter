#ifndef TC_MEDIA_PROBE_H
#define TC_MEDIA_PROBE_H

#include "tc_common.h"
#include <libavformat/avformat.h>
#include <libavcodec/codec_par.h>

typedef struct {
    enum AVPacketSideDataType type;
    GBytes *bytes;
} TcMediaSideData;

typedef struct {
    int index, id, disposition;
    AVCodecParameters *codec;   /* Owned deep copy, including extradata. */
    AVDictionary *metadata;
    GPtrArray *side_data;       /* TcMediaSideData, including unknown types. */
    AVRational time_base, average_frame_rate, guessed_frame_rate, sample_aspect_ratio;
    int64_t start_time, duration, frame_count; /* Native stream time base. */
    guint bit_depth;            /* 0 means unknown. */
    gboolean parameters_known;
    gboolean has_display_matrix, has_hdr_metadata;
    double rotation_degrees;   /* NAN unless a valid display matrix is present. */
} TcMediaStream;

typedef struct {
    int64_t id, start, end;
    AVRational time_base;
    AVDictionary *metadata;
} TcMediaChapter;

typedef struct {
    char *format_name;
    AVDictionary *metadata;
    GPtrArray *streams;         /* Owned TcMediaStream, in input order. */
    GPtrArray *chapters;        /* Owned TcMediaChapter. */
    guint program_count, stream_group_count;
    int64_t duration_us, start_time_us, bitrate_bps;
    struct stat source_stat;
} TcMediaInfo;

typedef struct {
    gint64 timeout_ms;          /* Cooperative I/O deadline, not CPU preemption. */
    guint64 max_read_bytes;
} TcMediaProbeLimits;

#define TC_MEDIA_PROBE_DEFAULT_LIMITS { 15000, 64U * 1024U * 1024U }

/* Only local regular files are accepted. Demuxer secondary opens are denied.
 * Returns an owned snapshot independent of libavformat's input context. Unknown
 * properties remain unknown: a successful probe is NOT a preservation validator
 * or proof of CFR, complete packet integrity, or complete container metadata. */
TcStatusCode tc_media_probe_file(const char *path, const TcMediaProbeLimits *limits,
                                  GCancellable *cancellable, TcMediaInfo **out,
                                  char **error_out);
void tc_media_info_free(TcMediaInfo *info);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(TcMediaInfo, tc_media_info_free)

TcStatusCode tc_media_packet_manifest(const char *path,GCancellable *cancel,GPtrArray **out,char **error);
#endif
