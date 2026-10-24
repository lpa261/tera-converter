#include "tc_engine.h"
#include <glib/gstdio.h>
#include <zlib.h>
extern TcEngine tc_engine_image_preserve;
static char *directory;
static void run(const char *const *argv){GError *e=NULL;GSubprocess *p=g_subprocess_newv(argv,G_SUBPROCESS_FLAGS_NONE,&e);g_assert_no_error(e);g_assert_nonnull(p);g_assert_true(g_subprocess_wait_check(p,NULL,&e));g_assert_no_error(e);g_object_unref(p);}
static TcStatusCode optimize(const char *src,const char *dst,GCancellable *cancel){char *error=NULL;TcStatusCode st=tc_engine_image_preserve.convert(src,dst,"comp-image",0,0,cancel,&error,NULL,NULL);if(error)g_test_message("%s",error);g_free(error);return st;}
static void test_png_samples_and_metadata(void){
    const char *modes[]={"RGB","RGBA","I;16","P"};
    for(unsigned i=0;i<G_N_ELEMENTS(modes);i++){
        g_autofree char *name=g_strdup_printf("png-%u.png",i),*outname=g_strdup_printf("png-%u-out.png",i);
        g_autofree char *src=g_build_filename(directory,name,NULL),*dst=g_build_filename(directory,outname,NULL);
        run((const char*[]){"python3","-c","from PIL import Image,PngImagePlugin;import sys;im=Image.new(sys.argv[2],(256,192));m=PngImagePlugin.PngInfo();m.add_text('Title','TeraConvert metadata');im.save(sys.argv[1],compress_level=0,pnginfo=m)",src,modes[i],NULL});
        g_assert_cmpint(optimize(src,dst,NULL),==,TC_STATUS_OK);
        run((const char*[]){"python3","-c","from PIL import Image;import sys;a=Image.open(sys.argv[1]);b=Image.open(sys.argv[2]);assert a.mode==b.mode and a.size==b.size and a.tobytes()==b.tobytes();assert a.info==b.info",src,dst,NULL});
        struct stat a,b;g_assert_cmpint(stat(src,&a),==,0);g_assert_cmpint(stat(dst,&b),==,0);g_assert_cmpint(b.st_size,<,a.st_size);
        g_autofree char *repeat=g_strconcat(dst,".again.png",NULL);
        g_assert_cmpint(optimize(dst,repeat,NULL),==,TC_STATUS_NO_BENEFIT);
    }
}
static void test_jpeg_coefficients_and_metadata(void){
    for(unsigned i=0;i<2;i++){
        g_autofree char *n=g_strdup_printf("jpeg-%u.jpg",i),*o=g_strdup_printf("jpeg-%u-out.jpg",i);
        g_autofree char *src=g_build_filename(directory,n,NULL),*dst=g_build_filename(directory,o,NULL);
        run((const char*[]){"python3","-c","from PIL import Image;import sys;im=Image.new('RGB',(256,192),(30,80,130));e=Image.Exif();e[274]=6;e[271]='TeraConvert';im.save(sys.argv[1],quality=93,optimize=False,progressive=sys.argv[2]=='1',exif=e)",src,i?"1":"0",NULL});
        g_assert_cmpint(optimize(src,dst,NULL),==,TC_STATUS_OK);
        run((const char*[]){"python3","-c","from PIL import Image;import sys;a=Image.open(sys.argv[1]);b=Image.open(sys.argv[2]);assert a.size==b.size and a.tobytes()==b.tobytes();assert a.getexif().tobytes()==b.getexif().tobytes()",src,dst,NULL});
    }
}
static void test_reject_and_alias(void){
    g_autofree char *src=g_build_filename(directory,"reject.png",NULL),*dst=g_build_filename(directory,"reject-out.png",NULL);
    run((const char*[]){"python3","-c","from PIL import Image;import sys;Image.new('RGBA',(24,24)).save(sys.argv[1],compress_level=0)",src,NULL});
    g_autofree char *before=NULL;gsize length=0;g_assert_true(g_file_get_contents(src,&before,&length,NULL));
    g_assert_cmpint(optimize(src,src,NULL),==,TC_STATUS_ERROR);
    g_autofree char *after=NULL;gsize size=0;g_assert_true(g_file_get_contents(src,&after,&size,NULL));g_assert_cmpmem(before,length,after,size);
    g_autoptr(GCancellable) cancel=g_cancellable_new();g_cancellable_cancel(cancel);
    g_assert_cmpint(optimize(src,dst,cancel),==,TC_STATUS_ERROR_CANCELLED);g_assert_false(g_file_test(dst,G_FILE_TEST_EXISTS));
    before[30]^=1;g_assert_true(g_file_set_contents(src,before,length,NULL));
    g_assert_cmpint(optimize(src,dst,NULL),==,TC_STATUS_ERROR_CORRUPT);g_assert_false(g_file_test(dst,G_FILE_TEST_EXISTS));
    run((const char*[]){"python3","-c","from PIL import Image;import sys;im=Image.new('RGBA',(24,24));im.save(sys.argv[1],save_all=True,append_images=[Image.new('RGBA',(24,24),(255,0,0,255))])",src,NULL});
    g_assert_cmpint(optimize(src,dst,NULL),==,TC_STATUS_ERROR_UNSUPPORTED);
}
static void test_malformed_jpeg(void) {
    g_autofree char *src=g_build_filename(directory,"truncated.jpg",NULL),*dst=g_build_filename(directory,"truncated-out.jpg",NULL);
    /* Truncated tables enter libjpeg's longjmp failure path. */
    const unsigned char broken[]={0xff,0xd8,0xff,0xdb,0x00,0x43,0x00,0x01,0x02,0x03,0xff,0xd9};
    g_assert_true(g_file_set_contents(src,(const char*)broken,sizeof(broken),NULL));
    g_assert_cmpint(optimize(src,dst,NULL),==,TC_STATUS_ERROR_CORRUPT);
    g_assert_false(g_file_test(dst,G_FILE_TEST_EXISTS));
}
int main(int argc,char **argv){g_test_init(&argc,&argv,NULL);directory=g_dir_make_tmp("tc-image-preserve-XXXXXX",NULL);g_assert_nonnull(directory);
    g_test_add_func("/image/jpeg-truncated-tables",test_malformed_jpeg);
    g_test_add_func("/image/png-depth-alpha-palette-metadata",test_png_samples_and_metadata);
    g_test_add_func("/image/jpeg-coefficients-exif-progressive",test_jpeg_coefficients_and_metadata);
    g_test_add_func("/image/reject-corrupt-animation-alias-cancel",test_reject_and_alias);
    int result=g_test_run();GDir *d=g_dir_open(directory,0,NULL);const char *name;while((name=g_dir_read_name(d))){g_autofree char *path=g_build_filename(directory,name,NULL);g_unlink(path);}g_dir_close(d);g_rmdir(directory);g_free(directory);return result;
}
