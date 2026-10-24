#include "tc_engine.h"
#include "tc_media_probe.h"
#include <glib/gstdio.h>
extern TcEngine tc_engine_media_remux;
static char *directory,*source;
static void run(const char *const *argv){GError *e=NULL;GSubprocess *p=g_subprocess_newv(argv,G_SUBPROCESS_FLAGS_NONE,&e);g_assert_no_error(e);g_assert_nonnull(p);g_assert_true(g_subprocess_wait_check(p,NULL,&e));g_assert_no_error(e);g_object_unref(p);}
static void test_preserve_packets(void){
    g_autofree char *target=g_build_filename(directory,"optimized.mp4",NULL);char *error=NULL;
    TcStatusCode status=tc_engine_media_remux.convert(source,target,"comp-remux",0,0,NULL,&error,NULL,NULL);
    if(error)g_test_message("%s",error);
    g_free(error);g_assert_cmpint(status,==,TC_STATUS_OK);
    GPtrArray *a=NULL,*b=NULL;g_assert_cmpint(tc_media_packet_manifest(source,NULL,&a,NULL),==,TC_STATUS_OK);g_assert_cmpint(tc_media_packet_manifest(target,NULL,&b,NULL),==,TC_STATUS_OK);
    g_assert_cmpuint(a->len,==,b->len);for(guint i=0;i<a->len;i++)g_assert_cmpstr(g_ptr_array_index(a,i),==,g_ptr_array_index(b,i));g_ptr_array_unref(a);g_ptr_array_unref(b);
    struct stat in,out;stat(source,&in);stat(target,&out);g_assert_cmpint(out.st_size,<,in.st_size);
}
static void test_reject_opaque_alias_cancel(void){
    char *error=NULL;g_autofree char *target=g_build_filename(directory,"opaque.mp4",NULL),*input=g_build_filename(directory,"opaque-input.mp4",NULL);
    run((const char*[]){"python3","-c","import sys,struct;a=open(sys.argv[1],'rb').read();open(sys.argv[2],'wb').write(a+struct.pack('>I4s',24,b'uuid')+bytes(16))",source,input,NULL});
    g_assert_cmpint(tc_engine_media_remux.convert(input,target,"comp-remux",0,0,NULL,&error,NULL,NULL),==,TC_STATUS_ERROR_UNSUPPORTED);g_free(error);error=NULL;g_assert_false(g_file_test(target,G_FILE_TEST_EXISTS));
    g_assert_cmpint(tc_engine_media_remux.convert(source,source,"comp-remux",0,0,NULL,&error,NULL,NULL),==,TC_STATUS_ERROR);g_free(error);error=NULL;
    g_autoptr(GCancellable) c=g_cancellable_new();g_cancellable_cancel(c);
    g_assert_cmpint(tc_engine_media_remux.convert(source,target,"comp-remux",0,0,c,&error,NULL,NULL),==,TC_STATUS_ERROR_CANCELLED);g_free(error);
}
int main(int argc,char **argv){g_test_init(&argc,&argv,NULL);directory=g_dir_make_tmp("tc-remux-XXXXXX",NULL);source=g_build_filename(directory,"source.mp4",NULL);
    run((const char*[]){"/usr/bin/ffmpeg","-v","error","-nostdin","-y","-f","lavfi","-i","testsrc2=size=160x120:rate=12:duration=0.5","-f","lavfi","-i","sine=duration=0.5:sample_rate=48000","-f","lavfi","-i","sine=frequency=660:duration=0.5:sample_rate=44100","-map","0:v","-map","1:a","-map","2:a","-c:v","libx264","-threads","2","-c:a","aac","-movflags","+faststart",source,NULL});
    run((const char*[]){"python3","-c","import sys,struct;f=open(sys.argv[1],'ab');f.write(struct.pack('>I4s',262144,b'free')+bytes(262136));f.close()",source,NULL});
    g_test_add_func("/remux/packets-timestamps-metadata-and-saving",test_preserve_packets);g_test_add_func("/remux/reject-opaque-alias-and-cancel",test_reject_opaque_alias_cancel);
    int result=g_test_run();GDir *d=g_dir_open(directory,0,NULL);const char *name;while((name=g_dir_read_name(d))){g_autofree char *path=g_build_filename(directory,name,NULL);g_unlink(path);}g_dir_close(d);g_rmdir(directory);g_free(directory);g_free(source);return result;
}
