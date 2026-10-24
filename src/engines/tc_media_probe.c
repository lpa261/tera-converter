#include "tc_media_probe.h"
#include "tc_runtime_limits.h"

#include <math.h>
#include <libavutil/mathematics.h>
#include <libavcodec/avcodec.h>
#include <libavutil/display.h>
#include <libavutil/pixdesc.h>

typedef struct {
    int fd;
    int64_t size;
    gint64 started_us;
    TcMediaProbeLimits limits;
    GCancellable *cancellable;  /* Borrowed for this synchronous call. */
    guint64 read_bytes;
    gboolean read_limit_hit, secondary_open_denied;
} ProbeInput;

static int interrupted(void *opaque) {
    ProbeInput *input = opaque;
    return (input->cancellable && g_cancellable_is_cancelled(input->cancellable)) ||
           g_get_monotonic_time() - input->started_us >= input->limits.timeout_ms * 1000;
}

static int read_input(void *opaque, uint8_t *buffer, int size) {
    ProbeInput *input = opaque;
    if (size <= 0) return AVERROR(EINVAL);
    if (input->read_bytes >= input->limits.max_read_bytes) {
        input->read_limit_hit = TRUE;
        return AVERROR(ENOSPC);
    }
    size = (int)MIN((guint64)size, input->limits.max_read_bytes - input->read_bytes);
    ssize_t count;
    do {
        if (interrupted(input)) return AVERROR_EXIT;
        count = read(input->fd, buffer, (size_t)size);
    } while (count < 0 && errno == EINTR);
    if (count < 0) return AVERROR(errno);
    if (count == 0) return AVERROR_EOF;
    input->read_bytes += (guint64)count;
    return (int)count;
}

static int64_t seek_input(void *opaque, int64_t offset, int whence) {
    ProbeInput *input = opaque;
    if (interrupted(input)) return AVERROR_EXIT;
    if (whence & AVSEEK_SIZE) return input->size;
    whence &= ~AVSEEK_FORCE;
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) return AVERROR(EINVAL);
    off_t position = lseek(input->fd, offset, whence);
    return position < 0 ? AVERROR(errno) : position;
}

static int deny_secondary_open(AVFormatContext *context, AVIOContext **pb,
                               const char *url, int flags, AVDictionary **options) {
    (void)pb; (void)url; (void)flags; (void)options;
    ProbeInput *input = context->opaque;
    if (input) input->secondary_open_denied = TRUE;
    return AVERROR(EPERM);
}

static void side_data_free(gpointer pointer) {
    TcMediaSideData *side = pointer;
    g_bytes_unref(side->bytes);
    g_free(side);
}

static void stream_free(gpointer pointer) {
    TcMediaStream *stream = pointer;
    avcodec_parameters_free(&stream->codec);
    av_dict_free(&stream->metadata);
    g_ptr_array_unref(stream->side_data);
    g_free(stream);
}

static void chapter_free(gpointer pointer) {
    TcMediaChapter *chapter = pointer;
    av_dict_free(&chapter->metadata);
    g_free(chapter);
}

void tc_media_info_free(TcMediaInfo *info) {
    if (!info) return;
    g_free(info->format_name);
    av_dict_free(&info->metadata);
    g_ptr_array_unref(info->streams);
    g_ptr_array_unref(info->chapters);
    g_free(info);
}

