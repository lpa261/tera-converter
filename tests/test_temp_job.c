#include "tc_temp_job.h"
#include <glib/gstdio.h>
#include <sys/wait.h>
static char *state,*output;
static void test_live_and_crash(void){
    g_autofree char *dest=g_build_filename(output,"out.png",NULL);char *error=NULL;
    TcTempJob *live=tc_temp_job_new(dest,&error);g_assert_null(error);g_assert_nonnull(live);
    g_autofree char *live_dir=g_strdup(tc_temp_job_directory(live));
    g_assert_cmpuint(tc_temp_job_recover(),==,0);g_assert_true(g_file_test(live_dir,G_FILE_TEST_IS_DIR));
    tc_temp_job_finish(&live);g_assert_null(live);g_assert_false(g_file_test(live_dir,G_FILE_TEST_EXISTS));
    g_autofree char *source=g_build_filename(output,"keep.txt",NULL);g_assert_true(g_file_set_contents(source,"preserve",-1,NULL));
    pid_t child=fork();g_assert_cmpint(child,>=,0);
    if(!child){TcTempJob *job=tc_temp_job_new(dest,NULL);if(!job)_exit(2);g_autofree char *link=g_build_filename(tc_temp_job_directory(job),"outside-link",NULL);if(symlink(source,link))_exit(3);_exit(0);}
    int status;g_assert_cmpint(waitpid(child,&status,0),==,child);g_assert_true(WIFEXITED(status));g_assert_cmpint(WEXITSTATUS(status),==,0);
    g_assert_cmpuint(tc_temp_job_recover(),==,1);g_assert_true(g_file_test(source,G_FILE_TEST_IS_REGULAR));g_assert_cmpuint(tc_temp_job_recover(),==,0);
    g_unlink(source);
}
static void test_unknown_retained(void){
    g_autofree char *record=g_build_filename(state,"unknown.job",NULL);g_assert_true(g_file_set_contents(record,"not an owned job record",-1,NULL));
    g_autofree char *foreign=g_build_filename(output,".teraconvert-foreign",NULL);g_assert_cmpint(g_mkdir(foreign,0700),==,0);
    g_assert_cmpuint(tc_temp_job_recover(),==,0);g_assert_true(g_file_test(record,G_FILE_TEST_EXISTS));g_assert_true(g_file_test(foreign,G_FILE_TEST_IS_DIR));
    g_unlink(record);g_rmdir(foreign);
}
int main(int argc,char **argv){g_test_init(&argc,&argv,NULL);state=g_dir_make_tmp("tc-job-state-XXXXXX",NULL);output=g_dir_make_tmp("tc-job-output-XXXXXX",NULL);g_setenv("TC_JOB_STATE_DIR",state,TRUE);
    g_test_add_func("/temp/live-lock-and-crash-recovery",test_live_and_crash);g_test_add_func("/temp/unknown-retained",test_unknown_retained);
    int result=g_test_run();g_rmdir(state);g_rmdir(output);g_free(state);g_free(output);return result;
}
