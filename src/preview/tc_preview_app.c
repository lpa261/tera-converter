#include "tc_preview.h"
#include <adwaita.h>
#include <gdk/gdkkeysyms.h>

typedef struct {
    GtkApplication *app;
    GtkWindow *window; /* weak; application owns the window */
    GtkWidget *body,*label,*position,*previous,*next,*info,*details;
    char *info_summary;
    GtkMediaStream *media;
    GPtrArray *files;
    unsigned index,page,pages,generation;
    gboolean busy;
    GCancellable *cancel;
    guint registrations[2],idle;
    GDBusNodeInfo *interface;
} PreviewApp;
static PreviewApp state;
static gboolean sushi;
static char *parent_handle;
static const char *object_path(void){return sushi?"/org/gnome/NautilusPreviewer":"/org/gnome/TeraConvert/Preview";}
static void visible_changed(gboolean visible) {
    GDBusConnection *bus=g_application_get_dbus_connection(G_APPLICATION(state.app));
    if(!bus)return;
    for(unsigned i=0;i<2;i++){GVariantBuilder changed;g_variant_builder_init(&changed,G_VARIANT_TYPE_VARDICT);g_variant_builder_add(&changed,"{sv}","Visible",g_variant_new_boolean(visible));
        g_dbus_connection_emit_signal(bus,NULL,object_path(),"org.freedesktop.DBus.Properties","PropertiesChanged",g_variant_new("(sa{sv}as)",i?"org.gnome.NautilusPreviewer2":"org.gnome.NautilusPreviewer",&changed,NULL),NULL);}
}
static void request_load(void);
static void stop_media(void) {
    if(state.media) gtk_media_stream_pause(state.media);
    g_clear_object(&state.media);
    g_clear_pointer(&state.info_summary,g_free);
}
static gboolean close_window(GtkWindow *window,gpointer unused) {
    (void)window;(void)unused;
    state.generation++;if(state.cancel)g_cancellable_cancel(state.cancel);
    stop_media();visible_changed(FALSE);return FALSE;
}
static void draw_page(GtkDrawingArea *area,cairo_t *cr,int width,int height,gpointer data) {
    (void)area;cairo_surface_t *surface=data;
    double w=cairo_image_surface_get_width(surface),h=cairo_image_surface_get_height(surface);
    double scale=MIN(width/w,height/h);
    cairo_translate(cr,(width-w*scale)/2,(height-h*scale)/2);cairo_scale(cr,scale,scale);
    cairo_set_source_surface(cr,surface,0,0);cairo_paint(cr);
}
static void navigate(int direction) {
    if(!state.files || !state.files->len)return;
    if(sushi && state.files->len==1){GDBusConnection *bus=g_application_get_dbus_connection(G_APPLICATION(state.app));g_dbus_connection_emit_signal(bus,NULL,object_path(),"org.gnome.NautilusPreviewer2","SelectionEvent",g_variant_new("(u)",direction<0?GTK_DIR_LEFT:GTK_DIR_RIGHT),NULL);return;}
    if(direction<0 && state.index)state.index--;
    else if(direction>0 && state.index+1<state.files->len)state.index++;
    else return;
    state.page=0;request_load();
}
static void previous(GtkButton *button,gpointer data){(void)button;(void)data;navigate(-1);}
static void next(GtkButton *button,gpointer data){(void)button;(void)data;navigate(1);}
static void change_page(int direction) {
    if((direction<0&&!state.page)||(direction>0&&state.page+1>=state.pages))return;
    state.page+=direction;request_load();
}
static void page_previous(GtkButton *button,gpointer data){(void)button;(void)data;change_page(-1);}
static void page_next(GtkButton *button,gpointer data){(void)button;(void)data;change_page(1);}
static gboolean key(GtkEventControllerKey *controller,guint value,guint code,GdkModifierType modifiers,gpointer data) {
    (void)controller;(void)code;(void)modifiers;(void)data;
    if(value==GDK_KEY_Escape || value==GDK_KEY_space){gtk_window_close(state.window);return TRUE;}
    if(value==GDK_KEY_Left){navigate(-1);return TRUE;}
    if(value==GDK_KEY_Right){navigate(1);return TRUE;}
    if(value==GDK_KEY_Page_Up && state.pages){change_page(-1);return TRUE;}
    if(value==GDK_KEY_Page_Down && state.pages){change_page(1);return TRUE;}
    return FALSE;
}
static void open_file(GtkButton *button,gpointer data) {
    (void)button;(void)data;
    if(!state.files||!state.files->len)return;
    g_autoptr(GFile) file=g_file_new_for_commandline_arg(g_ptr_array_index(state.files,state.index));
    g_autofree char *uri=g_file_get_uri(file);g_app_info_launch_default_for_uri_async(uri,NULL,NULL,NULL,NULL);
}
static void ensure_window(void) {
    if(state.window)return;
    state.window=GTK_WINDOW(adw_application_window_new(state.app));
    g_object_add_weak_pointer(G_OBJECT(state.window),(gpointer*)&state.window);
    gtk_window_set_default_size(state.window,920,680);gtk_window_set_title(state.window,"TeraConvert Quick Look");
    g_signal_connect(state.window,"close-request",G_CALLBACK(close_window),NULL);
    GtkWidget *box=gtk_box_new(GTK_ORIENTATION_VERTICAL,0),*header=adw_header_bar_new();
    state.previous=gtk_button_new_from_icon_name("go-previous-symbolic");
    state.next=gtk_button_new_from_icon_name("go-next-symbolic");
    gtk_widget_set_tooltip_text(state.previous,"Previous file (Left)");gtk_widget_set_tooltip_text(state.next,"Next file (Right)");
    g_signal_connect(state.previous,"clicked",G_CALLBACK(previous),NULL);g_signal_connect(state.next,"clicked",G_CALLBACK(next),NULL);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header),state.previous);adw_header_bar_pack_start(ADW_HEADER_BAR(header),state.next);
    GtkWidget *open=gtk_button_new_with_label("Open");g_signal_connect(open,"clicked",G_CALLBACK(open_file),NULL);adw_header_bar_pack_end(ADW_HEADER_BAR(header),open);
    state.label=gtk_label_new("Quick Look");gtk_label_set_ellipsize(GTK_LABEL(state.label),PANGO_ELLIPSIZE_MIDDLE);adw_header_bar_set_title_widget(ADW_HEADER_BAR(header),state.label);
    state.body=gtk_box_new(GTK_ORIENTATION_VERTICAL,8);gtk_widget_set_vexpand(state.body,TRUE);
    state.position=gtk_label_new("Space / Esc: close · Left / Right: files · PgUp / PgDn: PDF pages");
    GtkWidget *metadata=gtk_box_new(GTK_ORIENTATION_VERTICAL,4);
    gtk_widget_set_margin_top(metadata,10);gtk_widget_set_margin_bottom(metadata,10);
    gtk_widget_set_margin_start(metadata,18);gtk_widget_set_margin_end(metadata,18);
    state.info=gtk_label_new("");gtk_label_set_wrap(GTK_LABEL(state.info),TRUE);
    gtk_label_set_selectable(GTK_LABEL(state.info),TRUE);
    gtk_widget_add_css_class(state.info,"dim-label");
    GtkWidget *expander=gtk_expander_new(tc_is_vietnamese()?"Thông tin chi tiết":"File details");
    state.details=gtk_label_new("");gtk_label_set_wrap(GTK_LABEL(state.details),TRUE);
    gtk_label_set_xalign(GTK_LABEL(state.details),0);gtk_label_set_selectable(GTK_LABEL(state.details),TRUE);
    gtk_expander_set_child(GTK_EXPANDER(expander),state.details);
    gtk_box_append(GTK_BOX(metadata),state.info);gtk_box_append(GTK_BOX(metadata),expander);
    gtk_box_append(GTK_BOX(box),header);gtk_box_append(GTK_BOX(box),state.body);
    gtk_box_append(GTK_BOX(box),gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(box),metadata);gtk_box_append(GTK_BOX(box),state.position);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(state.window),box);visible_changed(TRUE);
    GtkEventController *keys=gtk_event_controller_key_new();gtk_event_controller_set_propagation_phase(keys,GTK_PHASE_CAPTURE);g_signal_connect(keys,"key-pressed",G_CALLBACK(key),NULL);gtk_widget_add_controller(GTK_WIDGET(state.window),keys);
}
typedef struct {char *path;unsigned page,generation;} Load;
static void load_free(gpointer data){Load *job=data;g_free(job->path);g_free(job);}
static void load_thread(GTask *task,gpointer source,gpointer data,GCancellable *cancel) {
    (void)source;Load *job=data;GError *error=NULL;
    TcPreview *p=tc_preview_load(job->path,job->page,cancel,&error);
    if(p)g_task_return_pointer(task,p,(GDestroyNotify)tc_preview_free);else g_task_return_error(task,error);
}
static void media_error(GObject *object,GParamSpec *spec,gpointer data) {
    (void)spec;(void)data;
    const GError *error=gtk_media_stream_get_error(GTK_MEDIA_STREAM(object));
    if(error&&state.window&&object==G_OBJECT(state.media))gtk_label_set_text(GTK_LABEL(state.position),error->message);
}
static void show_metadata(const TcPreview *p) {
    gboolean vi=tc_is_vietnamese();
    g_autofree char *size=g_format_size(p->file_size);
    g_autofree char *display_path=g_filename_display_name(p->path);
    g_autofree char *geometry=NULL;
    if(p->width>0 && p->height>0)geometry=g_strdup_printf("%d × %d px",p->width,p->height);
    else if(p->pages)geometry=g_strdup_printf(vi?"%u trang · %.1f × %.1f mm":"%u pages · %.1f × %.1f mm",p->pages,p->page_width*25.4/72,p->page_height*25.4/72);
    state.info_summary=geometry?g_strdup_printf("%s · %s · %s",p->type_name,size,geometry):g_strdup_printf("%s · %s",p->type_name,size);
    gtk_label_set_text(GTK_LABEL(state.info),state.info_summary);
    g_autofree char *details=g_strdup_printf(vi?
        "Loại: %s\nMIME: %s\nDung lượng: %s (%" G_GUINT64_FORMAT " byte)\nKích thước: %s\nSửa đổi: %s\nVị trí: %s":
        "Kind: %s\nMIME: %s\nSize: %s (%" G_GUINT64_FORMAT " bytes)\nDimensions: %s\nModified: %s\nLocation: %s",
        p->type_name,p->mime_type,size,p->file_size,geometry?geometry:"—",p->modified,display_path);
    gtk_label_set_text(GTK_LABEL(state.details),details);
}
static void media_metadata(GObject *object,GParamSpec *spec,gpointer data) {
    (void)spec;(void)data;
    if(!state.window || object!=G_OBJECT(state.media) || !state.info_summary)return;
    gint64 duration=gtk_media_stream_get_duration(state.media);
    if(duration>0){
        guint64 seconds=(guint64)(duration/G_USEC_PER_SEC);
        g_autofree char *text=g_strdup_printf("%s · %02" G_GUINT64_FORMAT ":%02" G_GUINT64_FORMAT ":%02" G_GUINT64_FORMAT,
            state.info_summary,seconds/3600,(seconds/60)%60,seconds%60);
        gtk_label_set_text(GTK_LABEL(state.info),text);
    }
}
static void loaded(GObject *source,GAsyncResult *result,gpointer unused) {
    (void)source;(void)unused;
    GTask *task=G_TASK(result);Load *job=g_task_get_task_data(task);GError *error=NULL;
    g_autoptr(TcPreview) p=g_task_propagate_pointer(task,&error);state.busy=FALSE;
    if(job->generation!=state.generation) {
        g_clear_error(&error);
        if(state.window)request_load();
        g_application_release(G_APPLICATION(state.app));return;
    }
    if(state.window) {
        GtkWidget *child;
        while((child=gtk_widget_get_first_child(state.body)))gtk_box_remove(GTK_BOX(state.body),child);
        if(!p){child=gtk_label_new(error?error->message:"Preview unavailable");gtk_label_set_wrap(GTK_LABEL(child),TRUE);gtk_box_append(GTK_BOX(state.body),child);}
        else {
            state.pages=p->pages;state.page=p->page_index;
            gtk_label_set_text(GTK_LABEL(state.label),p->name);
            show_metadata(p);
            if(p->kind==TC_PREVIEW_IMAGE) {
                GBytes *pixels=gdk_pixbuf_read_pixel_bytes(p->image);GdkTexture *texture=gdk_memory_texture_new(gdk_pixbuf_get_width(p->image),gdk_pixbuf_get_height(p->image),gdk_pixbuf_get_has_alpha(p->image)?GDK_MEMORY_R8G8B8A8:GDK_MEMORY_R8G8B8,pixels,gdk_pixbuf_get_rowstride(p->image));g_bytes_unref(pixels);child=gtk_picture_new_for_paintable(GDK_PAINTABLE(texture));g_object_unref(texture);
                gtk_picture_set_content_fit(GTK_PICTURE(child),GTK_CONTENT_FIT_CONTAIN);
            } else if(p->kind==TC_PREVIEW_PDF) {
                child=gtk_drawing_area_new();gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(child),draw_page,cairo_surface_reference(p->page),(GDestroyNotify)cairo_surface_destroy);
                GtkWidget *row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8),*prev=gtk_button_new_with_label("Previous page"),*next_page=gtk_button_new_with_label("Next page");
                g_signal_connect(prev,"clicked",G_CALLBACK(page_previous),NULL);g_signal_connect(next_page,"clicked",G_CALLBACK(page_next),NULL);
                gtk_widget_set_sensitive(prev,state.page>0);gtk_widget_set_sensitive(next_page,state.page+1<state.pages);
                gtk_box_append(GTK_BOX(row),prev);gtk_box_append(GTK_BOX(row),next_page);gtk_box_append(GTK_BOX(state.body),row);
            } else if(p->kind==TC_PREVIEW_MEDIA) {
                state.media=gtk_media_file_new_for_filename(p->path);child=gtk_video_new_for_media_stream(state.media);
                gtk_video_set_autoplay(GTK_VIDEO(child),FALSE);g_signal_connect(state.media,"notify::error",G_CALLBACK(media_error),NULL);
                g_signal_connect(state.media,"notify::duration",G_CALLBACK(media_metadata),NULL);
                g_signal_connect(state.media,"notify::prepared",G_CALLBACK(media_metadata),NULL);media_metadata(G_OBJECT(state.media),NULL,NULL);
            } else {
                child=gtk_scrolled_window_new();GtkWidget *text=gtk_text_view_new();gtk_text_view_set_editable(GTK_TEXT_VIEW(text),FALSE);gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(text),FALSE);gtk_text_view_set_monospace(GTK_TEXT_VIEW(text),TRUE);gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text),GTK_WRAP_WORD_CHAR);
                gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(text)),p->text?p->text:"Preview unavailable",-1);gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(child),text);
            }
            gtk_widget_set_vexpand(child,TRUE);gtk_widget_set_hexpand(child,TRUE);gtk_box_append(GTK_BOX(state.body),child);
            g_autofree char *position=p->pages?g_strdup_printf("File %u/%u · Page %u/%u · Space / Esc: close",state.index+1,state.files->len,state.page+1,p->pages):g_strdup_printf("File %u/%u · Space / Esc: close",state.index+1,state.files->len);
            gtk_label_set_text(GTK_LABEL(state.position),position);
        }
    }
    g_clear_error(&error);g_application_release(G_APPLICATION(state.app));
}
static void request_load(void) {
    state.generation++;state.pages=0;stop_media();
    if(state.window){gtk_label_set_text(GTK_LABEL(state.info),"");gtk_label_set_text(GTK_LABEL(state.details),"");}
    if(state.cancel)g_cancellable_cancel(state.cancel);
    if(state.busy)return; /* coalesce rapid navigation; only one decoder runs */
    if(!state.window||!state.files||!state.files->len)return;
    g_clear_object(&state.cancel);state.cancel=g_cancellable_new();state.busy=TRUE;
    gtk_label_set_text(GTK_LABEL(state.label),"Loading preview…");
    gtk_widget_set_sensitive(state.previous,sushi||state.index>0);gtk_widget_set_sensitive(state.next,sushi||state.index+1<state.files->len);
    Load *job=g_new0(Load,1);job->path=g_strdup(g_ptr_array_index(state.files,state.index));job->page=state.page;job->generation=state.generation;
    GTask *task=g_task_new(NULL,state.cancel,loaded,NULL);g_task_set_task_data(task,job,load_free);
    g_application_hold(G_APPLICATION(state.app));g_task_run_in_thread(task,load_thread);g_object_unref(task);
}
static void show(const char *uri,gboolean toggle,const char *token) {
    if(toggle && state.window && state.files && state.files->len==1 && !g_strcmp0(uri,g_ptr_array_index(state.files,0))){gtk_window_close(state.window);return;}
    g_ptr_array_set_size(state.files,0);g_ptr_array_add(state.files,g_strdup(uri));state.index=state.page=0;ensure_window();if(token&&*token)gtk_window_set_startup_id(state.window,token);request_load();gtk_window_present(state.window);
}
static const char xml[]="<node><interface name='org.gnome.NautilusPreviewer'><method name='ShowFile'><arg type='s' direction='in'/><arg type='i' direction='in'/><arg type='b' direction='in'/></method><method name='Close'/><property name='Visible' type='b' access='read'/></interface><interface name='org.gnome.NautilusPreviewer2'><method name='ShowFile'><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='b' direction='in'/><arg type='s' direction='in'/></method><method name='Close'/><property name='Visible' type='b' access='read'/><property name='ParentHandle' type='s' access='read'/><signal name='SelectionEvent'><arg type='u'/></signal></interface></node>";
static void method(GDBusConnection *connection,const char *sender,const char *path,const char *interface,const char *name,GVariant *args,GDBusMethodInvocation *invocation,gpointer data) {
    (void)connection;(void)sender;(void)path;(void)interface;(void)data;
    if(!strcmp(name,"ShowFile")){const char *uri,*handle="",*token="";gboolean toggle;if(!strcmp(interface,"org.gnome.NautilusPreviewer2"))g_variant_get(args,"(&s&sb&s)",&uri,&handle,&toggle,&token);else{gint32 xid;g_variant_get(args,"(&sib)",&uri,&xid,&toggle);}g_free(parent_handle);parent_handle=g_strdup(handle);g_autoptr(GFile) file=g_file_new_for_uri(uri);if(!g_file_is_native(file)){g_dbus_method_invocation_return_error(invocation,G_IO_ERROR,G_IO_ERROR_NOT_SUPPORTED,"Only local files are supported");return;}show(uri,toggle,token);}
    else if(!strcmp(name,"Close")&&state.window)gtk_window_close(state.window);
    g_dbus_method_invocation_return_value(invocation,NULL);
}
static GVariant *property(GDBusConnection *connection,const char *sender,const char *path,const char *interface,const char *name,GError **error,gpointer data){(void)connection;(void)sender;(void)path;(void)interface;(void)name;(void)error;(void)data;return !strcmp(name,"ParentHandle")?g_variant_new_string(parent_handle?parent_handle:""):g_variant_new_boolean(state.window!=NULL);}
static gboolean idle(gpointer data){(void)data;if(!state.window&&!state.busy)g_application_quit(G_APPLICATION(state.app));return G_SOURCE_CONTINUE;}
static void startup(GApplication *application,gpointer data) {
    (void)data;g_application_hold(application);state.files=g_ptr_array_new_with_free_func(g_free);
    GError *error=NULL;state.interface=g_dbus_node_info_new_for_xml(xml,&error);
    static const GDBusInterfaceVTable vtable={.method_call=method,.get_property=property};
    GDBusConnection *bus=g_application_get_dbus_connection(application);
    if(bus&&state.interface)for(unsigned i=0;i<2&&!error;i++)state.registrations[i]=g_dbus_connection_register_object(bus,object_path(),state.interface->interfaces[i],&vtable,NULL,NULL,&error);
    if(error){g_warning("Preview service: %s",error->message);g_clear_error(&error);}
    state.idle=g_timeout_add_seconds(60,idle,NULL);
}
static void shutdown_service(GApplication *app,gpointer data) {
    (void)data;GDBusConnection *bus=g_application_get_dbus_connection(app);
    if(bus)for(unsigned i=0;i<2;i++)if(state.registrations[i])g_dbus_connection_unregister_object(bus,state.registrations[i]);
}
static int command(GApplication *application,GApplicationCommandLine *line,gpointer data) {
    (void)application;(void)data;int argc;g_auto(GStrv) argv=g_application_command_line_get_arguments(line,&argc);
    g_ptr_array_set_size(state.files,0);
    for(int i=1;i<argc;i++)if(argv[i][0]!='-') {g_autoptr(GFile) f=g_application_command_line_create_file_for_arg(line,argv[i]);g_ptr_array_add(state.files,g_file_get_uri(f));}
    if(state.files->len){state.index=state.page=0;ensure_window();request_load();gtk_window_present(state.window);}
    return 0;
}
int main(int argc,char **argv) {
    for(int i=1;i<argc;i++)if(!strcmp(argv[i],"--sushi-service"))sushi=TRUE;
    g_autoptr(AdwApplication) app=adw_application_new(sushi?"org.gnome.NautilusPreviewer":"org.gnome.TeraConvert.Preview",G_APPLICATION_HANDLES_COMMAND_LINE);
    state.app=GTK_APPLICATION(app);
    g_application_add_main_option(G_APPLICATION(app),"sushi-service",0,G_OPTION_FLAG_NONE,G_OPTION_ARG_NONE,"Serve Nautilus Space-key preview requests",NULL);
    g_signal_connect(app,"startup",G_CALLBACK(startup),NULL);g_signal_connect(app,"shutdown",G_CALLBACK(shutdown_service),NULL);g_signal_connect(app,"command-line",G_CALLBACK(command),NULL);
    int result=g_application_run(G_APPLICATION(app),argc,argv);
    stop_media();g_clear_object(&state.cancel);if(state.files)g_ptr_array_unref(state.files);if(state.interface)g_dbus_node_info_unref(state.interface);if(state.idle)g_source_remove(state.idle);
    g_free(parent_handle);return result;
}