static int copy_stream(TcMediaInfo *info, const AVStream *source) {
    TcMediaStream *stream = g_new0(TcMediaStream, 1);
    stream->side_data = g_ptr_array_new_with_free_func(side_data_free);
    g_ptr_array_add(info->streams, stream);
    stream->codec = avcodec_parameters_alloc();
    if (!stream->codec) return AVERROR(ENOMEM);
    int status = avcodec_parameters_copy(stream->codec, source->codecpar);
    if (status < 0) return status;
    if ((status = av_dict_copy(&stream->metadata, source->metadata, 0)) < 0) return status;
    stream->index = source->index;
    stream->id = source->id;
    stream->disposition = source->disposition;
    stream->time_base = source->time_base;
    stream->average_frame_rate = source->avg_frame_rate;
    stream->guessed_frame_rate = source->r_frame_rate;
    stream->sample_aspect_ratio = source->sample_aspect_ratio;
    stream->start_time = source->start_time;
    stream->duration = source->duration;
    stream->frame_count = source->nb_frames;
    stream->rotation_degrees = NAN;

#if LIBAVCODEC_VERSION_MAJOR >= 61
    const AVPacketSideData *sides = source->codecpar->coded_side_data;
    int side_count = source->codecpar->nb_coded_side_data;
#else
    const AVPacketSideData *sides = source->side_data;
    int side_count = source->nb_side_data;
#endif
    for (int i = 0; i < side_count; i++) {
        TcMediaSideData *side = g_new0(TcMediaSideData, 1);
        side->type = sides[i].type;
        side->bytes = g_bytes_new(sides[i].data, sides[i].size);
        g_ptr_array_add(stream->side_data, side);
        if (side->type == AV_PKT_DATA_DISPLAYMATRIX) {
            stream->has_display_matrix = TRUE;
            if (sides[i].size == 9 * sizeof(int32_t)) {
                /* The bytes need not have int32_t alignment. */
                int32_t matrix[9];
                memcpy(matrix, sides[i].data, sizeof(matrix));
                stream->rotation_degrees = av_display_rotation_get(matrix);
            }
        }
        if (side->type == AV_PKT_DATA_MASTERING_DISPLAY_METADATA ||
            side->type == AV_PKT_DATA_CONTENT_LIGHT_LEVEL || side->type == AV_PKT_DATA_DOVI_CONF ||
            side->type == AV_PKT_DATA_DYNAMIC_HDR10_PLUS) stream->has_hdr_metadata = TRUE;
    }

    AVCodecParameters *codec = stream->codec;
    gboolean identified = codec->codec_id != AV_CODEC_ID_NONE &&
                          stream->time_base.num > 0 && stream->time_base.den > 0;
    if (codec->codec_type == AVMEDIA_TYPE_VIDEO) {
        const AVPixFmtDescriptor *pixel = av_pix_fmt_desc_get(codec->format);
        if (pixel) {
            for (int c = 0; c < pixel->nb_components; c++)
                stream->bit_depth = MAX(stream->bit_depth, (guint)pixel->comp[c].depth);
        }
        stream->parameters_known = identified && codec->width > 0 && codec->height > 0 && pixel;
    } else if (codec->codec_type == AVMEDIA_TYPE_AUDIO) {
#if LIBAVCODEC_VERSION_MAJOR >= 59
        int channels = codec->ch_layout.nb_channels;
#else
        int channels = codec->channels;
#endif
        stream->bit_depth = MAX(codec->bits_per_raw_sample, 0);
        stream->parameters_known = identified && codec->sample_rate > 0 && channels > 0;
    } else {
        stream->parameters_known = identified;
    }
    return 0;
}

static gboolean same_file(const struct stat *left, const struct stat *right) {
    return left->st_dev == right->st_dev && left->st_ino == right->st_ino &&
           left->st_size == right->st_size &&
           left->st_mtim.tv_sec == right->st_mtim.tv_sec && left->st_mtim.tv_nsec == right->st_mtim.tv_nsec &&
           left->st_ctim.tv_sec == right->st_ctim.tv_sec && left->st_ctim.tv_nsec == right->st_ctim.tv_nsec;
}

