#include "tc_engine.h"
#include "tc_media_probe.h"
#include "tc_process_runner.h"
#include <libavutil/mathematics.h>
#define META_LIMIT (128ULL*1024*1024)
typedef struct {
    uint64_t offset;
    unsigned char bytes[12];
} BitrateHint;
typedef struct {
    GArray *bitrate_hints;
    int fd;
    GChecksum *hash;
    uint64_t metadata;
    unsigned atoms,ftyp,moov,mdat;
    GCancellable *cancel;
} Layout;
static uint32_t be32(const unsigned char *p){
    return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];
}
static bool read_at(int fd,void *buffer,size_t count,uint64_t offset){
    char *p=buffer;
    while(count){
        ssize_t n=pread(fd,p,count,(off_t)offset);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)return false;
        p+=n;
        offset+=n;
        count-=n;
    }
    return true;
}
static bool one_of(const unsigned char *type,const char *const *names){
    for(unsigned i=0;names[i];i++)if(!memcmp(type,names[i],4))return true;
    return false;
}
static bool hash_leaf(Layout *s,const unsigned char type[4],uint64_t offset,uint64_t length){
    if(length>16ULL*1024*1024||length>META_LIMIT-s->metadata)return false;
    s->metadata+=length;
    g_checksum_update(s->hash,type,4);
    uint64_t n=GUINT64_TO_LE(length);
    g_checksum_update(s->hash,(const guint8*)&n,8);
    unsigned char buffer[65536];
    while(length){
        if(s->cancel&&g_cancellable_is_cancelled(s->cancel))return false;
        size_t bytes=(size_t)MIN(length,sizeof(buffer));
        if(!read_at(s->fd,buffer,bytes,offset))return false;
        g_checksum_update(s->hash,buffer,bytes);
        offset+=bytes;
        length-=bytes;
    }
    return true;
}
static bool sample_description(Layout *s,uint64_t offset,uint64_t length){
    unsigned char h[44];
    if(length<16||!read_at(s->fd,h,16,offset)||be32(h)!=0||be32(h+4)!=1)return false;
    uint32_t size=be32(h+8);
    const unsigned char *codec=h+12;
    static const char *video[]={
        "avc1","avc3","hvc1","hev1","av01","vp09",NULL
    };
    static const char *audio[]={
        "mp4a","ac-3","ec-3","Opus","fLaC",NULL
    };
    size_t fixed=one_of(codec,video)?86:one_of(codec,audio)?36:0;
    if(!fixed||size!=length-8||size<fixed||!read_at(s->fd,h,16,offset+16))return false;
    if(h[6]!=0||h[7]!=1)return false;
    /* One self-contained data reference. */
    if(fixed==36 && (h[8]||h[9]))return false;
    /* QuickTime audio versions need another validator. */
    static const char *children[]={
        "avcC","hvcC","av1C","vpcC","esds","btrt","pasp","colr","clap","fiel","clli","mdcv","dvcC","dvvC","dOps","dfLa","dac3","dec3","srat",NULL
    };
    uint64_t pos=offset+8+fixed,end=offset+length;
    while(pos<end){
        if(end-pos<8||!read_at(s->fd,h,8,pos))return false;
        uint32_t n=be32(h);
        if(n<8||n>end-pos||!one_of(h+4,children))return false;
        if(!memcmp(h+4,"btrt",4)){
            if(n!=20)return false;
            if(s->bitrate_hints){
                if(s->bitrate_hints->len>=1024)return false;
                BitrateHint hint={
                    .offset=pos+8
                };
                if(!read_at(s->fd,hint.bytes,12,hint.offset))return false;
                g_array_append_val(s->bitrate_hints,hint);
            }
        }
        pos+=n;
    }
    return pos==end;
}
static bool sample_table(Layout *s,const unsigned char type[4],uint64_t offset,uint64_t length){
    unsigned char h[16];
    if(length<8||!read_at(s->fd,h,MIN(length,sizeof(h)),offset))return false;
    if(!memcmp(type,"stsz",4)){
        if(length<12||be32(h+8)>5000000||be32(h+4)>64*1024*1024)return false;
        uint32_t uniform=be32(h+4),count=be32(h+8);
        if(uniform)return length==12;
        if(length!=12ULL+4ULL*count)return false;
        for(uint32_t i=0;i<count;i++){
            if(s->cancel&&g_cancellable_is_cancelled(s->cancel))return false;
            if(!read_at(s->fd,h,4,offset+12+4ULL*i)||be32(h)>64*1024*1024)return false;
        }
    } else if(!memcmp(type,"stsc",4)){
        uint32_t count=be32(h+4);
        if(count>5000000||length!=8ULL+12ULL*count)return false;
        for(uint32_t i=0;i<count;i++){
            if(s->cancel&&g_cancellable_is_cancelled(s->cancel))return false;
            if(!read_at(s->fd,h,4,offset+8+12ULL*i+8)||be32(h)!=1)return false;
        }
    } else{
        uint32_t count=be32(h+4);
        unsigned entry=!memcmp(type,"co64",4)||!memcmp(type,"stts",4)||!memcmp(type,"ctts",4)?8:4;
        if(count>5000000 || length!=8ULL+(uint64_t)count*entry)return false;
        if(!memcmp(type,"stts",4)||!memcmp(type,"ctts",4)){
            uint64_t total=0;
            for(uint32_t i=0;i<count;i++){
                if(s->cancel&&g_cancellable_is_cancelled(s->cancel))return false;
                if(!read_at(s->fd,h,4,offset+8+8ULL*i))return false;
                total+=be32(h);
                if(total>5000000)return false;
            }
        }
    }
    if(length>META_LIMIT-s->metadata)return false;
    s->metadata+=length;
    return true;
}
static bool walk(Layout *s,uint64_t begin,uint64_t end,unsigned depth,const char parent[4]){
    if(depth>16)return false;
    static const char *containers[]={
        "moov","trak","mdia","minf","stbl","edts","dinf","udta","meta","ilst",NULL
    };
    static const char *leaves[]={
        "mvhd","tkhd","mdhd","hdlr","vmhd","smhd","nmhd","elst","sdtp","cslg",NULL
    };
    static const char *tables[]={
        "stts","ctts","stsc","stsz","stco","co64","stss",NULL
    };
    static const char *tags[]={
        "\xa9" "nam","\xa9" "ART","\xa9" "alb","\xa9" "cmt","\xa9" "day","\xa9" "gen","\xa9" "too","\xa9" "wrt","cprt","desc","ldes","trkn","disk","covr",NULL
    };
    for(uint64_t pos=begin;pos<end;){
        unsigned char h[32];
        if(++s->atoms>100000||end-pos<8||!read_at(s->fd,h,8,pos))return false;
        uint64_t size=be32(h),header=8;
        unsigned char type[4];
        memcpy(type,h+4,4);
        if(size==1){
            if(end-pos<16||!read_at(s->fd,h+8,8,pos+8))return false;
            size=(uint64_t)be32(h+8)<<32|be32(h+12);
            header=16;
        } else if(!size)size=end-pos;
        if(size<header||size>end-pos)return false;
        uint64_t payload=pos+header,length=size-header;
        if(s->cancel&&g_cancellable_is_cancelled(s->cancel))return false;
        if(!memcmp(type,"free",4)||!memcmp(type,"skip",4)||(!memcmp(type,"wide",4)&&size==8)){
        }
        else if(depth==0&&!memcmp(type,"mdat",4)){
            if(!length)return false;
            s->mdat++;
        }
        else if(depth==0&&!memcmp(type,"ftyp",4)){
            if(pos||++s->ftyp!=1||length<8||!hash_leaf(s,type,payload,length))return false;
        }
        else if(one_of(type,containers)){
            if(!memcmp(type,"moov",4)&&(!depth&&++s->moov!=1))return false;
            if(!memcmp(type,"meta",4)){
                if(length<4||!read_at(s->fd,h,4,payload)||be32(h))return false;
                payload+=4;
                length-=4;
            }
            g_checksum_update(s->hash,type,4);
            if(!walk(s,payload,payload+length,depth+1,(const char*)type))return false;
            g_checksum_update(s->hash,(const guint8*)"END!",4);
        } else if(!memcmp(parent,"ilst",4)){
            if(!one_of(type,tags)||!hash_leaf(s,type,payload,length))return false;
        } else if(!memcmp(type,"stsd",4)){
            if(!sample_description(s,payload,length)||!hash_leaf(s,type,payload,length))return false;
        } else if(!memcmp(type,"dref",4)){
            if(length!=20||!read_at(s->fd,h,20,payload)||be32(h)||be32(h+4)!=1||be32(h+8)!=12||memcmp(h+12,"url ",4)||be32(h+16)!=1||!hash_leaf(s,type,payload,length))return false;
        } else if(!memcmp(type,"sgpd",4)||!memcmp(type,"sbgp",4)){
            if(length<8||!read_at(s->fd,h,8,payload)||(memcmp(h+4,"roll",4)&&memcmp(h+4,"prol",4))||!hash_leaf(s,type,payload,length))return false;
        } else if(one_of(type,tables)){
            if(!sample_table(s,type,payload,length))return false;
        }
        else if(one_of(type,leaves)){
            if(!hash_leaf(s,type,payload,length))return false;
        }
        else return false;
        pos+=size;
    }
    return true;
}
static TcStatusCode layout_manifest(const char *path,GCancellable *cancel,char digest[65],GArray **hints,char **error){
    if(hints)*hints=NULL;
    Layout s={
        .fd=open(path,O_RDONLY|O_CLOEXEC|O_NONBLOCK),.hash=g_checksum_new(G_CHECKSUM_SHA256),.cancel=cancel,.bitrate_hints=hints?g_array_new(FALSE,FALSE,sizeof(BitrateHint)):NULL
    };
    struct stat st;
    bool ok=s.fd>=0&&fstat(s.fd,&st)==0&&S_ISREG(st.st_mode)&&st.st_size>0&&walk(&s,0,st.st_size,0,"root")&&s.ftyp==1&&s.moov==1&&s.mdat;
    if(ok){
        g_strlcpy(digest,g_checksum_get_string(s.hash),65);
        if(hints){
            *hints=s.bitrate_hints;
            s.bitrate_hints=NULL;
        }
    }
    if(s.bitrate_hints)g_array_unref(s.bitrate_hints);
    if(s.fd>=0)close(s.fd);
    g_checksum_free(s.hash);
    if(cancel&&g_cancellable_is_cancelled(cancel))return TC_STATUS_ERROR_CANCELLED;
    if(!ok&&error&&!*error)*error=g_strdup("MP4/MOV contains unverified, external, fragmented, opaque or over-budget structures");
    return ok?TC_STATUS_OK:TC_STATUS_ERROR_UNSUPPORTED;
}
/* The muxer recalculates bitrate hints even for packet-copy. Restore these
 * non-media scalars before validating every retained semantic atom. */
