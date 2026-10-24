#include "tc_engine.h"
#include <glib/gstdio.h>
extern TcEngine tc_engine_audio_preserve;
static char *directory;
static void run(const char *const *argv){GError *e=NULL;GSubprocess *p=g_subprocess_newv(argv,G_SUBPROCESS_FLAGS_NONE,&e);g_assert_no_error(e);g_assert_nonnull(p);g_assert_true(g_subprocess_wait_check(p,NULL,&e));g_assert_no_error(e);g_object_unref(p);}
static void test_integer_depth(gconstpointer value){
    int bits=GPOINTER_TO_INT(value);g_autofree char *name=g_strdup_printf("source-%d.wav",bits),*outname=g_strdup_printf("output-%d.flac",bits);
    g_autofree char *src=g_build_filename(directory,name,NULL),*dst=g_build_filename(directory,outname,NULL);
    run((const char*[]){"/usr/bin/ffmpeg","-v","error","-nostdin","-y","-f","lavfi","-i","aevalsrc=0.47*sin(2*PI*991*t)|0.37*sin(2*PI*631*t):s=48000:d=0.25","-c:a",bits==16?"pcm_s16le":"pcm_s24le",src,NULL});
    char *error=NULL;TcStatusCode status=tc_engine_audio_preserve.convert(src,dst,"safe-flac",0,0,NULL,&error,NULL,NULL);if(error)g_test_message("%s",error);g_free(error);g_assert_cmpint(status,==,TC_STATUS_OK);
    run((const char*[]){"python3","-c","import subprocess,sys,json;ff='/usr/bin/ffmpeg';f=lambda p:subprocess.check_output([ff,'-v','error','-i',p,'-map','0:a:0','-c:a','pcm_s32le','-f','s32le','-']);assert f(sys.argv[1])==f(sys.argv[2]);p=json.loads(subprocess.check_output(['/usr/bin/ffprobe','-v','error','-show_streams','-of','json',sys.argv[2]]));assert int(p['streams'][0]['bits_per_raw_sample'])==int(sys.argv[3])",src,dst,bits==16?"16":"24",NULL});
}
static void test_float_and_alias(void){
    g_autofree char *src=g_build_filename(directory,"float.wav",NULL),*dst=g_build_filename(directory,"float.flac",NULL);
    run((const char*[]){"/usr/bin/ffmpeg","-v","error","-nostdin","-y","-f","lavfi","-i","sine=duration=0.1:sample_rate=48000","-c:a","pcm_f32le",src,NULL});
    char *error=NULL;g_assert_cmpint(tc_engine_audio_preserve.convert(src,dst,"safe-flac",0,0,NULL,&error,NULL,NULL),==,TC_STATUS_ERROR_UNSUPPORTED);g_free(error);error=NULL;g_assert_false(g_file_test(dst,G_FILE_TEST_EXISTS));
    g_assert_cmpint(tc_engine_audio_preserve.convert(src,src,"safe-flac",0,0,NULL,&error,NULL,NULL),==,TC_STATUS_ERROR);g_free(error);
}
int main(int argc,char **argv){g_test_init(&argc,&argv,NULL);directory=g_dir_make_tmp("tc-audio-preserve-XXXXXX",NULL);
    g_test_add_data_func("/audio/integer-16-bit-stereo",GINT_TO_POINTER(16),test_integer_depth);g_test_add_data_func("/audio/integer-24-bit-stereo",GINT_TO_POINTER(24),test_integer_depth);g_test_add_func("/audio/reject-float-and-alias",test_float_and_alias);
    int result=g_test_run();GDir *d=g_dir_open(directory,0,NULL);const char *name;while((name=g_dir_read_name(d))){g_autofree char *path=g_build_filename(directory,name,NULL);g_unlink(path);}g_dir_close(d);g_rmdir(directory);g_free(directory);return result;
}