TcStatusCode tc_media_probe_file(const char *path, const TcMediaProbeLimits *limits,
                                  GCancellable *cancellable, TcMediaInfo **out,
                                  char **error_out) {
    const TcMediaProbeLimits defaults = TC_MEDIA_PROBE_DEFAULT_LIMITS;
    if (out) *out = NULL;
    if (!limits) limits = &defaults;
    if (!path || !out || limits->timeout_ms <= 0 || limits->timeout_ms > G_MAXINT64 / 1000 ||
        limits->max_read_bytes == 0) {
        if (error_out) *error_out = g_strdup("Invalid media probe arguments or limits");
        return TC_STATUS_ERROR;
    }
    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
        if (error_out) *error_out = g_strdup("Media probe cancelled");
        return TC_STATUS_ERROR_CANCELLED;
    }

    ProbeInput input = { .fd = -1, .limits = *limits, .cancellable = cancellable,
                         .started_us = g_get_monotonic_time() };
    AVFormatContext *format = NULL;
    AVIOContext *io = NULL;
    AVDictionary *options = NULL;
    AVDictionary **decoder_options = NULL;
    guint decoder_count = 0;
    g_autoptr(TcMediaInfo) info = g_new0(TcMediaInfo, 1);
    info->streams = g_ptr_array_new_with_free_func(stream_free);
    info->chapters = g_ptr_array_new_with_free_func(chapter_free);
    TcStatusCode outcome = TC_STATUS_ERROR;
    int status = 0;
    const char *failure = NULL;

    /* O_NONBLOCK prevents a FIFO path from blocking before fstat can reject it. */
    input.fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (input.fd < 0 || fstat(input.fd, &info->source_stat) != 0) {
        status = AVERROR(errno);
        failure = "Cannot open media source";
        goto done;
    }
    if (!S_ISREG(info->source_stat.st_mode) || info->source_stat.st_size <= 0) {
        outcome = TC_STATUS_ERROR_UNSUPPORTED;
        failure = "Media probe requires a nonempty local regular file";
        goto done;
    }
    input.size = info->source_stat.st_size;
    format = avformat_alloc_context();
    uint8_t *buffer = av_malloc(32768);
    if (!format || !buffer) {
        av_free(buffer);
        status = AVERROR(ENOMEM);
        goto done;
    }
    io = avio_alloc_context(buffer, 32768, 0, &input, read_input, NULL, seek_input);
    if (!io) {
        av_free(buffer);
        status = AVERROR(ENOMEM);
        goto done;
    }
    format->pb = io;
    format->flags |= AVFMT_FLAG_CUSTOM_IO;
    format->interrupt_callback = (AVIOInterruptCB){ interrupted, &input };
    format->opaque = &input;
    format->io_open = deny_secondary_open;
    format->max_streams = 128;
    format->probesize = 16 * 1024 * 1024;
    format->max_analyze_duration = 5 * AV_TIME_BASE;
    format->max_index_size = 1024 * 1024;
    status = av_dict_set(&options, "format_whitelist", "mov,matroska,webm,avi,mp3,wav,flac,aac,ogg,gif,mpeg,mpegts,asf,flv,nut,aiff,ape,ac3,eac3,mxf", 0);
    if (status < 0) goto done;
    status = avformat_open_input(&format, NULL, NULL, &options);
    if (status >= 0) {
        decoder_count = format->nb_streams;
        decoder_options = av_calloc(MAX(decoder_count, 1U), sizeof(*decoder_options));
        if (!decoder_options) { status = AVERROR(ENOMEM); goto done; }
        const char *thread_count = tc_engine_thread_limit() > 1 ? "2" : "1";
        for (guint i = 0; i < decoder_count; i++) {
            status = av_dict_set(&decoder_options[i], "threads", thread_count, 0);
            if (status < 0) goto done;
        }
        status = avformat_find_stream_info(format, decoder_options);
    }
    if (status < 0 || input.read_limit_hit || input.secondary_open_denied || interrupted(&input)) goto done;
    if (format->nb_streams == 0 || format->nb_chapters > 4096) {
        outcome = TC_STATUS_ERROR_UNSUPPORTED;
        failure = "No media streams or chapter count exceeds probe limit";
        goto done;
    }

    info->format_name = g_strdup(format->iformat->name);
    info->duration_us = format->duration;
    info->start_time_us = format->start_time;
    info->bitrate_bps = format->bit_rate;
    info->program_count = format->nb_programs;
#if LIBAVFORMAT_VERSION_MAJOR >= 61
    info->stream_group_count = format->nb_stream_groups;
#endif
    if ((status = av_dict_copy(&info->metadata, format->metadata, 0)) < 0) goto done;
    for (guint i = 0; i < format->nb_streams; i++) {
        if ((status = copy_stream(info, format->streams[i])) < 0) goto done;
    }
    for (guint i = 0; i < format->nb_chapters; i++) {
        const AVChapter *source = format->chapters[i];
        TcMediaChapter *chapter = g_new0(TcMediaChapter, 1);
        g_ptr_array_add(info->chapters, chapter);
        chapter->id = source->id;
        chapter->start = source->start;
        chapter->end = source->end;
        chapter->time_base = source->time_base;
        if ((status = av_dict_copy(&chapter->metadata, source->metadata, 0)) < 0) goto done;
    }
    struct stat after, named;
    if (fstat(input.fd, &after) != 0 || stat(path, &named) != 0 ||
        !same_file(&info->source_stat, &after) || !same_file(&info->source_stat, &named)) {
        failure = "Source changed while reading media information";
        goto done;
    }
    if (!interrupted(&input)) outcome = TC_STATUS_OK;

