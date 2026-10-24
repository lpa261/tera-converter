#include "tc_process_runner.h"

#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>

static char *self_path;
static const TcProcessOptions quick = { .timeout_ms = 3000, .terminate_grace_ms = 80 };

static void write_bytes(int fd, const char *data, gsize size) {
    while (size) {
        ssize_t count = write(fd, data, size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) _exit(99);
        data += count;
        size -= count;
    }
}

static int child_mode(int argc, char **argv) {
    const char *mode = argv[2];
    if(!strcmp(mode,"large-file")){
        int fd=open(argv[3],O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd<0)return 75;char bytes[4096]={0};
        for(unsigned i=0;i<1024;i++)write_bytes(fd,bytes,sizeof(bytes));
        close(fd);return 0;
    }
    if(!strcmp(mode,"multi-file")){
        for(unsigned i=0;i<20;i++){char path[4096];snprintf(path,sizeof(path),"%s/chunk-%u.bin",argv[3],i);int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd<0)return 75;char bytes[2048]={0};write_bytes(fd,bytes,sizeof(bytes));close(fd);usleep(20000);}return 0;
    }
    if(!strcmp(mode,"large-memory")){
        size_t size=192U*1024U*1024U;volatile unsigned char *memory=malloc(size);if(!memory)return 76;
        for(size_t i=0;i<size;i+=4096)memory[i]=1;
        write_bytes(STDOUT_FILENO,"ready",5);for(;;)pause();
    }
    if (!strcmp(mode, "success")) {
        write_bytes(STDOUT_FILENO, argv[3], strlen(argv[3]));
        write_bytes(STDERR_FILENO, "diagnostic", 10);
        return 0;
    }
    if (!strcmp(mode, "fail")) {
        write_bytes(STDERR_FILENO, "specific failure", 16);
        return 23;
    }
    if (!strcmp(mode, "signal")) {
        raise(SIGUSR1);
        return 99;
    }
    if (!strcmp(mode, "flood")) {
        char data[4096];
        memset(data, 'X', sizeof(data));
        for (int i = 0; i < 512; i++) {
            write_bytes(STDOUT_FILENO, data, sizeof(data));
            write_bytes(STDERR_FILENO, data, sizeof(data));
        }
        write_bytes(STDERR_FILENO, "TAIL", 4);
        return 0;
    }
    signal(SIGTERM, SIG_IGN);
    if (!strcmp(mode, "tree") || !strcmp(mode, "orphan")) {
        int ready[2];
        if (pipe(ready) < 0) return 98;
        pid_t child = fork();
        if (child < 0) return 97;
        if (child == 0) {
            close(ready[0]);
            write_bytes(ready[1], "R", 1);
            close(ready[1]);
            for (;;) pause();
        }
        close(ready[1]);
        char byte;
        if (read(ready[0], &byte, 1) != 1) return 96;
        close(ready[0]);
        char pid_text[32];
        int length = snprintf(pid_text, sizeof(pid_text), "%ld\n", (long)child);
        write_bytes(STDOUT_FILENO, pid_text, (gsize)length);
        if (!strcmp(mode, "orphan")) return 0;
    } else {
        write_bytes(STDOUT_FILENO, "ready\n", 6);
    }
    (void)argc;
    for (;;) pause();
}

static void collect_output(const char *data, gsize length, gpointer opaque) {
    g_string_append_len(opaque, data, length);
}

static void assert_reaped(const TcProcessResult *result) {
    int status;
    errno = 0;
    g_assert_cmpint(waitpid(result->pid, &status, WNOHANG), ==, -1);
    g_assert_cmpint(errno, ==, ECHILD);
}

