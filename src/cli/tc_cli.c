#include "tc_ipc.h"
#include <signal.h>
static volatile sig_atomic_t interrupted;
static void on_interrupt(int signal){
    (void)signal;
    interrupted=1;
}
static void string_json(const char *text){
    putchar('"');
    for(const unsigned char *p=(const unsigned char*)text;*p;p++){
        if(*p=='"'||*p=='\\'){
            putchar('\\');
            putchar(*p);
        } else if(*p<32)printf("\\u%04x",*p);
        else putchar(*p);
    }
    putchar('"');
}
static bool ack_read(int fd,uint16_t type,TcIpcAckV2 *ack){
    TcIpcHeader h;
    return tc_ipc_receive_all(fd,&h,sizeof(h),1000)&&h.magic==TC_IPC_MAGIC&&h.version==2&&!h.has_fd&&h.msg_type==type&&h.payload_size==sizeof(*ack)&&tc_ipc_receive_all(fd,ack,sizeof(*ack),1000);
}
static bool control(uint16_t type,uint64_t id,int *listener){
    int fd=tc_ipc_connect_client(500);
    if(fd<0)return false;
    struct __attribute__((packed)){
        TcIpcHeader h;
        uint64_t id;
    }
    f={
        .h={
            .magic=TC_IPC_MAGIC,.version=2,.msg_type=type,.payload_size=8
        },.id=id
    };
    bool ok=tc_ipc_send_all(fd,&f,sizeof(f),500);
    if(ok&&listener)*listener=fd;
    else close(fd);
    return ok;
}
static bool ping(TcIpcAckV2 *ack){
    int fd=tc_ipc_connect_client(500);
    if(fd<0)return false;
    TcIpcHeader h={
        .magic=TC_IPC_MAGIC,.version=2,.msg_type=TC_IPC_MSG_PING
    };
    bool ok=tc_ipc_send_all(fd,&h,sizeof(h),500)&&ack_read(fd,TC_IPC_MSG_PONG,ack);
    close(fd);
    return ok;
}
static int watch_batch(int fd,uint64_t id,int timeout){
    TcIpcProgressReaderV2 reader={
        0
    };
    gint64 start=g_get_monotonic_time();
    bool cancel_sent=false;
    for(;;){
        if(interrupted&&!cancel_sent){
            control(TC_IPC_MSG_CANCEL_REQ,id,NULL);
            cancel_sent=true;
        }
        if(timeout>0&&g_get_monotonic_time()-start>(gint64)timeout*1000000){
            control(TC_IPC_MSG_CANCEL_REQ,id,NULL);
            fputs("Timed out waiting for completion; cancellation requested.\n",stderr);
            close(fd);
            return 124;
        }
        TcIpcProgressV2 event={
            0
        };
        int status=tc_ipc_read_progress_v2(fd,&reader,&event);
        if(status<0|| (status==1&&event.batch_id!=id)){
            fputs("IPC v2 stream ended or returned the wrong batch.\n",stderr);
            close(fd);
            return 1;
        }
        if(status==0){
            struct pollfd wait={
                .fd=fd,.events=POLLIN
            };
            poll(&wait,1,100);
            continue;
        }
        printf("{\"batch_id\":%" G_GUINT64_FORMAT ",\"phase\":%u,\"status\":%u,\"completed\":%u,\"total\":%u,\"created\":%u,\"no_benefit\":%u,\"unsupported\":%u,\"existing\":%u,\"cancelled\":%u,\"failed\":%u,\"input_bytes\":%" G_GUINT64_FORMAT ",\"output_bytes\":%" G_GUINT64_FORMAT ",\"saved_bytes\":%" G_GUINT64_FORMAT ",\"reason\":",
        event.batch_id,event.phase,event.status,event.progress.completed_files,event.progress.total_files,event.saved_files,event.no_benefit_files,event.unsupported_files,event.existing_files,event.cancelled_files,event.progress.failed_files,event.input_bytes,event.output_bytes,event.saved_bytes);
        string_json(event.reason);
        fputs(",\"file\":",stdout);
        string_json(event.progress.current_filename);
        puts("}");
        fflush(stdout);
        if(event.phase==TC_PHASE_DONE){
            close(fd);
            return event.progress.failed_files?1:event.cancelled_files?130:0;
        }
    }
}
int main(int argc,char **argv){
    char *format=NULL,*cancel_id=NULL,*listen_id=NULL,*overwrite=NULL;
    char **files=NULL;
    gboolean doctor=FALSE;
    int width=0,height=0,timeout=6*60*60+6*60;
    const GOptionEntry options[]={
        {
            "doctor",0,0,G_OPTION_ARG_NONE,&doctor,"Probe daemon IPC version and capabilities",NULL
        },
        {
            "format",'f',0,G_OPTION_ARG_STRING,&format,"Target format (comp-padding, comp-remux, comp-image, comp-office, comp-pdf-safe, safe-flac, etc.)","FORMAT"
        },
        {
            "cancel",0,0,G_OPTION_ARG_STRING,&cancel_id,"Cancel one batch","ID"
        },
        {
            "listen",0,0,G_OPTION_ARG_STRING,&listen_id,"Listen to one active or recently completed batch","ID"
        },
        {
            "overwrite",0,0,G_OPTION_ARG_STRING,&overwrite,"auto-number, skip or replace (default auto-number)","POLICY"
        },
        {
            "width",0,0,G_OPTION_ARG_INT,&width,"Resize width","PIXELS"
        },
        {
            "height",0,0,G_OPTION_ARG_INT,&height,"Resize height","PIXELS"
        },
        {
            "timeout",0,0,G_OPTION_ARG_INT,&timeout,"Maximum wait in seconds (0 disables)","SECONDS"
        },
        {
            G_OPTION_REMAINING,0,0,G_OPTION_ARG_FILENAME_ARRAY,&files,"Local input files","FILES"
        },{
            NULL
        }
    };
    GOptionContext *context=g_option_context_new("— TeraConvert IPC v2; emits JSON lines");
    g_option_context_add_main_entries(context,options,NULL);
    GError *error=NULL;
    bool parsed=g_option_context_parse(context,&argc,&argv,&error);
    g_option_context_free(context);
    if(!parsed){
        g_printerr("%s\n",error->message);
        g_clear_error(&error);
        return 2;
    }
    signal(SIGINT,on_interrupt);
    signal(SIGTERM,on_interrupt);
    signal(SIGPIPE,SIG_IGN);
    TcIpcAckV2 capabilities={
        0
    };
    if(!ping(&capabilities)){
        g_printerr("Cannot negotiate IPC v2 with the daemon: %s. Start teraconvert.socket and update all components together.\n",g_strerror(errno));
        return 1;
    }
    if(doctor){
        printf("{\"client_version\":\"%s\",\"ipc_version\":2,\"capabilities\":%u}\n",TC_VERSION,capabilities.capabilities);
        return 0;
    }
    if(cancel_id||listen_id){
        char *end=NULL;
        errno=0;
        const char *text=cancel_id?cancel_id:listen_id;
        uint64_t id=g_ascii_strtoull(text,&end,10);
        if(!id||errno||!end||*end||text[0]=='-'||(cancel_id&&listen_id)){
            fputs("Expected one positive batch ID.\n",stderr);
            return 2;
        }
        if(cancel_id)return control(TC_IPC_MSG_CANCEL_REQ,id,NULL)?0:1;
        int fd=-1;
        if(!control(TC_IPC_MSG_REGISTER_UI_LISTENER,id,&fd))return 1;
        return watch_batch(fd,id,timeout);
    }
    if(!format||strlen(format)>=24||!files||!files[0]||timeout<0||width<0||height<0||width>TC_MAX_IMAGE_DIMENSION||height>TC_MAX_IMAGE_DIMENSION||(!width!=!height)){
        fputs("Specify --format and local files; resize requires width and height.\n",stderr);
        return 2;
    }
    TcOverwritePolicy policy=TC_OVERWRITE_AUTO_NUMBER;
    if(overwrite){
        if(!strcmp(overwrite,"skip"))policy=TC_OVERWRITE_SKIP;
        else if(!strcmp(overwrite,"replace"))policy=TC_OVERWRITE_REPLACE;
        else if(strcmp(overwrite,"auto-number")){
            fputs("Unknown overwrite policy.\n",stderr);
            return 2;
        }
    }
    TcActionType action=width?TC_ACTION_RESIZE:g_str_has_prefix(format,"comp-")?TC_ACTION_COMPRESS:!strcmp(format,"rmbg")?TC_ACTION_REMOVE_BG:TC_ACTION_CONVERT;
    TcCompressionPolicy compression=tc_compression_policy(action,format);
    uint64_t id=((uint64_t)g_random_int()<<32)|g_random_int();
    if(!id)id=1;
    TcIpcRequestV2 request={
        .batch_id=id,.min_saved_bytes=1,.mode=compression.mode,.flags=1
    };
    GString *payload=g_string_new_len((const char*)&request,sizeof(request));
    guint count=0;
    for(char **p=files;*p;p++){
        char *absolute=realpath(*p,NULL);
        struct stat st;
        if(!absolute||strchr(absolute,'\n')||stat(absolute,&st)||!S_ISREG(st.st_mode)){
            g_printerr("Invalid input: %s\n",*p);
            free(absolute);
            g_string_free(payload,TRUE);
            return 2;
        }
        g_string_append(payload,absolute);
        g_string_append_c(payload,'\n');
        free(absolute);
        count++;
    }
    if(count>TC_IPC_MAX_FILES||payload->len>TC_INLINE_PAYLOAD_MAX_BYTES){
        fputs("CLI inline batch exceeds its bounded size; split the batch.\n",stderr);
        g_string_free(payload,TRUE);
        return 2;
    }
    int fd=tc_ipc_connect_client(500);
    if(fd<0){
        g_string_free(payload,TRUE);
        return 1;
    }
    TcIpcHeader header={
        .magic=TC_IPC_MAGIC,.version=2,.msg_type=TC_IPC_MSG_CONVERT_REQ,.action=action,.overwrite_policy=policy,.total_files=count,.payload_size=(uint32_t)payload->len,.target_width=width,.target_height=height
    };
    g_strlcpy(header.target_format,format,sizeof(header.target_format));
    bool sent=tc_ipc_header_is_valid(&header)&&tc_ipc_send_all(fd,&header,sizeof(header),500)&&tc_ipc_send_all(fd,payload->str,payload->len,500);
    g_string_free(payload,TRUE);
    TcIpcAckV2 ack={
        0
    };
    if(!sent||!ack_read(fd,TC_IPC_MSG_CONVERT_RESP,&ack)||ack.batch_id!=id||ack.status){
        fputs("Daemon did not accept the batch.\n",stderr);
        close(fd);
        return 1;
    }
    return watch_batch(fd,id,timeout);
}
