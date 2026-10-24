#include "tc_engine.h"
#include <zlib.h>
#include <jpeglib.h>
#include <setjmp.h>
#define IMAGE_FILE_LIMIT (32U*1024U*1024U)
#define IMAGE_RAW_LIMIT (64U*1024U*1024U)
#define IMAGE_PIXEL_LIMIT (16U*1024U*1024U)
static TcStatusCode failure(char **error,TcStatusCode code,const char *text){
    if(error && !*error)*error=g_strdup(text);
    return code;
}
static bool cancelled(GCancellable *c){
    return c&&g_cancellable_is_cancelled(c);
}
static bool write_all(int fd,const void *buffer,size_t length){
    const char *p=buffer;
    while(length){
        ssize_t n=write(fd,p,length);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)return false;
        p+=n;
        length-=n;
    }
    return true;
}
static int candidate_open(const char *source,const char *target){
    struct stat in,out;
    if(stat(source,&in))return -1;
    int fd=open(target,O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0)return -1;
    if(fstat(fd,&out)||!S_ISREG(out.st_mode)||(in.st_dev==out.st_dev&&in.st_ino==out.st_ino)||ftruncate(fd,0)){
        close(fd);
        errno=EINVAL;
        return -1;
    }
    return fd;
}
static GBytes *read_bounded(const char *path){
    int fd=open(path,O_RDONLY|O_CLOEXEC);
    struct stat st;
    if(fd<0)return NULL;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<=0||st.st_size>IMAGE_FILE_LIMIT){
        close(fd);
        return NULL;
    }
    gsize size=(gsize)st.st_size;
    guint8 *bytes=g_try_malloc(size);
    if(!bytes){
        close(fd);
        return NULL;
    }
    gsize offset=0;
    while(offset<size){
        ssize_t n=read(fd,bytes+offset,size-offset);
        if(n<0&&errno==EINTR)continue;
        if(n<=0){
            g_free(bytes);
            close(fd);
            return NULL;
        }
        offset+=n;
    }
    close(fd);
    return g_bytes_new_take(bytes,size);
}
static guint32 be32(const guint8 *p){
    return ((guint32)p[0]<<24)|((guint32)p[1]<<16)|((guint32)p[2]<<8)|p[3];
}
static void put32(guint8 *p,guint32 n){
    p[0]=n>>24;
    p[1]=n>>16;
    p[2]=n>>8;
    p[3]=n;
}
typedef struct {
    GBytes *file;
    GByteArray *idat;
    guint8 *raw;
    gsize raw_size;
    char hash[65];
} Png;
static void png_clear(Png *p){
    if(p->file)g_bytes_unref(p->file);
    if(p->idat)g_byte_array_unref(p->idat);
    g_free(p->raw);
    memset(p,0,sizeof(*p));
}
static bool known_chunk(const guint8 *type){
    static const char *known[]={
        "IHDR","PLTE","IDAT","IEND","cHRM","gAMA","iCCP","sBIT","sRGB","bKGD","hIST","tRNS","pHYs","sPLT","tIME","iTXt","tEXt","zTXt","eXIf",NULL
    };
    for(unsigned i=0;known[i];i++){
        if(!memcmp(type,known[i],4))return true;
    }
    return false;
}
static TcStatusCode png_read(const char *path,Png *png,GCancellable *cancel,char **error){
    png->file=read_bounded(path);
    if(!png->file)return failure(error,TC_STATUS_ERROR_UNSUPPORTED,"PNG exceeds the bounded safe-image input limit or is unreadable");
    gsize size=0;
    const guint8 *bytes=g_bytes_get_data(png->file,&size);
    if(size<33||memcmp(bytes,"\x89PNG\r\n\x1a\n",8))return failure(error,TC_STATUS_ERROR_CORRUPT,"Not a PNG file");
    png->idat=g_byte_array_new();
    g_autoptr(GChecksum) hash=g_checksum_new(G_CHECKSUM_SHA256);
    bool have_header=false,have_end=false,have_idat=false,ended_idat=false,have_palette=false;
    guint width=0,height=0,bits=0,channels=0,interlace=0,color=0;
    for(gsize pos=8;pos<size;){
        if(cancelled(cancel))return TC_STATUS_ERROR_CANCELLED;
        if(size-pos<12)return failure(error,TC_STATUS_ERROR_CORRUPT,"Truncated PNG chunk");
        guint32 length=be32(bytes+pos);
        const guint8 *type=bytes+pos+4,*data=bytes+pos+8;
        if(length>size-pos-12||crc32(0,type,(uInt)length+4)!=be32(data+length))return failure(error,TC_STATUS_ERROR_CORRUPT,"PNG chunk CRC or length mismatch");
        if(!known_chunk(type))return failure(error,TC_STATUS_ERROR_UNSUPPORTED,"Animated, signed or unknown PNG chunks are not rewritten");
        if(!memcmp(type,"IHDR",4)){
            if(pos!=8||have_header||length!=13)return failure(error,TC_STATUS_ERROR_CORRUPT,"Invalid PNG header");
            have_header=true;
            width=be32(data);
            height=be32(data+4);
            bits=data[8];
            color=data[9];
            interlace=data[12];
            channels=color==0||color==3?1:color==2?3:color==4?2:color==6?4:0;
            bool depth=(color==0&&(bits==1||bits==2||bits==4||bits==8||bits==16))||
            (color==3&&(bits==1||bits==2||bits==4||bits==8))||((color==2||color==4||color==6)&&(bits==8||bits==16));
            if(!width||!height||width>TC_MAX_IMAGE_DIMENSION||height>TC_MAX_IMAGE_DIMENSION||
            (uint64_t)width*height>IMAGE_PIXEL_LIMIT||!channels||!depth||data[10]||data[11]||interlace>1)
            return failure(error,TC_STATUS_ERROR_UNSUPPORTED,"PNG geometry, depth or method is outside safe-image limits");
        } else if(!have_header)return failure(error,TC_STATUS_ERROR_CORRUPT,"Missing PNG header");
        if(!memcmp(type,"PLTE",4)){
            if(have_palette||have_idat||!length||length%3||length>768)return failure(error,TC_STATUS_ERROR_CORRUPT,"Invalid PNG palette");
            have_palette=true;
        }
        if(!memcmp(type,"IDAT",4)){
            if(ended_idat||have_end)return failure(error,TC_STATUS_ERROR_CORRUPT,"Nonconsecutive PNG data");
            have_idat=true;
            g_byte_array_append(png->idat,data,length);
        } else{
            if(have_idat)ended_idat=true;
            g_checksum_update(hash,bytes+pos,length+12);
        }
        pos+=(gsize)length+12;
        if(!memcmp(type,"IEND",4)){
            if(length||pos!=size||!have_idat)return failure(error,TC_STATUS_ERROR_CORRUPT,"Invalid PNG end or trailing data");
            have_end=true;
        }
    }
    if(!have_end||(color==3&&!have_palette))return failure(error,TC_STATUS_ERROR_CORRUPT,"Incomplete PNG");
    static const guint xs[]={
        0,4,0,2,0,1,0
    },ys[]={
        0,0,4,0,2,0,1
    },dx[]={
        8,8,4,4,2,2,1
    },dy[]={
        8,8,8,4,4,2,2
    };
    unsigned passes=interlace?7:1;
    for(unsigned i=0;i<passes;i++){
        guint w=interlace?(width>xs[i]?(width-xs[i]+dx[i]-1)/dx[i]:0):width;
        guint h=interlace?(height>ys[i]?(height-ys[i]+dy[i]-1)/dy[i]:0):height;
        if(w&&h)png->raw_size+=(((gsize)w*channels*bits+7)/8+1)*h;
    }
    if(!png->raw_size||png->raw_size>IMAGE_RAW_LIMIT)return failure(error,TC_STATUS_ERROR_UNSUPPORTED,"PNG decoded data exceeds safe-image budget");
    png->raw=g_try_malloc(png->raw_size);
    if(!png->raw)return TC_STATUS_ERROR_MEM_LIMIT;
    uLongf raw_size=png->raw_size;
    uLong compressed_size=png->idat->len;
    if(uncompress2(png->raw,&raw_size,png->idat->data,&compressed_size)!=Z_OK||raw_size!=png->raw_size||compressed_size!=png->idat->len)
    return failure(error,TC_STATUS_ERROR_CORRUPT,"Invalid PNG DEFLATE data or decoded length");
    gsize row=0;
    for(unsigned i=0;i<passes;i++){
        guint w=interlace?(width>xs[i]?(width-xs[i]+dx[i]-1)/dx[i]:0):width;
        guint h=interlace?(height>ys[i]?(height-ys[i]+dy[i]-1)/dy[i]:0):height;
        if(!w||!h)continue;
        gsize stride=((gsize)w*channels*bits+7)/8+1;
        for(guint y=0;y<h;y++,row+=stride)if(png->raw[row]>4)return failure(error,TC_STATUS_ERROR_CORRUPT,"Invalid PNG row filter");
    }
    g_checksum_update(hash,png->raw,png->raw_size);
    g_strlcpy(png->hash,g_checksum_get_string(hash),sizeof(png->hash));
    return TC_STATUS_OK;
}
static TcStatusCode optimize_png(const char *source,const char *target,GCancellable *cancel,char **error){
    Png input={
        0
    },check={
        0
    };
    int fd=-1;
    guint8 *compressed=NULL;
    TcStatusCode status=png_read(source,&input,cancel,error);
    if(status!=TC_STATUS_OK)goto done;
    uLongf length=compressBound(input.raw_size);
    compressed=g_try_malloc(length);
    if(!compressed){
        status=TC_STATUS_ERROR_MEM_LIMIT;
        goto done;
    }
    if(compress2(compressed,&length,input.raw,input.raw_size,9)!=Z_OK){
        status=TC_STATUS_ERROR;
        goto done;
    }
    if(cancelled(cancel)){
        status=TC_STATUS_ERROR_CANCELLED;
        goto done;
    }
    fd=candidate_open(source,target);
    if(fd<0){
        status=failure(error,TC_STATUS_ERROR,"Cannot create a distinct PNG candidate");
        goto done;
    }
    gsize size=0;
    const guint8 *bytes=g_bytes_get_data(input.file,&size);
    bool wrote=false;
    if(!write_all(fd,bytes,8)){
        status=TC_STATUS_ERROR;
        goto done;
    }
    for(gsize pos=8;pos<size;){
        guint32 n=be32(bytes+pos);
        const guint8 *type=bytes+pos+4;
        if(!memcmp(type,"IDAT",4)){
            if(!wrote){
                guint8 header[8],crc[4];
                put32(header,(guint32)length);
                memcpy(header+4,"IDAT",4);
                uLong sum=crc32(0,(const Bytef*)"IDAT",4);
                sum=crc32(sum,compressed,length);
                put32(crc,sum);
                if(!write_all(fd,header,8)||!write_all(fd,compressed,length)||!write_all(fd,crc,4)){
                    status=TC_STATUS_ERROR;
                    goto done;
                }
                wrote=true;
            }
        } else if(!write_all(fd,bytes+pos,n+12)){
            status=TC_STATUS_ERROR;
            goto done;
        }
        pos+=(gsize)n+12;
    }
    if(fsync(fd)){
        status=TC_STATUS_ERROR;
        goto done;
    }
    close(fd);
    fd=-1;
    struct stat out;
    if(stat(target,&out)){
        status=TC_STATUS_ERROR;
        goto done;
    }
    if((uint64_t)out.st_size>=size){
        status=failure(error,TC_STATUS_NO_BENEFIT,"PNG already has an equally compact representation");
        goto done;
    }
    status=png_read(target,&check,cancel,error);
    if(status==TC_STATUS_OK&&strcmp(input.hash,check.hash))status=failure(error,TC_STATUS_ERROR_CORRUPT,"PNG samples or metadata changed");
    done:
    if(fd>=0)close(fd);
    g_free(compressed);
    png_clear(&input);
    png_clear(&check);
    return status;
}
typedef struct {
    struct jpeg_error_mgr base;
    jmp_buf jump;
    char message[JMSG_LENGTH_MAX];
} JError;
typedef struct {
    struct jpeg_decompress_struct input;
    struct jpeg_compress_struct output;
    JError error;
    bool in_ready,out_ready;
    FILE *in,*out;
    GChecksum *hash;
} Jpeg;
static void jpeg_fail(j_common_ptr codec){
    JError *e=(JError*)codec->err;
    (*codec->err->format_message)(codec,e->message);
    longjmp(e->jump,1);
}
static void jpeg_warning(j_common_ptr codec,int level){
    if(level<0)jpeg_fail(codec);
}
static void jpeg_clear(Jpeg *j){
    if(j->hash)g_checksum_free(j->hash);
    if(j->in_ready)jpeg_destroy_decompress(&j->input);
    if(j->out_ready)jpeg_destroy_compress(&j->output);
    if(j->in)fclose(j->in);
    if(j->out)fclose(j->out);
    g_free(j);
}
static Jpeg *jpeg_context(void){
    Jpeg *j=g_new0(Jpeg,1);
    j->input.err=j->output.err=jpeg_std_error(&j->error.base);
    j->error.base.error_exit=jpeg_fail;
    j->error.base.emit_message=jpeg_warning;
    return j;
}
static bool jpeg_markers_safe(jpeg_saved_marker_ptr marker){
    for(;marker;marker=marker->next){
        if(marker->marker==JPEG_COM)continue;
        if(marker->marker==JPEG_APP0 && marker->data_length>=5 && (!memcmp(marker->data,"JFIF\0",5)||!memcmp(marker->data,"JFXX\0",5)))continue;
        if(marker->marker==JPEG_APP0+1 && ((marker->data_length>=6&&!memcmp(marker->data,"Exif\0\0",6))||
        (marker->data_length>=29&&!memcmp(marker->data,"http://ns.adobe.com/xap/1.0/\0",29))))continue;
        if(marker->marker==JPEG_APP0+2 && marker->data_length>=12&&!memcmp(marker->data,"ICC_PROFILE\0",12))continue;
        if(marker->marker==JPEG_APP0+14 && marker->data_length>=5&&!memcmp(marker->data,"Adobe",5))continue;
        return false;
    }
    return true;
}
static TcStatusCode jpeg_open(Jpeg *j,const char *path,char **error){
    struct stat st;
    if(stat(path,&st)||st.st_size<=0||st.st_size>IMAGE_FILE_LIMIT)return failure(error,TC_STATUS_ERROR_UNSUPPORTED,"JPEG exceeds safe-image input limit");
    j->in=fopen(path,"rb");
    if(!j->in)return TC_STATUS_ERROR;
    j->in_ready=true;
    jpeg_create_decompress(&j->input);
    jpeg_stdio_src(&j->input,j->in);
    for(int i=0;i<16;i++)jpeg_save_markers(&j->input,JPEG_APP0+i,0xffff);
    jpeg_save_markers(&j->input,JPEG_COM,0xffff);
    jpeg_read_header(&j->input,TRUE);
    if(j->input.data_precision!=8 || !j->input.image_width || !j->input.image_height ||
    (uint64_t)j->input.image_width*j->input.image_height>IMAGE_PIXEL_LIMIT || !jpeg_markers_safe(j->input.marker_list))
    return failure(error,TC_STATUS_ERROR_UNSUPPORTED,"JPEG depth, geometry, MPO, signatures or unknown metadata cannot be preserved safely");
    return TC_STATUS_OK;
}
static TcStatusCode jpeg_fingerprint(const char *path,char digest[65],char **error){
    Jpeg *j=jpeg_context();
    TcStatusCode status=TC_STATUS_ERROR;
    if(setjmp(j->error.jump)){
        status=failure(error,TC_STATUS_ERROR_CORRUPT,j->error.message);
        goto done;
    }
    status=jpeg_open(j,path,error);
    if(status!=TC_STATUS_OK)goto done;
    jvirt_barray_ptr *coeff=jpeg_read_coefficients(&j->input);
    j->hash=g_checksum_new(G_CHECKSUM_SHA256);
    GChecksum *hash=j->hash;
    g_checksum_update(hash,(const guint8*)&j->input.image_width,sizeof(j->input.image_width));
    g_checksum_update(hash,(const guint8*)&j->input.image_height,sizeof(j->input.image_height));
    g_checksum_update(hash,(const guint8*)&j->input.num_components,sizeof(j->input.num_components));
    for(int c=0;c<j->input.num_components;c++){
        jpeg_component_info *component=&j->input.comp_info[c];
        int fields[]={
            component->component_id,component->h_samp_factor,component->v_samp_factor,component->quant_tbl_no
        };
        g_checksum_update(hash,(const guint8*)fields,sizeof(fields));
        for(JDIMENSION y=0;y<component->height_in_blocks;y++){
            JBLOCKARRAY row=(*j->input.mem->access_virt_barray)((j_common_ptr)&j->input,coeff[c],y,1,FALSE);
            g_checksum_update(hash,(const guint8*)row[0],component->width_in_blocks*sizeof(JBLOCK));
        }
    }
    for(int i=0;i<NUM_QUANT_TBLS;i++)if(j->input.quant_tbl_ptrs[i])g_checksum_update(hash,(const guint8*)j->input.quant_tbl_ptrs[i]->quantval,sizeof(j->input.quant_tbl_ptrs[i]->quantval));
    for(jpeg_saved_marker_ptr m=j->input.marker_list;m;m=m->next){
        g_checksum_update(hash,(const guint8*)&m->marker,sizeof(m->marker));
        g_checksum_update(hash,(const guint8*)&m->data_length,sizeof(m->data_length));
        g_checksum_update(hash,m->data,m->data_length);
    }
    jpeg_finish_decompress(&j->input);
    long end=ftell(j->in)-(long)j->input.src->bytes_in_buffer;
    struct stat st;
    if(fstat(fileno(j->in),&st)||end!=st.st_size)status=failure(error,TC_STATUS_ERROR_UNSUPPORTED,"JPEG trailing or opaque image data is not rewritten");
    if(status==TC_STATUS_OK)g_strlcpy(digest,g_checksum_get_string(hash),65);
    done:jpeg_clear(j);
    return status;
}
static TcStatusCode optimize_jpeg(const char *source,const char *target,GCancellable *cancel,char **error){
    char before[65],after[65];
    TcStatusCode status=jpeg_fingerprint(source,before,error);
    if(status!=TC_STATUS_OK)return status;
    if(cancelled(cancel))return TC_STATUS_ERROR_CANCELLED;
    Jpeg *j=jpeg_context();
    if(setjmp(j->error.jump)){
        status=failure(error,TC_STATUS_ERROR_CORRUPT,j->error.message);
        goto done;
    }
    status=jpeg_open(j,source,error);
    if(status!=TC_STATUS_OK)goto done;
    jvirt_barray_ptr *coeff=jpeg_read_coefficients(&j->input);
    int fd=candidate_open(source,target);
    if(fd<0){
        status=failure(error,TC_STATUS_ERROR,"Cannot create a distinct JPEG candidate");
        goto done;
    }
    j->out=fdopen(fd,"wb");
    if(!j->out){
        close(fd);
        status=TC_STATUS_ERROR;
        goto done;
    }
    j->out_ready=true;
    jpeg_create_compress(&j->output);
    jpeg_stdio_dest(&j->output,j->out);
    jpeg_copy_critical_parameters(&j->input,&j->output);
    j->output.optimize_coding=TRUE;
    j->output.write_JFIF_header=FALSE;
    j->output.write_Adobe_marker=FALSE;
    if(j->input.progressive_mode)jpeg_simple_progression(&j->output);
    jpeg_write_coefficients(&j->output,coeff);
    for(jpeg_saved_marker_ptr m=j->input.marker_list;m;m=m->next)jpeg_write_marker(&j->output,m->marker,m->data,m->data_length);
    jpeg_finish_compress(&j->output);
    jpeg_finish_decompress(&j->input);
    if(fflush(j->out)||fsync(fileno(j->out)))status=TC_STATUS_ERROR;
    done:jpeg_clear(j);
    if(status!=TC_STATUS_OK)return status;
    if(cancelled(cancel))return TC_STATUS_ERROR_CANCELLED;
    status=jpeg_fingerprint(target,after,error);
    if(status==TC_STATUS_OK&&strcmp(before,after))status=failure(error,TC_STATUS_ERROR_CORRUPT,"JPEG coefficients, quantization or metadata changed");
    return status;
}
static bool supports(const char *format){
    return format&&!strcmp(format,"comp-image");
}
static TcStatusCode convert(const char *src,const char *dst,const char *format,uint16_t w,uint16_t h,GCancellable *cancel,char **error,TcProgressCallback progress,void *data){
    (void)format;
    (void)w;
    (void)h;
    (void)progress;
    (void)data;
    if(cancelled(cancel))return TC_STATUS_ERROR_CANCELLED;
    int fd=open(src,O_RDONLY|O_CLOEXEC);
    guint8 signature[8];
    ssize_t n=fd>=0?read(fd,signature,8):-1;
    if(fd>=0)close(fd);
    if(n==8&&!memcmp(signature,"\x89PNG\r\n\x1a\n",8))return optimize_png(src,dst,cancel,error);
    if(n>=2&&signature[0]==0xff&&signature[1]==0xd8)return optimize_jpeg(src,dst,cancel,error);
    return failure(error,TC_STATUS_ERROR_UNSUPPORTED,"Safe image optimization supports bounded static PNG and JPEG only");
}
TcEngine tc_engine_image_preserve={
    .name="image_preserve",.category=TC_CATEGORY_IMAGE,.supports_format=supports,.convert=convert
};
