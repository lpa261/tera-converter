#include "tc_ui.h"
#include "tc_ipc.h"
typedef struct {
    GtkWidget *window,*bar,*status,*detail,*cancel;
    uint64_t batch_id;
    int fd;
    guint watch,timer;
    bool done,cancelling;
    TcIpcProgressReaderV2 reader;
} TcHud;
static void cancel_batch(TcHud *hud) {
    if(!hud->batch_id)return;
    int fd=tc_ipc_connect_client(100);
    if(fd<0)return;
    struct __attribute__((packed)){
        TcIpcHeader header;
        uint64_t id;
    }
    frame={
        .header={
            .magic=TC_IPC_MAGIC,.version=2,.msg_type=TC_IPC_MSG_CANCEL_REQ,.payload_size=sizeof(uint64_t)
        },
        .id=hud->batch_id
    };
    tc_ipc_send_all(fd,&frame,sizeof(frame),100);
    close(fd);
}
static void destroy(GtkWidget *widget,gpointer data) {
    (void)widget;
    TcHud *hud=data;
    if(!hud->done)cancel_batch(hud);
    if(hud->watch)g_source_remove(hud->watch);
    if(hud->timer)g_source_remove(hud->timer);
    if(hud->fd>=0)close(hud->fd);
    g_free(hud);
}
static void cancel_clicked(GtkButton *button,gpointer data) {
    (void)button;
    TcHud *hud=data;
    if(hud->done){
        gtk_window_close(GTK_WINDOW(hud->window));
        return;
    }
    if(!hud->cancelling){
        hud->cancelling=true;
        cancel_batch(hud);
        gtk_label_set_text(GTK_LABEL(hud->status),tc_is_vietnamese()?"Đang hủy lô này…":"Cancelling this batch…");
        gtk_widget_set_sensitive(hud->cancel,FALSE);
    }
}
static const char *phase_text(unsigned phase,bool vi) {
    switch(phase){
        case TC_PHASE_QUEUED:return vi?"Đang xếp hàng":"Queued";
        case TC_PHASE_WAITING:return vi?"Đang chờ tài nguyên":"Waiting for resources";
        case TC_PHASE_PROBING:return vi?"Đang kiểm tra tệp":"Inspecting input";
        case TC_PHASE_VALIDATING:return vi?"Đang xác minh kết quả":"Validating output";
        case TC_PHASE_COMMITTING:return vi?"Đang lưu kết quả":"Publishing output";
        default:return vi?"Đang xử lý":"Processing";
    }
}
static gboolean progress(GIOChannel *channel,GIOCondition condition,gpointer data) {
    (void)channel;
    (void)condition;
    TcHud *hud=data;
    bool vi=tc_is_vietnamese();
    TcIpcProgressV2 event={
        0
    };
    int rc=tc_ipc_read_progress_v2(hud->fd,&hud->reader,&event);
    if(!rc)return G_SOURCE_CONTINUE;
    if(rc<0 || event.batch_id!=hud->batch_id){
        hud->watch=0;
        if(!hud->done)gtk_label_set_text(GTK_LABEL(hud->status),vi?"Mất kết nối trước khi nhận kết quả.":"Disconnected before receiving the result.");
        hud->done=true;
        gtk_button_set_label(GTK_BUTTON(hud->cancel),vi?"Đóng":"Close");
        gtk_widget_set_sensitive(hud->cancel,TRUE);
        return G_SOURCE_REMOVE;
    }
    TcIpcProgressPayload *p=&event.progress;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(hud->bar),p->progress_fraction);
    g_autofree char *summary=NULL;
    if(p->completed_files==p->total_files){
        hud->done=true;
        summary=g_strdup_printf(vi?"Hoàn tất: %u tạo mới · %u không cải thiện · %u không hỗ trợ · %u đã có · %u hủy · %u lỗi":
        "Done: %u created · %u no benefit · %u unsupported · %u existing · %u cancelled · %u failed",
        event.saved_files,event.no_benefit_files,event.unsupported_files,event.existing_files,event.cancelled_files,p->failed_files);
        gtk_button_set_label(GTK_BUTTON(hud->cancel),vi?"Đóng":"Close");
        gtk_widget_set_sensitive(hud->cancel,TRUE);
        gtk_widget_remove_css_class(hud->cancel,"destructive-action");
    } else if(!hud->cancelling)summary=g_strdup_printf("%s: %u/%u",phase_text(event.phase,vi),p->completed_files,p->total_files);
    if(summary)gtk_label_set_text(GTK_LABEL(hud->status),summary);
    const char *mode=event.mode==TC_MODE_PRESERVE?(vi?"Giữ nguyên nội dung":"Preserve content"):
    event.mode==TC_MODE_PERCEPTUAL?(vi?"Có giảm chất lượng":"Lossy compression"):(vi?"Chuyển đổi":"Conversion");
    g_autofree char *saved=g_format_size(event.saved_bytes);
    g_autofree char *detail=g_strdup_printf(vi?"%s · Tiết kiệm: %s\n%s\n%s":"%s · Saved: %s\n%s\n%s",
    mode,saved,p->current_filename,event.reason);
    gtk_label_set_text(GTK_LABEL(hud->detail),detail);
    return G_SOURCE_CONTINUE;
}
void tc_ui_show_hud(GtkApplication *app,uint32_t total,int action,const char *file,uint64_t batch_id) {
    (void)total;
    (void)action;
    bool vi=tc_is_vietnamese();
    TcHud *hud=g_new0(TcHud,1);
    hud->fd=-1;
    hud->batch_id=batch_id;
    hud->window=adw_window_new();
    gtk_window_set_application(GTK_WINDOW(hud->window),app);
    gtk_window_set_title(GTK_WINDOW(hud->window),"TeraConvert");
    gtk_window_set_default_size(GTK_WINDOW(hud->window),440,260);
    g_signal_connect(hud->window,"destroy",G_CALLBACK(destroy),hud);
    GtkWidget *box=gtk_box_new(GTK_ORIENTATION_VERTICAL,12);
    gtk_widget_set_margin_top(box,20);
    gtk_widget_set_margin_bottom(box,20);
    gtk_widget_set_margin_start(box,20);
    gtk_widget_set_margin_end(box,20);
    hud->status=gtk_label_new(vi?"Đang kết nối…":"Connecting…");
    gtk_label_set_wrap(GTK_LABEL(hud->status),TRUE);
    hud->detail=gtk_label_new(file?file:"");
    gtk_label_set_wrap(GTK_LABEL(hud->detail),TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(hud->detail),64);
    gtk_widget_add_css_class(hud->detail,"dim-label");
    hud->bar=gtk_progress_bar_new();
    hud->cancel=gtk_button_new_with_label(vi?"Hủy lô này":"Cancel this batch");
    gtk_widget_add_css_class(hud->cancel,"destructive-action");
    g_signal_connect(hud->cancel,"clicked",G_CALLBACK(cancel_clicked),hud);
    gtk_box_append(GTK_BOX(box),hud->status);
    gtk_box_append(GTK_BOX(box),hud->bar);
    gtk_box_append(GTK_BOX(box),hud->detail);
    gtk_box_append(GTK_BOX(box),hud->cancel);
    hud->fd=batch_id?tc_ipc_connect_client(200):-1;
    if(hud->fd>=0){
        struct __attribute__((packed)){
            TcIpcHeader header;
            uint64_t id;
        }
        reg={
            .header={
                .magic=TC_IPC_MAGIC,.version=2,.msg_type=TC_IPC_MSG_REGISTER_UI_LISTENER,.payload_size=sizeof(uint64_t)
            },.id=batch_id
        };
        if(tc_ipc_send_all(hud->fd,&reg,sizeof(reg),100)){
            GIOChannel *channel=g_io_channel_unix_new(hud->fd);
            g_io_channel_set_encoding(channel,NULL,NULL);
            g_io_channel_set_buffered(channel,FALSE);
            hud->watch=g_io_add_watch(channel,G_IO_IN|G_IO_ERR|G_IO_HUP,progress,hud);
            g_io_channel_unref(channel);
        }
    }
    if(!hud->watch){
        hud->done=true;
        gtk_label_set_text(GTK_LABEL(hud->status),vi?"Không kết nối được lô. Hãy cập nhật client và daemon cùng phiên bản.":"Cannot connect to batch. Update client and daemon together.");
        gtk_button_set_label(GTK_BUTTON(hud->cancel),vi?"Đóng":"Close");
    }
    adw_window_set_content(ADW_WINDOW(hud->window),box);
    gtk_window_present(GTK_WINDOW(hud->window));
}