static void test_success(void) {
    const char *literal = "literal $(echo forbidden) ; 'space'";
    const char *argv[] = { self_path, "--child", "success", literal, NULL };
    g_autoptr(GString) output = g_string_new(NULL);
    g_autofree char *error = NULL;
    TcProcessResult result = {0};
    g_assert_cmpint(tc_process_run(argv, &quick, NULL, collect_output, output, &result, &error), ==, TC_STATUS_OK);
    g_assert_null(error);
    g_assert_cmpstr(output->str, ==, literal);
    g_assert_cmpstr(result.stderr_tail, ==, "diagnostic");
    g_assert_true(result.exited);
    g_assert_cmpint(result.exit_code, ==, 0);
    assert_reaped(&result);
    tc_process_result_clear(&result);
}

static void test_failure(void) {
    const char *argv[] = { self_path, "--child", "fail", NULL };
    g_autofree char *error = NULL;
    TcProcessResult result = {0};
    g_assert_cmpint(tc_process_run(argv, &quick, NULL, NULL, NULL, &result, &error), ==, TC_STATUS_ERROR);
    g_assert_true(result.exited);
    g_assert_cmpint(result.exit_code, ==, 23);
    g_assert_nonnull(strstr(error, "specific failure"));
    assert_reaped(&result);
    tc_process_result_clear(&result);
}

static void test_signal(void) {
    const char *argv[] = { self_path, "--child", "signal", NULL };
    g_autofree char *error = NULL;
    TcProcessResult result = {0};
    g_assert_cmpint(tc_process_run(argv, &quick, NULL, NULL, NULL, &result, &error), ==, TC_STATUS_ERROR);
    g_assert_false(result.exited);
    g_assert_cmpint(result.exit_code, ==, -1);
    g_assert_cmpint(result.term_signal, ==, SIGUSR1);
    g_assert_nonnull(strstr(error, "signal"));
    assert_reaped(&result);
    tc_process_result_clear(&result);
}

typedef struct {
    GCancellable *cancellable;
    GString *output;
    gboolean cancel_on_ready;
} ReadyState;

static void ready_output(const char *data, gsize length, gpointer opaque) {
    ReadyState *state = opaque;
    g_string_append_len(state->output, data, length);
    if (state->cancel_on_ready && strchr(state->output->str, '\n')) g_cancellable_cancel(state->cancellable);
}

static void test_cancel_stubborn(void) {
    const char *argv[] = { self_path, "--child", "stubborn", NULL };
    g_autoptr(GCancellable) cancellable = g_cancellable_new();
    g_autoptr(GString) output = g_string_new(NULL);
    ReadyState ready = { cancellable, output, TRUE };
    TcProcessResult result = {0};
    gint64 start = g_get_monotonic_time();
    g_assert_cmpint(tc_process_run(argv, &quick, cancellable, ready_output, &ready, &result, NULL), ==, TC_STATUS_ERROR_CANCELLED);
    g_assert_cmpint(g_get_monotonic_time() - start, <, 2000000);
    g_assert_cmpint(result.term_signal, ==, SIGKILL);
    assert_reaped(&result);
    tc_process_result_clear(&result);
}

static void test_timeout(void) {
    const char *argv[] = { self_path, "--child", "stubborn", NULL };
    const TcProcessOptions deadline = { .timeout_ms = 150, .terminate_grace_ms = 80 };
    TcProcessResult result = {0};
    gint64 start = g_get_monotonic_time();
    g_assert_cmpint(tc_process_run(argv, &deadline, NULL, NULL, NULL, &result, NULL), ==, TC_STATUS_ERROR_TIMEOUT);
    g_assert_cmpint(g_get_monotonic_time() - start, <, 2000000);
    g_assert_cmpint(result.term_signal, ==, SIGKILL);
    assert_reaped(&result);
    tc_process_result_clear(&result);
}

static void reap_descendant(pid_t pid) {
    int status = 0;
    gint64 deadline = g_get_monotonic_time() + 2000000;
    pid_t reaped;
    do {
        reaped = waitpid(pid, &status, WNOHANG);
        if (reaped == 0) g_usleep(1000);
    } while (reaped == 0 && g_get_monotonic_time() < deadline);
    g_assert_cmpint(reaped, ==, pid);
    g_assert_true(WIFSIGNALED(status));
    g_assert_cmpint(WTERMSIG(status), ==, SIGKILL);
}

