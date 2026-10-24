#include "tc_preview.h"
#include <math.h>
#ifdef HAVE_POPPLER
#include <poppler.h>
#endif
void tc_preview_free(TcPreview *p) {
    if (!p) return;
    g_free(p->path); g_free(p->name); g_free(p->description); g_free(p->text);
    g_free(p->mime_type); g_free(p->type_name); g_free(p->modified);
    g_clear_object(&p->image);
    if (p->page) cairo_surface_destroy(p->page);
    g_free(p);
}
static gboolean listed(const char *ext, const char *const *names) {
    for (unsigned i=0; names[i]; i++) if (!g_ascii_strcasecmp(ext,names[i])) return TRUE;
    return FALSE;
}
TcPreview *tc_preview_load(const char *argument, unsigned page, GCancellable *cancel, GError **error) {
    if (g_cancellable_set_error_if_cancelled(cancel,error)) return NULL;
    g_autoptr(GFile) file=g_file_new_for_commandline_arg(argument);
    g_autofree char *path=g_file_get_path(file);
    if (!path) { g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_NOT_SUPPORTED,"Only local files can be previewed"); return NULL; }
    int fd=open(path,O_RDONLY|O_NONBLOCK|O_CLOEXEC);
    struct stat st;
    if (fd<0 || fstat(fd,&st) || !S_ISREG(st.st_mode)) {
        if(fd>=0) close(fd);
        g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_NOT_SUPPORTED,"Choose a regular local file"); return NULL;
    }
    g_autoptr(TcPreview) p=g_new0(TcPreview,1);
    p->path=g_strdup(path); p->name=g_filename_display_basename(path);
    p->file_size=(uint64_t)st.st_size;
    unsigned char sniff[4096];
    ssize_t sniff_size;
    do { sniff_size=pread(fd,sniff,sizeof(sniff),0); } while(sniff_size<0 && errno==EINTR);
    gboolean uncertain=FALSE;
    g_autofree char *content_type=g_content_type_guess(path,sniff,sniff_size>0?(gsize)sniff_size:0,&uncertain);
    p->mime_type=content_type?g_content_type_get_mime_type(content_type):NULL;
    p->type_name=content_type?g_content_type_get_description(content_type):NULL;
    if(!p->mime_type)p->mime_type=g_strdup("application/octet-stream");
    if(!p->type_name)p->type_name=g_strdup("File");
    g_autoptr(GDateTime) changed=g_date_time_new_from_unix_local(st.st_mtime);
    p->modified=changed?g_date_time_format(changed,"%Y-%m-%d %H:%M:%S %Z"):g_strdup("—");
    g_autofree char *size=g_format_size(st.st_size);
    p->description=g_strdup_printf("%s · %s",p->name,size);
    const char *dot=strrchr(path,'.'), *ext=dot?dot+1:"";
    const char *images[]={"png","jpg","jpeg","webp","gif","bmp","tif","tiff",NULL};
    const char *media[]={"mp4","mov","mkv","webm","avi","mp3","wav","flac","ogg","opus","m4a","aac",NULL};
    if(listed(ext,media)) p->kind=TC_PREVIEW_MEDIA;
    else if(st.st_size>64LL*1024*1024) p->text=g_strdup("Preview size limit: 64 MiB. Open this file in its application.");
    else if(listed(ext,images)) {
        int w=0,h=0;
        if(!gdk_pixbuf_get_file_info(path,&w,&h) || w<=0 || h<=0 || w>16384 || h>16384 || (uint64_t)w*h>24000000) {
            g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_NOT_SUPPORTED,"Unsupported image or image exceeds 24 million pixels"); close(fd); return NULL;
        }
        p->image=gdk_pixbuf_new_from_file_at_scale(path,1800,1200,TRUE,error);
        if(!p->image) {close(fd);return NULL;}
        p->width=w;p->height=h;
        const char *orientation=gdk_pixbuf_get_option(p->image,"orientation");
        if(orientation && atoi(orientation)>=5 && atoi(orientation)<=8){p->width=h;p->height=w;}
        GdkPixbuf *oriented=gdk_pixbuf_apply_embedded_orientation(p->image);
        if(oriented) {g_object_unref(p->image);p->image=oriented;}
        p->kind=TC_PREVIEW_IMAGE;
    }
#ifdef HAVE_POPPLER
    else if(!g_ascii_strcasecmp(ext,"pdf")) {
        g_autofree char *uri=g_file_get_uri(file);
        g_autoptr(PopplerDocument) doc=poppler_document_new_from_file(uri,NULL,error);
        if(!doc) {close(fd);return NULL;}
        int n=poppler_document_get_n_pages(doc);
        if(n<1 || n>4096) {g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_NOT_SUPPORTED,"Unsupported PDF page count");close(fd);return NULL;}
        p->pages=n;p->page_index=MIN(page,(unsigned)n-1);
        g_autoptr(PopplerPage) item=poppler_document_get_page(doc,p->page_index);
        if(!item){g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_INVALID_DATA,"Cannot load PDF page");close(fd);return NULL;}
        double w=0,h=0;poppler_page_get_size(item,&w,&h);
        if(!isfinite(w)||!isfinite(h)||w<=0||h<=0) {g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_INVALID_DATA,"Invalid PDF geometry");close(fd);return NULL;}
        p->page_width=w;p->page_height=h;
        double scale=MIN(1600/w,1600/h);
        p->page=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,MAX(1,(int)ceil(w*scale)),MAX(1,(int)ceil(h*scale)));
        cairo_t *cr=cairo_create(p->page);cairo_set_source_rgb(cr,1,1,1);cairo_paint(cr);cairo_scale(cr,scale,scale);poppler_page_render(item,cr);cairo_destroy(cr);
        if(cairo_surface_status(p->page)!=CAIRO_STATUS_SUCCESS) {g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_FAILED,"Cannot render PDF page");close(fd);return NULL;}
        p->kind=TC_PREVIEW_PDF;
    }
#endif
    else {
        g_autofree unsigned char *buffer=g_malloc(256*1024+1);size_t used=0;
        while(used<256*1024) {
            if(g_cancellable_set_error_if_cancelled(cancel,error)){close(fd);return NULL;}
            ssize_t n=read(fd,buffer+used,256*1024-used);
            if(n<0&&errno==EINTR)continue;
            if(n<0){g_set_error(error,G_IO_ERROR,g_io_error_from_errno(errno),"Cannot read preview: %s",g_strerror(errno));close(fd);return NULL;}
            if(!n)break;
            used+=n;
        }
        buffer[used]=0;
        gboolean uncertain=FALSE;
        g_autofree char *type=g_content_type_guess(path,buffer,used,&uncertain);
        if(type && (g_content_type_is_a(type,"text/plain") || g_str_has_prefix(type,"text/") || !strcmp(type,"application/json")) && !memchr(buffer,0,used)) {
            p->text=g_utf8_make_valid((char*)buffer,used);p->kind=TC_PREVIEW_TEXT;
            if(st.st_size>(off_t)used){char *marked=g_strconcat(p->text,"\n\n[Preview truncated at 256 KiB]",NULL);g_free(p->text);p->text=marked;}
        } else p->text=g_strdup("No inline preview for this format. Use Open to view it in its application.");
    }
    close(fd);
    if(g_cancellable_set_error_if_cancelled(cancel,error))return NULL;
    return g_steal_pointer(&p);
}
