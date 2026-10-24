#include "tc_engine.h"
#include "tc_media_probe.h"
#include "tc_process_runner.h"
#include <libavutil/channel_layout.h>
static const char *binary(void){
    return "/usr/bin/ffmpeg";
}
static TcStatusCode fail(char **error,TcStatusCode status,const char *reason){
    if(error && !*error)*error=g_strdup(reason);
    return status;
}
typedef struct {
    GChecksum *hash;
    uint64_t bytes;
    bool overflow;
} PcmDigest;
static void hash_pcm(const char *data,gsize size,gpointer user){
    PcmDigest *d=user;
    if(size>UINT64_MAX-d->bytes){
        d->overflow=true;
        return;
    }
    d->bytes+=size;
    g_checksum_update(d->hash,(const guint8*)data,size);
}
static TcStatusCode pcm_digest(const char *path,GCancellable *cancel,char digest[65],uint64_t *bytes,char **error){
    const char *argv[]={
        binary(),"-v","error","-nostdin","-xerror","-threads","2","-i",path,
        "-map","0:a:0","-vn","-sn","-dn","-c:a","pcm_s32le","-threads","2","-f","s32le","pipe:1",NULL
    };
    const TcProcessOptions options={
        .timeout_ms=6*60*60*1000,.terminate_grace_ms=500,.max_address_bytes=2ULL*1024*1024*1024
    };
    PcmDigest d={
        .hash=g_checksum_new(G_CHECKSUM_SHA256)
    };
    TcStatusCode status=tc_process_run(argv,&options,cancel,hash_pcm,&d,NULL,error);
    if(status==TC_STATUS_OK && (d.overflow||!d.bytes))status=fail(error,TC_STATUS_ERROR_CORRUPT,"Audio decoder returned no samples or overflowed sample count");
    if(status==TC_STATUS_OK){
        g_strlcpy(digest,g_checksum_get_string(d.hash),65);
        *bytes=d.bytes;
    }
    g_checksum_free(d.hash);
    return status;
}
static bool metadata_contains(AVDictionary *before,AVDictionary *after){
    const AVDictionaryEntry *item=NULL;
    while((item=av_dict_get(before,"",item,AV_DICT_IGNORE_SUFFIX))){
        const AVDictionaryEntry *found=av_dict_get(after,item->key,NULL,0);
        if(!found||strcmp(found->value,item->value))return false;
    }
    return true;
}
static bool distinct(const char *src,const char *dst){
    struct stat a,b;
    if(stat(src,&a)||!S_ISREG(a.st_mode))return false;
    if(lstat(dst,&b))return errno==ENOENT;
    return S_ISREG(b.st_mode)&&(a.st_dev!=b.st_dev||a.st_ino!=b.st_ino);
}
static bool same_stat(const struct stat *a,const struct stat *b){
    return a->st_dev==b->st_dev&&a->st_ino==b->st_ino&&a->st_size==b->st_size&&a->st_mtim.tv_sec==b->st_mtim.tv_sec&&a->st_mtim.tv_nsec==b->st_mtim.tv_nsec&&a->st_ctim.tv_sec==b->st_ctim.tv_sec&&a->st_ctim.tv_nsec==b->st_ctim.tv_nsec;
}
static bool supports(const char *format){
    return format&&!strcmp(format,"safe-flac");
}
static TcStatusCode convert(const char *src,const char *dst,const char *format,uint16_t w,uint16_t h,GCancellable *cancel,char **error,TcProgressCallback progress,void *data){
    (void)format;
    (void)w;
    (void)h;
    (void)progress;
    (void)data;
    if(!distinct(src,dst))return fail(error,TC_STATUS_ERROR,"Audio candidate aliases the source or is nonregular");
    g_autoptr(TcMediaInfo) before=NULL,after=NULL;
    TcStatusCode status=tc_media_probe_file(src,NULL,cancel,&before,error);
    if(status!=TC_STATUS_OK)return status;
    if(before->streams->len!=1||before->chapters->len||before->program_count||before->stream_group_count)
    return fail(error,TC_STATUS_ERROR_UNSUPPORTED,"Lossless FLAC requires one audio stream without chapters, cover art or program groups");
    TcMediaStream *stream=g_ptr_array_index(before->streams,0);
    AVCodecParameters *p=stream->codec;
    /* WAVEFORMATEX defines legacy 1/2-channel PCM as mono/stereo even without
     * an extensible channel mask. Do not guess multichannel layouts. */
    if(!strcmp(before->format_name,"wav") && p->ch_layout.order==AV_CHANNEL_ORDER_UNSPEC &&
    (p->ch_layout.nb_channels==1||p->ch_layout.nb_channels==2))
    av_channel_layout_default(&p->ch_layout,p->ch_layout.nb_channels);
    int bits=p->bits_per_raw_sample?p->bits_per_raw_sample:p->bits_per_coded_sample;
    bool lossless=p->codec_id==AV_CODEC_ID_FLAC||p->codec_id==AV_CODEC_ID_PCM_S16LE||p->codec_id==AV_CODEC_ID_PCM_S16BE||p->codec_id==AV_CODEC_ID_PCM_S24LE||p->codec_id==AV_CODEC_ID_PCM_S24BE;
    if(p->codec_type!=AVMEDIA_TYPE_AUDIO||!lossless||(bits!=16&&bits!=24)||p->sample_rate<=0||p->ch_layout.nb_channels<=0||p->ch_layout.nb_channels>8||p->ch_layout.order!=AV_CHANNEL_ORDER_NATIVE)
    return fail(error,TC_STATUS_ERROR_UNSUPPORTED,"FLAC preservation supports known 16/24-bit integer PCM/FLAC layouts; float, unknown and other depths are skipped");
    char original[65],candidate[65];
    uint64_t original_bytes=0,candidate_bytes=0;
    status=pcm_digest(src,cancel,original,&original_bytes,error);
    if(status!=TC_STATUS_OK)return status;
    char depth[8];
    g_snprintf(depth,sizeof(depth),"%d",bits);
    const char *argv[]={
        binary(),"-v","error","-nostdin","-y","-xerror","-threads","2","-i",src,
        "-map","0:a:0","-map_metadata","0","-map_chapters","0","-c:a","flac","-compression_level","12",
        "-sample_fmt",bits==16?"s16":"s32","-bits_per_raw_sample",depth,"-threads","2","-f","flac",dst,NULL
    };
    const TcProcessOptions options={
        .timeout_ms=6*60*60*1000,.terminate_grace_ms=500,.max_address_bytes=2ULL*1024*1024*1024
    };
    status=tc_process_run(argv,&options,cancel,NULL,NULL,NULL,error);
    if(status!=TC_STATUS_OK)return status;
    status=tc_media_probe_file(dst,NULL,cancel,&after,error);
    if(status!=TC_STATUS_OK)return status;
    if(after->streams->len!=1)return fail(error,TC_STATUS_ERROR_CORRUPT,"FLAC stream count changed");
    TcMediaStream *out=g_ptr_array_index(after->streams,0);
    if(out->codec->codec_id!=AV_CODEC_ID_FLAC||out->codec->sample_rate!=p->sample_rate||
    out->codec->bits_per_raw_sample!=bits||av_channel_layout_compare(&p->ch_layout,&out->codec->ch_layout)||
    !metadata_contains(before->metadata,after->metadata)||!metadata_contains(stream->metadata,out->metadata))return fail(error,TC_STATUS_ERROR_CORRUPT,"FLAC depth, rate, channel layout or metadata changed");
    status=pcm_digest(dst,cancel,candidate,&candidate_bytes,error);
    if(status!=TC_STATUS_OK)return status;
    if(original_bytes!=candidate_bytes||strcmp(original,candidate))return fail(error,TC_STATUS_ERROR_CORRUPT,"Decoded PCM samples changed");
    struct stat end;
    if(stat(src,&end)||!same_stat(&before->source_stat,&end))return fail(error,TC_STATUS_ERROR_CORRUPT,"Audio source changed during optimization");
    return TC_STATUS_OK;
}
TcEngine tc_engine_audio_preserve={
    .name="audio_preserve",.category=TC_CATEGORY_MEDIA,.supports_format=supports,.convert=convert,.estimate=tc_media_estimate
};
