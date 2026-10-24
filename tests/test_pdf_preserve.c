#include "tc_engine.h"
#include <glib/gstdio.h>
extern TcEngine tc_engine_pdf_preserve;
static char *directory,*source;
static void run(const char *const *argv){GError *error=NULL;GSubprocess *p=g_subprocess_newv(argv,G_SUBPROCESS_FLAGS_NONE,&error);g_assert_no_error(error);g_assert_nonnull(p);g_assert_true(g_subprocess_wait_check(p,NULL,&error));g_assert_no_error(error);g_object_unref(p);}
static char *hash(const char *path){char *data=NULL;gsize size=0;g_assert_true(g_file_get_contents(path,&data,&size,NULL));char *sum=g_compute_checksum_for_data(G_CHECKSUM_SHA256,(guchar*)data,size);g_free(data);return sum;}
static TcStatusCode convert(const char *src,const char *dst,GCancellable *cancel){char *error=NULL;TcStatusCode s=tc_engine_pdf_preserve.convert(src,dst,"comp-pdf-safe",0,0,cancel,&error,NULL,NULL);if(error)g_test_message("%s",error);g_free(error);return s;}
static void test_static_pdf(void){
    g_autofree char *target=g_build_filename(directory,"optimized.pdf",NULL),*before=hash(source);
    g_assert_cmpint(convert(source,target,NULL),==,TC_STATUS_OK);
    g_autofree char *after=hash(source);g_assert_cmpstr(before,==,after);
    run((const char*[]){"python3","-c","import pikepdf,sys;a=pikepdf.open(sys.argv[1]);b=pikepdf.open(sys.argv[2]);assert len(a.pages)==len(b.pages)==2",source,target,NULL});
}
static void test_unsupported(gconstpointer data){
    const char *kind=data;g_autofree char *src=g_build_filename(directory,kind,NULL),*dst=g_strconcat(src,".out.pdf",NULL);
    run((const char*[]){"python3","-c","import pikepdf as q,sys;p=q.open(sys.argv[1]);k=sys.argv[3];\nif k=='encrypted':p.save(sys.argv[2],encryption=q.Encryption(owner='owner',user='secret'))\nelse:\n p.Root.AcroForm=q.Dictionary(Fields=q.Array());p.save(sys.argv[2])",source,src,kind,NULL});
    g_assert_true(g_file_set_contents(dst,"preserve-existing",-1,NULL));g_autofree char *before=hash(dst);
    g_assert_cmpint(convert(src,dst,NULL),==,TC_STATUS_ERROR_UNSUPPORTED);g_autofree char *after=hash(dst);g_assert_cmpstr(before,==,after);
}
static void test_alias_cancel_missing(void){
    g_autofree char *before=hash(source);g_assert_cmpint(convert(source,source,NULL),==,TC_STATUS_ERROR);g_autofree char *after=hash(source);g_assert_cmpstr(before,==,after);
    g_autofree char *out=g_build_filename(directory,"cancelled.pdf",NULL);g_autoptr(GCancellable) cancel=g_cancellable_new();g_cancellable_cancel(cancel);
    g_assert_cmpint(convert(source,out,cancel),==,TC_STATUS_ERROR_CANCELLED);g_assert_false(g_file_test(out,G_FILE_TEST_EXISTS));
    g_autofree char *helper=g_strdup(g_getenv("TC_HELPER_DIR"));g_setenv("TC_HELPER_DIR","/nonexistent-teraconvert-helpers",TRUE);
    g_assert_cmpint(convert(source,out,NULL),==,TC_STATUS_ERROR_UNSUPPORTED);g_setenv("TC_HELPER_DIR",helper,TRUE);
}
int main(int argc,char **argv){g_test_init(&argc,&argv,NULL);directory=g_dir_make_tmp("tc-pdf-preserve-XXXXXX",NULL);source=g_build_filename(directory,"source.pdf",NULL);
    g_autofree char *arg=g_strdup_printf("-sOutputFile=%s",source);
    run((const char*[]){"gs","-q","-dBATCH","-dNOPAUSE","-sDEVICE=pdfwrite",arg,"-c","/Helvetica findfont 20 scalefont setfont 50 700 moveto (TeraConvert page 1) show showpage /Helvetica findfont 20 scalefont setfont 50 700 moveto (TeraConvert page 2) show showpage",NULL});
    g_test_add_func("/pdf/static-stream-text-all-page-validation",test_static_pdf);g_test_add_data_func("/pdf/reject-encrypted","encrypted",test_unsupported);g_test_add_data_func("/pdf/reject-form","form",test_unsupported);g_test_add_func("/pdf/alias-cancel-missing-helper",test_alias_cancel_missing);
    int result=g_test_run();GDir *d=g_dir_open(directory,0,NULL);const char *name;while((name=g_dir_read_name(d))){g_autofree char *path=g_build_filename(directory,name,NULL);g_unlink(path);}g_dir_close(d);g_rmdir(directory);g_free(directory);g_free(source);return result;
}