static bool restore_bitrate_hints(const char *target,GArray *source,GArray *candidate){
    if(source->len!=candidate->len)return false;
    int fd=open(target,O_WRONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0)return false;
    bool ok=true;
    for(guint i=0;i<source->len&&ok;i++){
        BitrateHint *a=&g_array_index(source,BitrateHint,i),*b=&g_array_index(candidate,BitrateHint,i);
        size_t offset=0;
        while(offset<12){
            ssize_t n=pwrite(fd,a->bytes+offset,12-offset,(off_t)b->offset+offset);
            if(n<0&&errno==EINTR)continue;
            if(n<=0){
                ok=false;
                break;
            }
            offset+=n;
        }
    }
    if(ok&&fsync(fd))ok=false;
    close(fd);
    return ok;
}
static bool dict_equal(AVDictionary *a,AVDictionary *b){
    if(av_dict_count(a)!=av_dict_count(b))return false;
    const AVDictionaryEntry *item=NULL;
    while((item=av_dict_get(a,"",item,AV_DICT_IGNORE_SUFFIX))){
        const AVDictionaryEntry *other=av_dict_get(b,item->key,NULL,AV_DICT_MATCH_CASE);
        if(!other||strcmp(item->value,other->value))return false;
    }
    return true;
}
static bool snapshots_equal(const TcMediaInfo *a,const TcMediaInfo *b){
    if(a->streams->len!=b->streams->len||a->chapters->len!=b->chapters->len||a->program_count!=b->program_count||a->stream_group_count!=b->stream_group_count||!dict_equal(a->metadata,b->metadata))return false;
    for(guint i=0;i<a->streams->len;i++){
        TcMediaStream *x=g_ptr_array_index(a->streams,i),*y=g_ptr_array_index(b->streams,i);
        AVCodecParameters *p=x->codec,*q=y->codec;
        if(x->id!=y->id||x->disposition!=y->disposition||!dict_equal(x->metadata,y->metadata)||x->side_data->len!=y->side_data->len||
        p->codec_type!=q->codec_type||p->codec_id!=q->codec_id||p->width!=q->width||p->height!=q->height||p->format!=q->format||p->profile!=q->profile||p->level!=q->level||
        p->sample_rate!=q->sample_rate||p->bits_per_raw_sample!=q->bits_per_raw_sample||p->bits_per_coded_sample!=q->bits_per_coded_sample||
        av_channel_layout_compare(&p->ch_layout,&q->ch_layout)||p->color_range!=q->color_range||p->color_primaries!=q->color_primaries||p->color_trc!=q->color_trc||p->color_space!=q->color_space||p->chroma_location!=q->chroma_location||
        av_cmp_q(p->sample_aspect_ratio,q->sample_aspect_ratio)||p->extradata_size!=q->extradata_size||(p->extradata_size&&memcmp(p->extradata,q->extradata,p->extradata_size)))return false;
        for(guint j=0;j<x->side_data->len;j++){
            TcMediaSideData *u=g_ptr_array_index(x->side_data,j),*v=g_ptr_array_index(y->side_data,j);
            if(u->type!=v->type||!g_bytes_equal(u->bytes,v->bytes))return false;
        }
    }
    return true;
}
static bool supports(const char *format){
    return format&&!strcmp(format,"comp-remux");
}
static TcStatusCode convert(const char *src,const char *dst,const char *format,uint16_t w,uint16_t h,GCancellable *cancel,char **error,TcProgressCallback progress,void *data){
    (void)format;
    (void)w;
    (void)h;
    (void)progress;
    (void)data;
    struct stat in,out;
    if(stat(src,&in)||(!lstat(dst,&out)&&(!S_ISREG(out.st_mode)||(in.st_dev==out.st_dev&&in.st_ino==out.st_ino))))return TC_STATUS_ERROR;
    g_autoptr(GArray) original_hints=NULL,candidate_hints=NULL;
    char original_layout[65],candidate_layout[65];
    TcStatusCode status=layout_manifest(src,cancel,original_layout,&original_hints,error);
    if(status!=TC_STATUS_OK)return status;
    g_autoptr(TcMediaInfo) a=NULL,b=NULL;
    g_autoptr(GPtrArray) original_packets=NULL,candidate_packets=NULL;
    status=tc_media_probe_file(src,NULL,cancel,&a,error);
    if(status!=TC_STATUS_OK)return status;
    if(a->chapters->len||a->program_count||a->stream_group_count){
        if(error)*error=g_strdup("Chapter, program or stream-group remuxing requires an additional validator");
        return TC_STATUS_ERROR_UNSUPPORTED;
    }
    for(guint i=0;i<a->streams->len;i++){
        TcMediaStream *stream=g_ptr_array_index(a->streams,i);
        if((stream->codec->codec_type!=AVMEDIA_TYPE_VIDEO&&stream->codec->codec_type!=AVMEDIA_TYPE_AUDIO)||(stream->disposition&AV_DISPOSITION_ATTACHED_PIC)){
            if(error)*error=g_strdup("Remux currently supports verified MP4/MOV audio/video tracks only; use padding optimization for other layouts");
            return TC_STATUS_ERROR_UNSUPPORTED;
        }
    }
    status=tc_media_packet_manifest(src,cancel,&original_packets,error);
    if(status!=TC_STATUS_OK)return status;
    const char *argv[]={
        "/usr/bin/ffmpeg","-v","error","-nostdin","-y","-xerror","-copyts","-threads","2","-i",src,"-map","0","-map_metadata","0","-map_chapters","0","-c","copy","-copytb","1","-avoid_negative_ts","disabled","-movflags","+faststart",dst,NULL
    };
    const TcProcessOptions options={
        .timeout_ms=6*60*60*1000,.terminate_grace_ms=500,.max_address_bytes=2ULL*1024*1024*1024
    };
    status=tc_process_run(argv,&options,cancel,NULL,NULL,NULL,error);
    if(status!=TC_STATUS_OK)return status;
    status=layout_manifest(dst,cancel,candidate_layout,&candidate_hints,error);
    if(status!=TC_STATUS_OK)return status;
    if(!restore_bitrate_hints(dst,original_hints,candidate_hints)){
        if(error)*error=g_strdup("Cannot preserve bitrate-hint metadata");
        return TC_STATUS_ERROR_CORRUPT;
    }
    status=layout_manifest(dst,cancel,candidate_layout,NULL,error);
    if(status!=TC_STATUS_OK)return status;
    status=tc_media_probe_file(dst,NULL,cancel,&b,error);
    if(status!=TC_STATUS_OK)return status;
    if(strcmp(original_layout,candidate_layout)||!snapshots_equal(a,b)){
        if(error)*error=g_strdup("Remux candidate changed track properties or container metadata");
        return TC_STATUS_ERROR_CORRUPT;
    }
    status=tc_media_packet_manifest(dst,cancel,&candidate_packets,error);
    if(status!=TC_STATUS_OK)return status;
    if(original_packets->len!=candidate_packets->len)return TC_STATUS_ERROR_CORRUPT;
    for(guint i=0;i<original_packets->len;i++)if(strcmp(g_ptr_array_index(original_packets,i),g_ptr_array_index(candidate_packets,i))){
        if(error)*error=g_strdup("Remux packet bytes, timestamps, durations, flags or side data changed");
        return TC_STATUS_ERROR_CORRUPT;
    }
    const char *decode[]={
        "/usr/bin/ffmpeg","-v","error","-nostdin","-xerror","-threads","2","-i",dst,"-map","0","-threads","2","-f","null","-",NULL
    };
    status=tc_process_run(decode,&options,cancel,NULL,NULL,NULL,error);
    if(status!=TC_STATUS_OK)return status;
    struct stat after;
    if(stat(src,&after) || in.st_dev!=after.st_dev || in.st_ino!=after.st_ino ||
    in.st_size!=after.st_size || in.st_mtim.tv_sec!=after.st_mtim.tv_sec ||
    in.st_mtim.tv_nsec!=after.st_mtim.tv_nsec || in.st_ctim.tv_sec!=after.st_ctim.tv_sec ||
    in.st_ctim.tv_nsec!=after.st_ctim.tv_nsec){
        if(error)*error=g_strdup("Source changed during remux validation");
        return TC_STATUS_ERROR_CORRUPT;
    }
    return TC_STATUS_OK;
}
TcEngine tc_engine_media_remux={
    .name="media_remux",.category=TC_CATEGORY_MEDIA,.supports_format=supports,.convert=convert,.estimate=tc_media_estimate
};