done:
    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
        outcome = TC_STATUS_ERROR_CANCELLED;
        failure = "Media probe cancelled";
    } else if (g_get_monotonic_time() - input.started_us >= limits->timeout_ms * 1000) {
        outcome = TC_STATUS_ERROR_TIMEOUT;
        failure = "Media probe exceeded its I/O time limit";
    } else if (input.read_limit_hit || input.secondary_open_denied) {
        outcome = TC_STATUS_ERROR_UNSUPPORTED;
        failure = input.read_limit_hit ? "Media probe exceeded its read budget" : "External media references are unsupported";
    } else if (status < 0 && !failure && outcome == TC_STATUS_ERROR && status != AVERROR(ENOMEM)) {
        outcome = TC_STATUS_ERROR_CORRUPT;
    }
    av_dict_free(&options);
    if (decoder_options)
        for (guint i = 0; i < decoder_count; i++) av_dict_free(&decoder_options[i]);
    av_free(decoder_options);
    avformat_close_input(&format);
    if (io) av_freep(&io->buffer);
    avio_context_free(&io);
    if (input.fd >= 0) close(input.fd);
    if (outcome == TC_STATUS_OK) {
        *out = g_steal_pointer(&info);
    } else if (error_out) {
        char diagnostic[AV_ERROR_MAX_STRING_SIZE] = {0};
        if (status < 0) av_strerror(status, diagnostic, sizeof(diagnostic));
        *error_out = g_strdup_printf("%s%s%s", failure ? failure : "Cannot read media information",
                                     status < 0 ? ": " : "", diagnostic);
    }
    return outcome;
}

static void checksum_u64(GChecksum *hash,uint64_t value){value=GUINT64_TO_LE(value);g_checksum_update(hash,(const guint8*)&value,sizeof(value));}
static void checksum_time(GChecksum *hash,int64_t ticks,AVRational base){
    if(ticks==AV_NOPTS_VALUE){checksum_u64(hash,UINT64_MAX);return;}
    checksum_u64(hash,0);
    __int128 numerator=(__int128)ticks*base.num;
    int64_t remainder=(int64_t)(numerator%base.den);if(remainder<0)remainder=-remainder;
    int64_t divisor=av_gcd(remainder,base.den);if(!divisor)divisor=1;
    numerator/=divisor;
    __uint128_t unsigned_value=(__uint128_t)numerator;
    checksum_u64(hash,(uint64_t)unsigned_value);checksum_u64(hash,(uint64_t)(unsigned_value>>64));checksum_u64(hash,(uint64_t)(base.den/divisor));
}
/* Packet data, boundaries, rational timestamps, flags and side data in each
 * track are hashed independently of container interleaving and file offsets. */