static void test_process_tree(gconstpointer mode) {
    /* Adopt grandchildren only in the test, to verify group cleanup and reap
     * them without relying on container PID 1's zombie handling. */
    g_assert_cmpint(prctl(PR_SET_CHILD_SUBREAPER, 1), ==, 0);
    const char *argv[] = { self_path, "--child", mode, NULL };
    g_autoptr(GCancellable) cancellable = g_cancellable_new();
    g_autoptr(GString) output = g_string_new(NULL);
    gboolean cancel = !strcmp(mode, "tree");
    ReadyState ready = { cancellable, output, cancel };
    TcProcessResult result = {0};
    gint64 start = g_get_monotonic_time();
    g_assert_cmpint(tc_process_run(argv, &quick, cancellable, ready_output, &ready, &result, NULL), ==,
                    cancel ? TC_STATUS_ERROR_CANCELLED : TC_STATUS_OK);
    g_assert_cmpint(g_get_monotonic_time() - start, <, 2000000);
    pid_t descendant = (pid_t)g_ascii_strtoll(output->str, NULL, 10);
    g_assert_cmpint(descendant, >, 0);
    assert_reaped(&result);
    reap_descendant(descendant);
    g_assert_cmpint(prctl(PR_SET_CHILD_SUBREAPER, 0), ==, 0);
    tc_process_result_clear(&result);
}

static void count_output(const char *data, gsize length, gpointer opaque) {
    (void)data;
    *(gsize *)opaque += length;
}

static void test_output_bound(void) {
    const char *argv[] = { self_path, "--child", "flood", NULL };
    TcProcessResult result = {0};
    gsize count = 0;
    g_assert_cmpint(tc_process_run(argv, &quick, NULL, count_output, &count, &result, NULL), ==, TC_STATUS_OK);
    g_assert_cmpuint(count, ==, 2 * 1024 * 1024);
    g_assert_cmpuint(strlen(result.stderr_tail), ==, 16 * 1024);
    g_assert_true(result.stderr_truncated);
    g_assert_true(g_str_has_suffix(result.stderr_tail, "TAIL"));
    assert_reaped(&result);
    tc_process_result_clear(&result);
}

static void test_no_spawn(void) {
    const char *argv[] = { "/missing-tc-runner-executable", NULL };
    g_autoptr(GCancellable) cancellable = g_cancellable_new();
    TcProcessResult result = {0};
    g_cancellable_cancel(cancellable);
    g_assert_cmpint(tc_process_run(argv, &quick, cancellable, NULL, NULL, &result, NULL), ==, TC_STATUS_ERROR_CANCELLED);
    g_assert_cmpint(result.pid, ==, 0);
    g_assert_cmpint(tc_process_run(argv, &quick, NULL, NULL, NULL, &result, NULL), ==, TC_STATUS_ERROR);
    g_assert_cmpint(result.pid, ==, 0);
    const TcProcessOptions invalid = {0};
    g_assert_cmpint(tc_process_run(argv, &invalid, NULL, NULL, NULL, NULL, NULL), ==, TC_STATUS_ERROR);
}

static guint open_fd_count(void) {
    g_autoptr(GDir) dir = g_dir_open("/proc/self/fd", 0, NULL);
    g_assert_nonnull(dir);
    guint count = 0;
    while (g_dir_read_name(dir)) count++;
    return count;
}

static void test_fd_cleanup(void) {
    const char *argv[] = { self_path, "--child", "fail", NULL };
    guint before = open_fd_count();
    for (int i = 0; i < 32; i++) {
        g_assert_cmpint(tc_process_run(argv, &quick, NULL, NULL, NULL, NULL, NULL), ==, TC_STATUS_ERROR);
    }
    g_assert_cmpuint(open_fd_count(), ==, before);
}

