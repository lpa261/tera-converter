#include "tc_menu_policy.h"
void tc_menu_file_clear(gpointer data){TcMenuFile *file=data;g_free(file->extension);g_free(file->mime);}
static gboolean one_of(const char *s,const char *words){
    if(!s)return FALSE;
    g_auto(GStrv) values=g_strsplit(words," ",-1);
    for(unsigned i=0;values[i];i++)if(!g_ascii_strcasecmp(s,values[i]))return TRUE;
    return FALSE;
}
static gboolean image(const TcMenuFile *f){return one_of(f->extension,"png jpg jpeg webp avif bmp tiff tif ico gif heic heif") && (!f->mime||g_str_has_prefix(f->mime,"image/"));}
static gboolean video(const TcMenuFile *f){return one_of(f->extension,"mp4 mov mkv webm avi flv wmv mpeg mpg m4v") && (!f->mime||g_str_has_prefix(f->mime,"video/"));}
static gboolean audio(const TcMenuFile *f){return one_of(f->extension,"mp3 wav flac m4a ogg opus aac aiff ape ac3 eac3") && (!f->mime||g_str_has_prefix(f->mime,"audio/"));}
static gboolean pdf(const TcMenuFile *f){return !g_strcmp0(f->extension,"pdf") && (!f->mime||!strcmp(f->mime,"application/pdf"));}
static gboolean supports_file(const TcMenuFile *f,guint caps,TcActionType action,const char *format){
    if(!f->local)return FALSE;
    if(action==TC_ACTION_REMOVE_BG)return (caps&TC_MENU_AI)&&image(f);
    if(action==TC_ACTION_RESIZE)return (caps&TC_MENU_UI)&&(caps&TC_MENU_VIPS)&&image(f);
    if(action==TC_ACTION_MERGE)return (caps&TC_MENU_VIPS)&&(caps&TC_MENU_PYTHON)&&image(f)&&!strcmp(format,"pdf");
    if(!strcmp(format,"comp-image"))return (caps&TC_MENU_IMAGES)&&one_of(f->extension,"png jpg jpeg")&&image(f);
    if(!strcmp(format,"comp-padding")||!strcmp(format,"comp-remux"))return (caps&TC_MENU_MEDIA)&&video(f)&&one_of(f->extension,"mp4 mov");
    if(!strcmp(format,"safe-flac"))return (caps&TC_MENU_MEDIA)&&audio(f)&&one_of(f->extension,"wav flac");
    if(!strcmp(format,"comp-office"))return (caps&TC_MENU_OFFICE) && (one_of(f->extension,"docx xlsx pptx") || ((caps&TC_MENU_PYTHON)&&one_of(f->extension,"odt ods odp")));
    if(!strcmp(format,"comp-pdf-safe"))return (caps&TC_MENU_PDF_SAFE)&&pdf(f);
    if(g_str_has_prefix(format,"comp-pdf"))return (caps&TC_MENU_PDF)&&(caps&TC_MENU_GS)&&pdf(f);
    if(action==TC_ACTION_COMPRESS)return (caps&TC_MENU_MEDIA)&&video(f)&&one_of(format,"comp-web comp-hq comp-saver");
    if(pdf(f))return (caps&TC_MENU_PDF) && ((!strcmp(format,"txt")&&(caps&TC_MENU_PDF_TEXT)) || (one_of(format,"png jpg jpeg")&&(caps&TC_MENU_PDF_RASTER)));
    if(image(f)&&one_of(format,"bmp ico")&&!(caps&TC_MENU_PYTHON))return FALSE;
    if(image(f))return (caps&TC_MENU_VIPS) && (one_of(format,"webp png jpg jpeg avif bmp tiff tif ico gif") || (!strcmp(format,"pdf")&&(caps&TC_MENU_PYTHON)));
    if(audio(f))return (caps&TC_MENU_MEDIA)&&one_of(format,"mp3 wav flac m4a ogg opus");
    if(video(f))return (caps&TC_MENU_MEDIA)&&one_of(format,"mp4 mov mkv avi webm gif mp3 wav flac m4a");
    return FALSE;
}
gboolean tc_menu_action_supported(const TcMenuSelection *s,TcActionType action,const char *format){
    if(!s||!s->count||!format||!(s->capabilities&TC_MENU_DAEMON))return FALSE;
    if(action==TC_ACTION_MERGE && s->count<2)return FALSE;
    gboolean all_same=TRUE;
    for(guint i=0;i<s->count;i++){
        if(!supports_file(&s->files[i],s->capabilities,action,format))return FALSE;
        if(g_ascii_strcasecmp(s->files[i].extension,format) && !(one_of(format,"jpg jpeg")&&one_of(s->files[i].extension,"jpg jpeg")) && !(one_of(format,"tif tiff")&&one_of(s->files[i].extension,"tif tiff")))all_same=FALSE;
    }
    return action!=TC_ACTION_CONVERT || !all_same;
}
gboolean tc_menu_preview_supported(const TcMenuSelection *s){
    if(!s||!s->count||!(s->capabilities&TC_MENU_PREVIEW))return FALSE;
    const TcMenuFile *f=&s->files[0];
    return f->local && (one_of(f->extension,"png jpg jpeg webp gif bmp tif tiff mp4 mov mkv webm avi mp3 wav flac ogg opus m4a aac") || (pdf(f)&&(s->capabilities&TC_MENU_PDF)) || (f->mime&&(g_str_has_prefix(f->mime,"text/")||!strcmp(f->mime,"application/json"))));
}