TcStatusCode tc_media_packet_manifest(const char *path,GCancellable *cancel,GPtrArray **out,char **error){
    *out=NULL;struct stat initial={0},end={0};
    ProbeInput input={.fd=-1,.cancellable=cancel,.started_us=g_get_monotonic_time(),
        .limits={.timeout_ms=6*60*60*1000,.max_read_bytes=UINT64_MAX}};
    AVFormatContext *format=NULL;AVIOContext *io=NULL;AVPacket *packet=NULL;AVDictionary *options=NULL;
    GPtrArray *hashes=g_ptr_array_new_with_free_func((GDestroyNotify)g_checksum_free),*manifest=NULL;
    uint64_t counts[128]={0},packets=0;int status=AVERROR_INVALIDDATA;TcStatusCode result=TC_STATUS_ERROR_CORRUPT;
    input.fd=open(path,O_RDONLY|O_CLOEXEC|O_NONBLOCK);
    if(input.fd<0||fstat(input.fd,&initial)||!S_ISREG(initial.st_mode)||initial.st_size<=0)goto done;
    input.size=initial.st_size;
    input.limits.max_read_bytes=(uint64_t)initial.st_size> (UINT64_MAX-64ULL*1024*1024)/3?UINT64_MAX:(uint64_t)initial.st_size*3+64ULL*1024*1024;
    uint8_t *buffer=av_malloc(32768);format=avformat_alloc_context();
    if(!buffer||!format){av_free(buffer);result=TC_STATUS_ERROR_MEM_LIMIT;goto done;}
    io=avio_alloc_context(buffer,32768,0,&input,read_input,NULL,seek_input);
    if(!io){av_free(buffer);result=TC_STATUS_ERROR_MEM_LIMIT;goto done;}
    format->pb=io;format->flags|=AVFMT_FLAG_CUSTOM_IO;format->interrupt_callback=(AVIOInterruptCB){interrupted,&input};
    format->opaque=&input;format->io_open=deny_secondary_open;format->max_streams=128;format->probesize=16*1024*1024;format->max_index_size=1024*1024;
    av_dict_set(&options,"format_whitelist","mov",0);
    status=avformat_open_input(&format,NULL,NULL,&options);
    if(status<0||!format->nb_streams||format->nb_streams>128)goto done;
    for(guint i=0;i<format->nb_streams;i++){
        if(format->streams[i]->time_base.den<=0||format->streams[i]->time_base.num<=0)goto done;
        g_ptr_array_add(hashes,g_checksum_new(G_CHECKSUM_SHA256));
    }
    packet=av_packet_alloc();if(!packet){result=TC_STATUS_ERROR_MEM_LIMIT;goto done;}
    while((status=av_read_frame(format,packet))>=0){
        if(interrupted(&input)){result=cancel&&g_cancellable_is_cancelled(cancel)?TC_STATUS_ERROR_CANCELLED:TC_STATUS_ERROR_TIMEOUT;goto done;}
        if(++packets>20000000 || packet->stream_index<0 || (guint)packet->stream_index>=hashes->len ||
            packet->size<0 || packet->size>64*1024*1024 || packet->side_data_elems>128 || (packet->flags&AV_PKT_FLAG_CORRUPT))goto done;
        GChecksum *hash=g_ptr_array_index(hashes,packet->stream_index);
        AVRational base=format->streams[packet->stream_index]->time_base;
        checksum_u64(hash,(uint64_t)packet->size);g_checksum_update(hash,packet->data,packet->size);
        checksum_time(hash,packet->pts,base);checksum_time(hash,packet->dts,base);checksum_time(hash,packet->duration,base);
        checksum_u64(hash,(uint64_t)packet->flags);checksum_u64(hash,(uint64_t)packet->side_data_elems);
        for(int i=0;i<packet->side_data_elems;i++){
            const AVPacketSideData *side=&packet->side_data[i];if(side->size>1024*1024)goto done;
            checksum_u64(hash,side->type);checksum_u64(hash,side->size);g_checksum_update(hash,side->data,side->size);
        }
        counts[packet->stream_index]++;av_packet_unref(packet);
    }
    if(status!=AVERROR_EOF||!packets||input.read_limit_hit||input.secondary_open_denied)goto done;
    if(fstat(input.fd,&end)||!same_file(&initial,&end)||stat(path,&end)||!same_file(&initial,&end))goto done;
    manifest=g_ptr_array_new_with_free_func(g_free);
    for(guint i=0;i<hashes->len;i++){
        if(!counts[i])goto done;
        g_ptr_array_add(manifest,g_strdup_printf("%" G_GUINT64_FORMAT ":%s",counts[i],g_checksum_get_string(g_ptr_array_index(hashes,i))));
    }
    *out=manifest;manifest=NULL;result=TC_STATUS_OK;
 done:
    if(cancel&&g_cancellable_is_cancelled(cancel))result=TC_STATUS_ERROR_CANCELLED;
    if(result!=TC_STATUS_OK&&error&&!*error)*error=g_strdup("Packet manifest failed: corrupt, external, changing, unsupported or over-budget media");
    if(manifest)g_ptr_array_unref(manifest);
    g_ptr_array_unref(hashes);
    av_packet_free(&packet);av_dict_free(&options);avformat_close_input(&format);
    if(io)av_freep(&io->buffer);
    avio_context_free(&io);if(input.fd>=0)close(input.fd);return result;
}
