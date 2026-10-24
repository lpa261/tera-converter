#include "tc_temp_job.h"
#include <sys/file.h>
#include <dirent.h>
struct TcTempJob {
    int record_fd;
    char *record,*directory,*candidate;
};
typedef struct __attribute__((packed)) {
    char magic[8];
    uint64_t device,inode;
    uint32_t length;
    char path[4096];
} TcTempRecord;
static int state_open(char **path) {
    const char *override=g_getenv("TC_JOB_STATE_DIR");
    *path=override?g_strdup(override):g_build_filename(g_get_user_state_dir(),"teraconvert","jobs",NULL);
    if(g_mkdir_with_parents(*path,0700))return -1;
    int fd=open(*path,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    struct stat st;
    if(fd>=0 && (fstat(fd,&st)||st.st_uid!=getuid()||(st.st_mode&0077))){
        close(fd);
        fd=-1;
        errno=EPERM;
    }
    return fd;
}
/* All descendants belong to this private job directory. Never follow links. */
static bool remove_children(int fd,unsigned depth,unsigned *count) {
    if(depth>32)return false;
    int copy=dup(fd);
    if(copy<0)return false;
    DIR *dir=fdopendir(copy);
    if(!dir){
        close(copy);
        return false;
    }
    struct dirent *entry;
    bool ok=true;
    while((entry=readdir(dir))){
        if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;
        if(++*count>10000){
            ok=false;
            break;
        }
        struct stat st;
        if(fstatat(fd,entry->d_name,&st,AT_SYMLINK_NOFOLLOW)){
            ok=false;
            break;
        }
        if(S_ISDIR(st.st_mode)){
            int child=openat(fd,entry->d_name,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
            bool removed=child>=0 && remove_children(child,depth+1,count);
            if(child>=0)close(child);
            if(!removed || unlinkat(fd,entry->d_name,AT_REMOVEDIR)){
                ok=false;
                break;
            }
        } else if(unlinkat(fd,entry->d_name,0)){
            ok=false;
            break;
        }
    }
    closedir(dir);
    return ok;
}
static bool recover_record(int record) {
    struct stat marker;
    if(fstat(record,&marker)||!S_ISREG(marker.st_mode)||marker.st_uid!=getuid()||
    marker.st_nlink!=1||(marker.st_mode&0022)||marker.st_size<=0||marker.st_size>16384)return false;
    TcTempRecord data;
    if(marker.st_size!=sizeof(data)||pread(record,&data,sizeof(data),0)!=sizeof(data)||memcmp(data.magic,"TCJOB02",8))return false;
    uint32_t length=GUINT32_FROM_LE(data.length);
    if(!length||length>=sizeof(data.path)||data.path[length]||memchr(data.path,0,length)||data.path[0]!='/')return false;
    const char *path=data.path;
    g_autofree char *base=g_path_get_basename(path);
    if(!g_str_has_prefix(base,".teraconvert-"))return false;
    int fd=open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0)return errno==ENOENT;
    struct stat st;
    bool valid=fstat(fd,&st)==0 && st.st_uid==getuid() && !(st.st_mode&0077) &&
    (uint64_t)st.st_dev==GUINT64_FROM_LE(data.device) &&
    (uint64_t)st.st_ino==GUINT64_FROM_LE(data.inode);
    unsigned count=0;
    bool removed=valid && remove_children(fd,0,&count);
    close(fd);
    return removed && rmdir(path)==0;
}
unsigned tc_temp_job_recover(void) {
    g_autofree char *path=NULL;
    int state=state_open(&path);
    if(state<0)return 0;
    int copy=dup(state);
    DIR *dir=copy>=0?fdopendir(copy):NULL;
    if(!dir){
        if(copy>=0)close(copy);
        close(state);
        return 0;
    }
    unsigned removed=0,seen=0;
    struct dirent *entry;
    while((entry=readdir(dir)) && seen++<10000){
        if(!g_str_has_suffix(entry->d_name,".job"))continue;
        int record=openat(state,entry->d_name,O_RDWR|O_CLOEXEC|O_NOFOLLOW);
        if(record<0)continue;
        if(flock(record,LOCK_EX|LOCK_NB)==0 && recover_record(record) && unlinkat(state,entry->d_name,0)==0)removed++;
        close(record);
    }
    closedir(dir);
    close(state);
    return removed;
}
TcTempJob *tc_temp_job_new(const char *destination,char **error) {
    g_autofree char *state_path=NULL,*parent=NULL,*uuid=NULL,*name=NULL,*file=NULL;
    TcTempJob *job=g_new0(TcTempJob,1);
    job->record_fd=-1;
    int state=state_open(&state_path);
    if(state<0)goto fail;
    parent=g_path_get_dirname(destination);
    char canonical[4096];
    if(!realpath(parent,canonical))goto fail_state;
    job->directory=g_build_filename(canonical,".teraconvert-XXXXXX",NULL);
    if(!mkdtemp(job->directory))goto fail_state;
    struct stat st;
    if(stat(job->directory,&st))goto fail_directory;
    uuid=g_uuid_string_random();
    name=g_strconcat(uuid,".job",NULL);
    job->record=g_build_filename(state_path,name,NULL);
    job->record_fd=openat(state,name,O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(job->record_fd<0||flock(job->record_fd,LOCK_EX|LOCK_NB))goto fail_directory;
    TcTempRecord data={
        0
    };
    memcpy(data.magic,"TCJOB02",8);
    data.device=GUINT64_TO_LE(st.st_dev);
    data.inode=GUINT64_TO_LE(st.st_ino);
    size_t length=strlen(job->directory);
    if(length>=sizeof(data.path))goto fail_directory;
    data.length=GUINT32_TO_LE((uint32_t)length);
    memcpy(data.path,job->directory,length+1);
    size_t offset=0;
    while(offset<sizeof(data)){
        ssize_t written=write(job->record_fd,(char*)&data+offset,sizeof(data)-offset);
        if(written<0&&errno==EINTR)continue;
        if(written<=0)goto fail_directory;
        offset+=(size_t)written;
    }
    if(fsync(job->record_fd))goto fail_directory;
    const char *basename=strrchr(destination,'/');
    const char *dot=strrchr(basename?basename+1:destination,'.');
    file=g_strdup_printf("candidate%s",dot?dot:"");
    job->candidate=g_build_filename(job->directory,file,NULL);
    int candidate=open(job->candidate,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
    if(candidate<0)goto fail_directory;
    close(candidate);
    close(state);
    return job;
    fail_directory:
    if(job->record)unlink(job->record);
    if(job->directory)rmdir(job->directory);
    fail_state:
    close(state);
    fail:
    if(error)*error=g_strdup_printf("Cannot create owned job workspace: %s",g_strerror(errno));
    if(job->record_fd>=0)close(job->record_fd);
    g_free(job->record);
    g_free(job->directory);
    g_free(job->candidate);
    g_free(job);
    return NULL;
}
const char *tc_temp_job_path(const TcTempJob *job){
    return job?job->candidate:NULL;
}
const char *tc_temp_job_directory(const TcTempJob *job){
    return job?job->directory:NULL;
}
void tc_temp_job_finish(TcTempJob **owned) {
    if(!owned||!*owned)return;
    TcTempJob *job=*owned;
    *owned=NULL;
    if(recover_record(job->record_fd))unlink(job->record);
    close(job->record_fd);
    g_free(job->record);
    g_free(job->directory);
    g_free(job->candidate);
    g_free(job);
}
