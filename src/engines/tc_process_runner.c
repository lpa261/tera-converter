#include "tc_process_runner.h"

#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <dirent.h>
#include <limits.h>

#define TC_PROCESS_STDERR_LIMIT (16U * 1024U)
#define TC_PROCESS_DRAIN_LIMIT (64U * 1024U)

static _Thread_local const char *job_output;
static _Thread_local uint64_t job_output_limit, job_address_limit;
void tc_process_set_budget(const char *path,uint64_t output_bytes,uint64_t address_bytes) {
    job_output=path;job_output_limit=output_bytes;job_address_limit=address_bytes;
}
static bool directory_usage(int fd,unsigned depth,uint64_t limit,uint64_t *bytes,unsigned *entries){
    if(depth>16)return false;
    int copy=dup(fd);DIR *dir=copy>=0?fdopendir(copy):NULL;
    if(!dir){if(copy>=0)close(copy);return false;}
    bool ok=true;struct dirent *entry;
    while((entry=readdir(dir))){
        if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;
        struct stat st;
        if(++*entries>10000){ok=false;break;}
        if(fstatat(fd,entry->d_name,&st,AT_SYMLINK_NOFOLLOW)){
            if(errno==ENOENT)continue; /* helper removed a transient scratch file */
            ok=false;break;
        }
        if(S_ISDIR(st.st_mode)){
            int child=openat(fd,entry->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            if(child<0&&errno==ENOENT)continue;
            ok=child>=0&&directory_usage(child,depth+1,limit,bytes,entries);if(child>=0)close(child);if(!ok)break;
        }else if(S_ISREG(st.st_mode)){
            if(st.st_size<0||(uint64_t)st.st_size>limit-*bytes){ok=false;break;}*bytes+=(uint64_t)st.st_size;
        }
    }
    closedir(dir);return ok;
}
static bool process_memory(pid_t pid,pid_t group,unsigned depth,uint64_t limit,uint64_t *bytes,unsigned *count){
    if(depth>32||++*count>1024)return false;
    if(getpgid(pid)!=group)return pid!=group;
    char path[96];g_snprintf(path,sizeof(path),"/proc/%ld/statm",(long)pid);
    FILE *file=fopen(path,"r");if(!file)return pid!=group&&errno==ENOENT;
    unsigned long long total=0,resident=0;int got=fscanf(file,"%llu %llu",&total,&resident);fclose(file);
    long page=sysconf(_SC_PAGESIZE);if(got!=2||page<=0||resident>(limit-*bytes)/(uint64_t)page)return false;
    *bytes+=(uint64_t)resident*(uint64_t)page;
    g_snprintf(path,sizeof(path),"/proc/%ld/task",(long)pid);
    DIR *tasks=opendir(path);if(!tasks)return pid!=group&&errno==ENOENT;
    bool ok=true;struct dirent *task;unsigned threads=0;
    while(ok&&(task=readdir(tasks))){
        if(!g_ascii_isdigit(task->d_name[0]))continue;
        if(++threads>4096){ok=false;break;}
        char child_path[128];
        int length=g_snprintf(child_path,sizeof(child_path),"%s/%s/children",path,task->d_name);
        if(length<0 || (size_t)length>=sizeof(child_path)){ok=false;break;}
        file=fopen(child_path,"r");
        if(!file){if(errno==ENOENT)continue;ok=false;break;}
        long child;
        while(fscanf(file,"%ld",&child)==1){
            if(child<=0||child>INT_MAX||!process_memory((pid_t)child,group,depth+1,limit,bytes,count)){ok=false;break;}
        }
        fclose(file);
    }
    closedir(tasks);return ok;
}
static void apply_limit(int resource,uint64_t bytes) {
    if(!bytes)return;
    struct rlimit current;
    if(getrlimit(resource,&current))_exit(126);
    rlim_t bound=(rlim_t)bytes;
    if((uint64_t)bound!=bytes)_exit(126);
    if(current.rlim_max!=RLIM_INFINITY && bound>current.rlim_max)bound=current.rlim_max;
    current.rlim_cur=current.rlim_max=bound;
    if(setrlimit(resource,&current))_exit(126);
}

static void child_setup(gpointer data) {
    const TcProcessOptions *limits=data;
    /* Only async-signal-safe calls are allowed between fork and exec. */
    if (setpgid(0, 0) != 0) _exit(127);
    apply_limit(RLIMIT_FSIZE,limits->max_file_bytes);
    apply_limit(RLIMIT_AS,limits->max_address_bytes);
}

void tc_process_result_clear(TcProcessResult *result) {
    if (!result) return;
    g_free(result->stderr_tail);
    memset(result, 0, sizeof(*result));
}

static gboolean make_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static void append_stderr(GString *tail, const char *data, gsize size,
                          gboolean *truncated) {
    if (tail->len + size > TC_PROCESS_STDERR_LIMIT) {
        g_string_erase(tail, 0, tail->len + size - TC_PROCESS_STDERR_LIMIT);
        *truncated = TRUE;
    }
    g_string_append_len(tail, data, size);
}

/* Limit each drain so a child continuously writing cannot starve cancellation
 * or the other pipe. No pipe, including stderr, is allowed to block the child. */
static gboolean drain_pipe(int *fd, TcProcessOutputFunc callback,
                            gpointer user_data, GString *tail,
                            gboolean *truncated, gsize budget) {
    gsize drained = 0;
    while (*fd >= 0 && drained < budget) {
        char buf[4096];
        ssize_t count = read(*fd, buf, sizeof(buf));
        if (count > 0) {
            drained += (gsize)count;
            if (callback) callback(buf, (gsize)count, user_data);
            if (tail) append_stderr(tail, buf, (gsize)count, truncated);
        } else if (count == 0) {
            close(*fd);
            *fd = -1;
        } else {
            if (errno == EINTR) return TRUE;  /* Recheck cancellation outside. */
            if (errno == EAGAIN || errno == EWOULDBLOCK) return TRUE;
            return FALSE;
        }
    }
    return TRUE;
}

static void signal_group(GPid pid, int signum) {
    /* The leader has not been reaped, so its PID/group ID cannot be reused. */
    if (kill(-pid, signum) < 0 && errno == ESRCH) kill(pid, signum);
}

TcStatusCode tc_process_run(const char *const *argv,
                            const TcProcessOptions *options,
                            GCancellable *cancellable,
                            TcProcessOutputFunc stdout_cb,
                            gpointer user_data,
                            TcProcessResult *result,
                            char **error_out) {
    TcProcessResult local = { .exit_code = -1 };
    if (result) *result = local;
    if (!argv || !argv[0] || !options || options->timeout_ms <= 0 ||
        options->timeout_ms > G_MAXINT64 / 1000 || options->terminate_grace_ms > 10000) {
        if (error_out) *error_out = g_strdup("Invalid process arguments or deadline");
        return TC_STATUS_ERROR;
    }
    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
        if (error_out) *error_out = g_strdup("Operation cancelled before starting process");
        return TC_STATUS_ERROR_CANCELLED;
    }

    int out_fd = -1, err_fd = -1;
    TcProcessOptions child_limits=*options;
    if(job_output_limit && (!child_limits.max_file_bytes || child_limits.max_file_bytes>job_output_limit))child_limits.max_file_bytes=job_output_limit;
    if(job_address_limit && (!child_limits.max_address_bytes || child_limits.max_address_bytes>job_address_limit))child_limits.max_address_bytes=job_address_limit;
    g_auto(GStrv) environment=NULL;
    g_autofree char *working_directory=job_output?g_path_get_dirname(job_output):NULL;
    if(working_directory)environment=g_environ_setenv(g_get_environ(),"TMPDIR",working_directory,TRUE);
    g_autoptr(GError) spawn_error = NULL;
    if (!g_spawn_async_with_pipes(NULL, (char **)argv, environment,
                                 G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD |
                                 G_SPAWN_CLOEXEC_PIPES,
                                 child_setup, &child_limits, &local.pid, NULL,
                                 &out_fd, &err_fd, &spawn_error)) {
        if (error_out) *error_out = g_strdup(spawn_error->message);
        return TC_STATUS_ERROR;
    }

    TcStatusCode outcome = TC_STATUS_OK;
    g_autofree char *failure = NULL;
    g_autoptr(GString) stderr_tail = g_string_sized_new(TC_PROCESS_STDERR_LIMIT);
    gint64 started = g_get_monotonic_time();
    gint64 stopping_at = 0;
    gboolean stopping = FALSE, killed = FALSE, child_owned = TRUE;
    gint64 last_budget_check=0;
    if (!make_nonblocking(out_fd) || !make_nonblocking(err_fd)) {
        failure = g_strdup_printf("Cannot configure process pipes: %s", g_strerror(errno));
        outcome = TC_STATUS_ERROR;
    }

    for (;;) {
        gint64 now = g_get_monotonic_time();
        if (!stopping) {
            if (cancellable && g_cancellable_is_cancelled(cancellable)) {
                outcome = TC_STATUS_ERROR_CANCELLED;
            } else if (now - started >= options->timeout_ms * 1000) {
                outcome = TC_STATUS_ERROR_TIMEOUT;
            }
            if(outcome==TC_STATUS_OK && job_output && job_output_limit && now-last_budget_check>=100000) {
                last_budget_check=now;
                struct stat output;
                if(stat(job_output,&output)==0 && output.st_size>=0 && (uint64_t)output.st_size>job_output_limit) {
                    outcome=TC_STATUS_ERROR_NO_SPACE;
                    failure=g_strdup("Candidate exceeded the reserved output budget");
                }
                g_autofree char *directory=g_path_get_dirname(job_output);
                struct statvfs space;
                if(statvfs(directory,&space)!=0 || !space.f_frsize ||
                    (space.f_bavail<=UINT64_MAX/space.f_frsize && (uint64_t)space.f_bavail*space.f_frsize<TC_MIN_FREE_DISK_BYTES)) {
                    outcome=TC_STATUS_ERROR_NO_SPACE;
                    if(!failure)failure=g_strdup("Output filesystem no longer has emergency headroom");
                }
                uint64_t used=0;unsigned entries=0;
                int directory_fd=open(directory,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
                bool within=directory_fd>=0&&directory_usage(directory_fd,0,job_output_limit,&used,&entries);
                if(directory_fd>=0)close(directory_fd);
                if(!within){outcome=TC_STATUS_ERROR_NO_SPACE;if(!failure)failure=g_strdup("Job workspace exceeded its disk budget or cannot be measured");}
                if(job_address_limit){
                    uint64_t rss=0;unsigned processes=0;
                    if(!process_memory(local.pid,local.pid,0,job_address_limit/4,&rss,&processes)){
                        outcome=TC_STATUS_ERROR_MEM_LIMIT;if(!failure)failure=g_strdup("Process tree exceeded its RSS budget or cannot be measured");
                    }
                }
            }
            if (outcome != TC_STATUS_OK) {
                stopping = TRUE;
                stopping_at = now;
                signal_group(local.pid, SIGTERM);
                /* A setup failure may have left a blocking pipe. */
                if (failure) {
                    close(out_fd); out_fd = -1;
                    close(err_fd); err_fd = -1;
                }
            }
        }
        if (stopping && !killed &&
            now - stopping_at >= (gint64)options->terminate_grace_ms * 1000) {
            signal_group(local.pid, SIGKILL);
            killed = TRUE;
        }

        if (!drain_pipe(&out_fd, stdout_cb, user_data, NULL, NULL, TC_PROCESS_DRAIN_LIMIT) ||
            !drain_pipe(&err_fd, NULL, NULL, stderr_tail, &local.stderr_truncated, TC_PROCESS_DRAIN_LIMIT)) {
            if (!failure) failure = g_strdup_printf("Cannot read process output: %s", g_strerror(errno));
            if (!stopping) outcome = TC_STATUS_ERROR;
        }
        if (!stopping && cancellable && g_cancellable_is_cancelled(cancellable))
            outcome = TC_STATUS_ERROR_CANCELLED;

        /* Observe exit without reaping: descendants may still hold our pipes.
         * Keeping the zombie reserves the process group ID until cleanup. */
        siginfo_t info = {0};
        if (waitid(P_PID, local.pid, &info, WEXITED | WNOHANG | WNOWAIT) == 0) {
            if (info.si_pid == local.pid) {
                signal_group(local.pid, SIGKILL);
                /* Drain data already buffered at exit. The limit is the actual
                 * pipe capacity, so a descendant cannot keep us reading forever. */
                int out_capacity = out_fd >= 0 ? fcntl(out_fd, F_GETPIPE_SZ) : 0;
                int err_capacity = err_fd >= 0 ? fcntl(err_fd, F_GETPIPE_SZ) : 0;
                gboolean output_drained = drain_pipe(&out_fd, stdout_cb, user_data, NULL, NULL,
                    out_capacity > 0 ? (gsize)out_capacity : TC_PROCESS_DRAIN_LIMIT);
                gboolean error_drained = drain_pipe(&err_fd, NULL, NULL, stderr_tail, &local.stderr_truncated,
                    err_capacity > 0 ? (gsize)err_capacity : TC_PROCESS_DRAIN_LIMIT);
                if ((!output_drained || !error_drained) && !stopping) {
                    if (!failure) failure = g_strdup("Cannot drain buffered process output");
                    outcome = TC_STATUS_ERROR;
                }
                if (!stopping && cancellable && g_cancellable_is_cancelled(cancellable))
                    outcome = TC_STATUS_ERROR_CANCELLED;
                break;
            }
        } else if (errno != EINTR) {
            if (errno == ECHILD) child_owned = FALSE;
            if (!failure) failure = g_strdup_printf("Cannot observe process exit: %s", g_strerror(errno));
            outcome = TC_STATUS_ERROR;
            break;
        }

        struct pollfd pipes[2] = {
            { .fd = out_fd, .events = POLLIN },
            { .fd = err_fd, .events = POLLIN }
        };
        if (poll(pipes, 2, 20) < 0 && errno != EINTR) {
            if (!failure) failure = g_strdup_printf("Cannot poll process pipes: %s", g_strerror(errno));
            if (!stopping) outcome = TC_STATUS_ERROR;
        }
    }

    if (child_owned) {
        signal_group(local.pid, SIGKILL);
        int wait_status = 0;
        pid_t reaped;
        do { reaped = waitpid(local.pid, &wait_status, 0); } while (reaped < 0 && errno == EINTR);
        if (reaped == local.pid) {
            local.exited = WIFEXITED(wait_status);
            if (local.exited) local.exit_code = WEXITSTATUS(wait_status);
            if (WIFSIGNALED(wait_status)) local.term_signal = WTERMSIG(wait_status);
        } else {
            if (!failure) failure = g_strdup_printf("Cannot reap process: %s", g_strerror(errno));
            outcome = TC_STATUS_ERROR;
        }
    }
    if (out_fd >= 0) close(out_fd);
    if (err_fd >= 0) close(err_fd);
    g_spawn_close_pid(local.pid);

    for (gsize i = 0; i < stderr_tail->len; i++) {
        if (stderr_tail->str[i] == '\0') stderr_tail->str[i] = '?';
    }
    local.stderr_tail = g_utf8_make_valid(stderr_tail->str, stderr_tail->len);
    if (outcome == TC_STATUS_OK && (!local.exited || local.exit_code != 0)) outcome = TC_STATUS_ERROR;
    if(local.term_signal==SIGXFSZ && outcome!=TC_STATUS_ERROR_CANCELLED && outcome!=TC_STATUS_ERROR_TIMEOUT){
        outcome=TC_STATUS_ERROR_NO_SPACE;
        if(!failure)failure=g_strdup("Process exceeded its hard per-file output limit");
    }
    if (outcome != TC_STATUS_OK && error_out) {
        if (failure) *error_out = g_strdup(failure);
        else if (outcome == TC_STATUS_ERROR_CANCELLED) *error_out = g_strdup("Operation cancelled");
        else if (outcome == TC_STATUS_ERROR_TIMEOUT) *error_out = g_strdup("Process exceeded its time limit");
        else if (local.term_signal) *error_out = g_strdup_printf("Process terminated by signal %d: %s", local.term_signal, local.stderr_tail);
        else *error_out = g_strdup_printf("Process exited with code %d: %s", local.exit_code, local.stderr_tail);
    }
    if (result) *result = local;
    else tc_process_result_clear(&local);
    return outcome;
}

gboolean tc_run_helper(const char *const *argv,GCancellable *cancel,GError **error) {
    const TcProcessOptions options={.timeout_ms=30*60*1000,.terminate_grace_ms=500};
    char *message=NULL;
    TcStatusCode status=tc_process_run(argv,&options,cancel,NULL,NULL,NULL,&message);
    if(status!=TC_STATUS_OK && error)g_set_error_literal(error,G_IO_ERROR,
        status==TC_STATUS_ERROR_CANCELLED?G_IO_ERROR_CANCELLED:G_IO_ERROR_FAILED,message?message:"Helper failed");
    g_free(message);return status==TC_STATUS_OK;
}
