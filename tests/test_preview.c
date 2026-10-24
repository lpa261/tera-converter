#include "tc_preview.h"
#include <glib/gstdio.h>
static void test_inputs(void) {
    GError *error=NULL;
    g_assert_null(tc_preview_load("https://example.com/test.txt",0,NULL,&error));g_assert_error(error,G_IO_ERROR,G_IO_ERROR_NOT_SUPPORTED);g_clear_error(&error);
    g_autoptr(GCancellable) cancel=g_cancellable_new();g_cancellable_cancel(cancel);
    g_assert_null(tc_preview_load("/tmp",0,cancel,&error));g_assert_error(error,G_IO_ERROR,G_IO_ERROR_CANCELLED);g_clear_error(&error);
    g_assert_null(tc_preview_load("/tmp",0,NULL,&error));g_assert_error(error,G_IO_ERROR,G_IO_ERROR_NOT_SUPPORTED);g_clear_error(&error);
}
static void test_text_image(void) {
    g_autofree char *dir=g_dir_make_tmp("tc-preview-test-XXXXXX",NULL),*text=g_build_filename(dir,"ảnh ' draft.txt",NULL),*image=g_build_filename(dir,"image.png",NULL);
    g_assert_true(g_file_set_contents(text,"Unicode: xin chào",-1,NULL));GError *error=NULL;
    g_autoptr(TcPreview) a=tc_preview_load(text,0,NULL,&error);g_assert_no_error(error);g_assert_cmpint(a->kind,==,TC_PREVIEW_TEXT);g_assert_cmpstr(a->text,==,"Unicode: xin chào");
    g_assert_cmpuint(a->file_size,==,strlen("Unicode: xin chào"));
    g_assert_cmpstr(a->mime_type,==,"text/plain");g_assert_nonnull(a->type_name);
    g_assert_nonnull(a->modified);g_assert_cmpstr(a->name,==,"ảnh ' draft.txt");
    g_unlink(text);g_unlink(image);g_rmdir(dir);
}
static void test_image(void) {
    g_autofree char *dir=g_dir_make_tmp("tc-preview-image-XXXXXX",NULL),*path=g_build_filename(dir,"image.png",NULL);
    cairo_surface_t *surface=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,32,24);
    g_assert_cmpint(cairo_surface_write_to_png(surface,path),==,CAIRO_STATUS_SUCCESS);cairo_surface_destroy(surface);
    GError *error=NULL;g_autoptr(GdkPixbuf) probe=gdk_pixbuf_new_from_file(path,&error);
    if(error && (strstr(error->message,"Operation not permitted")||strstr(error->message,"D-Bus")))g_test_skip("Installed Glycin image backend cannot use D-Bus in this environment");
    else {g_assert_no_error(error);g_autoptr(TcPreview) p=tc_preview_load(path,0,NULL,&error);g_assert_no_error(error);g_assert_nonnull(p);g_assert_cmpint(p->kind,==,TC_PREVIEW_IMAGE);g_assert_cmpint(gdk_pixbuf_get_width(p->image),<=,1800);g_assert_cmpint(p->width,==,32);g_assert_cmpint(p->height,==,24);g_assert_cmpstr(p->mime_type,==,"image/png");}
    g_clear_error(&error);g_unlink(path);g_rmdir(dir);
}
#ifdef HAVE_POPPLER
#include <cairo-pdf.h>
static void test_pdf(void) {
    g_autofree char *dir=g_dir_make_tmp("tc-preview-pdf-XXXXXX",NULL),*path=g_build_filename(dir,"two-pages.pdf",NULL);
    cairo_surface_t *surface=cairo_pdf_surface_create(path,300,400);cairo_t *cr=cairo_create(surface);
    cairo_show_page(cr);cairo_show_page(cr);cairo_destroy(cr);cairo_surface_destroy(surface);
    GError *error=NULL;g_autoptr(TcPreview) p=tc_preview_load(path,1,NULL,&error);g_assert_no_error(error);g_assert_nonnull(p);
    g_assert_cmpint(p->kind,==,TC_PREVIEW_PDF);g_assert_cmpuint(p->pages,==,2);g_assert_cmpuint(p->page_index,==,1);g_assert_cmpfloat(p->page_width,==,300);g_assert_cmpfloat(p->page_height,==,400);g_assert_cmpstr(p->mime_type,==,"application/pdf");g_assert_cmpint(cairo_image_surface_get_height(p->page),<=,1600);
    g_unlink(path);g_rmdir(dir);
}
#endif
static void test_truncated_binary(void) {
    g_autofree char *dir=g_dir_make_tmp("tc-preview-bounds-XXXXXX",NULL),*path=g_build_filename(dir,"large.txt",NULL);
    g_autofree char *bytes=g_malloc0(300*1024);memset(bytes,'a',300*1024);g_assert_true(g_file_set_contents(path,bytes,300*1024,NULL));
    GError *error=NULL;g_autoptr(TcPreview) p=tc_preview_load(path,0,NULL,&error);g_assert_no_error(error);g_assert_nonnull(strstr(p->text,"truncated"));
    g_clear_pointer(&p,tc_preview_free);g_assert_true(g_file_set_contents(path,"\0binary",7,NULL));p=tc_preview_load(path,0,NULL,&error);g_assert_no_error(error);g_assert_cmpint(p->kind,==,TC_PREVIEW_INFO);
    g_unlink(path);g_rmdir(dir);
}
int main(int argc,char **argv){g_test_init(&argc,&argv,NULL);g_test_add_func("/preview/local-regular-and-cancellation",test_inputs);g_test_add_func("/preview/text-unicode-image",test_text_image);g_test_add_func("/preview/text-bound-and-binary",test_truncated_binary);g_test_add_func("/preview/image-backend",test_image);
#ifdef HAVE_POPPLER
 g_test_add_func("/preview/pdf-pagination",test_pdf);
#endif
return g_test_run();}