static void cleanup_directory(const char *path){GDir *d=g_dir_open(path,0,NULL);const char *name;while((name=g_dir_read_name(d))){g_autofree char *p=g_build_filename(path,name,NULL);unlink(p);}g_dir_close(d);rmdir(path);}
static void test_output_budgets(void){
    g_autofree char *dir=g_dir_make_tmp("tc-runner-budget-XXXXXX",NULL),*path=g_build_filename(dir,"candidate",NULL);
    const char *single[]={self_path,"--child","large-file",path,NULL};
    tc_process_set_budget(path,4096,0);TcProcessResult r={0};char *error=NULL;
    g_assert_cmpint(tc_process_run(single,&quick,NULL,NULL,NULL,&r,&error),==,TC_STATUS_ERROR_NO_SPACE);g_assert_nonnull(error);g_free(error);error=NULL;assert_reaped(&r);tc_process_result_clear(&r);
    struct stat st;g_assert_cmpint(stat(path,&st),==,0);g_assert_cmpint(st.st_size,<=,4096);unlink(path);
    const char *multiple[]={self_path,"--child","multi-file",dir,NULL};
    g_assert_cmpint(tc_process_run(multiple,&quick,NULL,NULL,NULL,&r,&error),==,TC_STATUS_ERROR_NO_SPACE);g_assert_nonnull(error);g_free(error);assert_reaped(&r);tc_process_result_clear(&r);
    tc_process_set_budget(NULL,0,0);cleanup_directory(dir);
}
static void test_rss_budget(void){
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    g_test_skip("Sanitizer runtime reserves a large virtual address range; RSS/AS child-cap test runs in normal build");
#else
    g_autofree char *dir=g_dir_make_tmp("tc-runner-rss-XXXXXX",NULL),*path=g_build_filename(dir,"candidate",NULL);
    tc_process_set_budget(path,16U*1024U*1024U,512ULL*1024*1024);
    const char *control[]={self_path,"--child","success","literal",NULL};
    g_assert_cmpint(tc_process_run(control,&quick,NULL,NULL,NULL,NULL,NULL),==,TC_STATUS_OK);
    const char *large[]={self_path,"--child","large-memory",NULL};TcProcessResult r={0};char *error=NULL;
    g_assert_cmpint(tc_process_run(large,&quick,NULL,NULL,NULL,&r,&error),==,TC_STATUS_ERROR_MEM_LIMIT);g_assert_nonnull(error);g_free(error);assert_reaped(&r);tc_process_result_clear(&r);
    tc_process_set_budget(NULL,0,0);cleanup_directory(dir);
#endif
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "--child")) return child_mode(argc, argv);
    g_test_init(&argc, &argv, NULL);
    self_path = g_file_read_link("/proc/self/exe", NULL);
    g_assert_nonnull(self_path);
    g_test_add_func("/runner/success-and-literal-argv", test_success);
    g_test_add_func("/runner/exit-diagnostic", test_failure);
    g_test_add_func("/runner/signal-status", test_signal);
    g_test_add_func("/runner/cancel-escalation", test_cancel_stubborn);
    g_test_add_func("/runner/timeout-escalation", test_timeout);
    g_test_add_data_func("/runner/cancel-process-group", "tree", test_process_tree);
    g_test_add_data_func("/runner/leader-exits-before-descendant", "orphan", test_process_tree);
    g_test_add_func("/runner/bounded-output-no-deadlock", test_output_bound);
    g_test_add_func("/runner/no-spawn", test_no_spawn);
    g_test_add_func("/runner/fd-cleanup", test_fd_cleanup);
    g_test_add_func("/runner/hard-file-and-aggregate-workspace-budgets",test_output_budgets);
    g_test_add_func("/runner/live-process-tree-rss-budget",test_rss_budget);
    int status = g_test_run();
    g_free(self_path);
    return status;
}
