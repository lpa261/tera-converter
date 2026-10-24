#include "tc_engine.h"
#include "tc_media_probe.h"
TcStatusCode tc_media_estimate(const char *source,const char *format,uint16_t w,uint16_t h,
GCancellable *cancel,TcEngineEstimate *estimate,char **error){
    (void)w;
    (void)h;
    memset(estimate,0,sizeof(*estimate));
    if(!strcmp(format,"comp-padding")){
        estimate->memory_bytes=64ULL*1024*1024;
        return TC_STATUS_OK;
    }
    g_autoptr(TcMediaInfo) info=NULL;
    TcStatusCode status=tc_media_probe_file(source,NULL,cancel,&info,error);
    if(status!=TC_STATUS_OK)return status;
    estimate->memory_bytes=128ULL*1024*1024;
    for(guint i=0;i<info->streams->len;i++){
        TcMediaStream *s=g_ptr_array_index(info->streams,i);
        AVCodecParameters *p=s->codec;
        if(p->codec_type==AVMEDIA_TYPE_VIDEO&&!(s->disposition&AV_DISPOSITION_ATTACHED_PIC)){
            if(p->width<=0||p->height<=0||p->width>32768||p->height>32768)return TC_STATUS_ERROR_UNSUPPORTED;
            /* Decoder references, encoder lookahead and two filter surfaces.
             * Conservative, bounded estimate; process caps remain independent. */
            uint64_t footprint=(uint64_t)p->width*p->height*(s->bit_depth>8?6:3)*24+96ULL*1024*1024;
            estimate->memory_bytes=MAX(estimate->memory_bytes,footprint);
        } else if(p->codec_type==AVMEDIA_TYPE_AUDIO && (!strcmp(format,"wav")||!strcmp(format,"safe-flac")||!strcmp(format,"flac"))){
            if(info->duration_us<=0||p->sample_rate<=0||p->ch_layout.nb_channels<=0||p->ch_layout.nb_channels>64){
                if(error)*error=g_strdup("Unknown decoded-audio footprint; output cannot be reserved safely");
                return TC_STATUS_ERROR_UNSUPPORTED;
            }
            unsigned bytes=!strcmp(format,"wav")?2:4;
            __uint128_t frames=((__uint128_t)info->duration_us*p->sample_rate+999999)/1000000;
            __uint128_t output=frames*p->ch_layout.nb_channels*bytes+16ULL*1024*1024;
            if(output>UINT64_MAX)return TC_STATUS_ERROR_NO_SPACE;
            estimate->output_bytes=MAX(estimate->output_bytes,(uint64_t)output);
        }
    }
    return TC_STATUS_OK;
}
