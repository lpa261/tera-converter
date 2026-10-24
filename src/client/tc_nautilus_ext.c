#define _GNU_SOURCE
#define NAUTILUS_COMPILATION
#include <nautilus-extension.h>
#include <gio/gio.h>
#include "tc_client.h"
#include "tc_menu_policy.h"
#include "tc_logging.h"
#include "teraconvert-resources-en.h"
#include "teraconvert-resources-vi.h"

typedef struct _TcNautilusExt TcNautilusExt;
typedef struct _TcNautilusExtClass TcNautilusExtClass;

struct _TcNautilusExt {
    GObject parent_instance;
};

struct _TcNautilusExtClass {
    GObjectClass parent_class;
};

static void tc_nautilus_ext_init (TcNautilusExt *self);
static void tc_nautilus_ext_class_init (TcNautilusExtClass *klass);
static void tc_nautilus_ext_class_finalize (TcNautilusExtClass *klass);
static void tc_nautilus_ext_menu_provider_iface_init (NautilusMenuProviderInterface *iface);

G_DEFINE_DYNAMIC_TYPE_EXTENDED (
    TcNautilusExt,
    tc_nautilus_ext,
    G_TYPE_OBJECT,
    0,
    G_IMPLEMENT_INTERFACE_DYNAMIC (
        NAUTILUS_TYPE_MENU_PROVIDER,
        tc_nautilus_ext_menu_provider_iface_init
    )
)

static void tc_nautilus_ext_init (TcNautilusExt *self) {
    (void)self;
}

static void tc_nautilus_ext_class_init (TcNautilusExtClass *klass) {
    (void)klass;
}

static void tc_nautilus_ext_class_finalize (TcNautilusExtClass *klass) {
    (void)klass;
}

/* User data struct for menu item callbacks */
typedef struct {
    TcActionType action;
    char target_format[24];
    GList *uris;
} TcMenuCallbackData;

static gpointer copy_str(gconstpointer src, gpointer data) {
    (void)data;
    return g_strdup((const char *)src);
}

static TcMenuCallbackData *tc_menu_callback_data_new (
    TcActionType action,
    const char *target_format,
    GList *uris
) {
    TcMenuCallbackData *data = g_new0(TcMenuCallbackData, 1);
    data->action = action;
    if (target_format) {
        strncpy(data->target_format, target_format, sizeof(data->target_format) - 1);
    }
    data->uris = g_list_copy_deep(uris, copy_str, NULL);
    return data;
}

static void tc_menu_callback_data_free (gpointer user_data, GClosure *closure) {
    (void)closure;
    TcMenuCallbackData *data = (TcMenuCallbackData *)user_data;
    if (data) {
        g_list_free_full(data->uris, g_free);
        g_free(data);
    }
}

static void on_menu_action_activated (NautilusMenuItem *item, gpointer user_data) {
    (void)item;
    TcMenuCallbackData *data = (TcMenuCallbackData *)user_data;
    if (!data) return;

    if (data->action == TC_ACTION_RESIZE) {
        tc_client_launch_ui("resize", data->uris);
    } else {
        tc_client_dispatch_request(data->action, data->target_format, data->uris, TC_OVERWRITE_AUTO_NUMBER);
    }
}

static NautilusMenuItem *create_action_item(
    const char *name,
    const char *label,
    const char *tip,
    TcActionType action,
    const char *target_format,
    GList *uris, const TcMenuSelection *selection
) {
    if(!tc_menu_action_supported(selection,action,target_format))return NULL;
    NautilusMenuItem *item = nautilus_menu_item_new(name, label, tip, "teraconvert");
    TcMenuCallbackData *cb_data = tc_menu_callback_data_new(action, target_format, uris);
    g_signal_connect_data(
        item,
        "activate",
        G_CALLBACK(on_menu_action_activated),
        cb_data,
        tc_menu_callback_data_free,
        0
    );
    return item;
}

/* Optional helpers are checked without spawning processes on Nautilus's UI thread. */
static gboolean program_available(const char *name){g_autofree char *path=g_find_program_in_path(name);return path!=NULL;}
static guint menu_capabilities(void){
    guint caps=program_available("teraconvert-daemon")?TC_MENU_DAEMON:0;
#ifdef HAVE_LIBVIPS
    caps|=TC_MENU_VIPS;
#endif
#ifdef HAVE_FFMPEG
    if(program_available("ffmpeg"))caps|=TC_MENU_MEDIA;
#endif
#ifdef HAVE_POPPLER
    caps|=TC_MENU_PDF;
    if(program_available("pdftotext"))caps|=TC_MENU_PDF_TEXT;
    if(program_available("pdftoppm"))caps|=TC_MENU_PDF_RASTER;
    g_autofree char *helper=g_build_filename(TC_HELPER_DIR,"teraconvert-pdf-safe",NULL);
    if((caps&TC_MENU_PDF_TEXT)&&(caps&TC_MENU_PDF_RASTER)&&g_file_test(helper,G_FILE_TEST_IS_EXECUTABLE)&&program_available("python3"))caps|=TC_MENU_PDF_SAFE;
#endif
#ifdef HAVE_LIBARCHIVE
    caps|=TC_MENU_OFFICE;
#endif
#ifdef HAVE_IMAGE_PRESERVE
    caps|=TC_MENU_IMAGES;
#endif
    if(program_available("python3"))caps|=TC_MENU_PYTHON;
    if(program_available("gs"))caps|=TC_MENU_GS;
    if(program_available("teraconvert-ui"))caps|=TC_MENU_UI;
    if(program_available("teraconvert-preview"))caps|=TC_MENU_PREVIEW;
#ifdef HAVE_ENGINE_AI
    g_autofree char *model=g_build_filename(g_get_user_data_dir(),"teraconvert/models/rembg.onnx",NULL);
    g_autofree char *receipt=g_strconcat(model,".sha256",NULL);
    if(program_available("teraconvert-rembg")&&(caps&TC_MENU_PYTHON)&&g_file_test(model,G_FILE_TEST_IS_REGULAR)&&g_file_test(receipt,G_FILE_TEST_IS_REGULAR))caps|=TC_MENU_AI;
#endif
    return caps;
}
static void append_supported(NautilusMenu *menu,NautilusMenuItem *item){if(item){nautilus_menu_append_item(menu,item);g_object_unref(item);}}
static GList *append_result(GList *items,NautilusMenuItem *item){return item?g_list_append(items,item):items;}
static GList *append_submenu(GList *items,NautilusMenuItem *root,NautilusMenu *menu){
    GList *children=nautilus_menu_get_items(menu);
    if(children){nautilus_menu_item_set_submenu(root,menu);items=g_list_append(items,root);nautilus_menu_item_list_free(children);}
    else g_object_unref(root);
    g_object_unref(menu);return items;
}

/* Native action states for standalone context menu entries */
static GList *selected_rembg_uris = NULL;
static GSimpleAction *rembg_action = NULL;
static char *selected_resize_uri = NULL;
static GSimpleAction *resize_action = NULL;

static void on_remove_bg_activated (GSimpleAction *action, GVariant *param, gpointer user_data) {
    (void)action; (void)param; (void)user_data;
    if (!selected_rembg_uris) return;

    tc_log_info("Activated native Remove Background on %u files", g_list_length(selected_rembg_uris));
    tc_client_dispatch_request(TC_ACTION_REMOVE_BG, "rmbg", selected_rembg_uris, TC_OVERWRITE_AUTO_NUMBER);
}

static void on_resize_activated (GSimpleAction *action, GVariant *param, gpointer user_data) {
    (void)action; (void)param; (void)user_data;
    if (!selected_resize_uri) return;

    tc_log_info("Activated native Resize on %s", selected_resize_uri);
    GList *list = g_list_append(NULL, g_strdup(selected_resize_uri));
    tc_client_launch_ui("resize", list);
    g_list_free_full(list, g_free);
}

static void ensure_actions (void) {
    if (rembg_action && resize_action) return;

    GApplication *app = g_application_get_default ();
    if (!app) return;

    if (!rembg_action) {
        rembg_action = g_simple_action_new ("teraconvert-remove-bg", NULL);
        g_signal_connect (rembg_action, "activate", G_CALLBACK (on_remove_bg_activated), NULL);
        g_action_map_add_action (G_ACTION_MAP (app), G_ACTION (rembg_action));
    }

    if (!resize_action) {
        resize_action = g_simple_action_new ("teraconvert-resize", NULL);
        g_signal_connect (resize_action, "activate", G_CALLBACK (on_resize_activated), NULL);
        g_action_map_add_action (G_ACTION_MAP (app), G_ACTION (resize_action));
    }
}

/* Preview activation stays asynchronous on Nautilus's main context. */
static void preview_sent(GObject *source, GAsyncResult *result, gpointer data) {
    (void)data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (!reply) tc_log_error("Quick Look request failed: %s", error ? error->message : "unknown error");
}

static void preview_bus_ready(GObject *source, GAsyncResult *result, gpointer data) {
    (void)source;
    g_autofree char *uri = data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GDBusConnection) connection = g_bus_get_finish(result, &error);
    if (!connection) {
        tc_log_error("Cannot connect to preview service: %s", error ? error->message : "unknown error");
        return;
    }
    g_dbus_connection_call(connection, "org.gnome.TeraConvert.Preview",
        "/org/gnome/TeraConvert/Preview", "org.gnome.NautilusPreviewer", "ShowFile",
        g_variant_new("(sib)", uri, (gint32)0, FALSE), NULL,
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL, preview_sent, NULL);
}

static void on_preview_activated(NautilusMenuItem *item, gpointer user_data) {
    (void)item;
    if (user_data) g_bus_get(G_BUS_TYPE_SESSION, NULL, preview_bus_ready, g_strdup(user_data));
}

static void string_closure_notify (gpointer data, GClosure *closure) {
    (void)closure;
    g_free (data);
}

static GList *tc_nautilus_ext_get_file_items (
    NautilusMenuProvider *provider,
    GList                *files
) {
    (void)provider;
    ensure_actions ();
    if(rembg_action)g_simple_action_set_enabled(rembg_action,FALSE);
    if(resize_action)g_simple_action_set_enabled(resize_action,FALSE);
    g_list_free_full(selected_rembg_uris,g_free);selected_rembg_uris=NULL;
    g_clear_pointer(&selected_resize_uri,g_free);
    if (!files) return NULL;

    bool vi = tc_is_vietnamese ();

    uint32_t image_count = 0;
    uint32_t video_count = 0;
    uint32_t audio_count = 0;
    uint32_t pdf_count = 0;
    uint32_t doc_count = 0;
    uint32_t ooxml_count = 0;
    uint32_t total_count = 0;

    g_autoptr(GArray) inputs=g_array_new(FALSE,FALSE,sizeof(TcMenuFile));
    g_array_set_clear_func(inputs,tc_menu_file_clear);
    GList *uri_list = NULL;
    g_autofree char *first_uri = NULL;
    g_autofree char *first_ext = NULL;

    for (GList *l = files; l != NULL; l = l->next) {
        NautilusFileInfo *file = NAUTILUS_FILE_INFO(l->data);
        if (nautilus_file_info_is_directory(file)) {
            g_list_free_full(uri_list, g_free);
            return NULL;
        }

        char *uri = nautilus_file_info_get_uri(file);
        if (!uri) continue;
        uri_list = g_list_append(uri_list, uri);
        total_count++;

        g_autofree char *mime = nautilus_file_info_get_mime_type(file);
        g_autoptr(GFile) input_file=g_file_new_for_uri(uri);
        g_autofree char *input_base=g_file_get_basename(input_file);
        const char *input_dot=input_base?strrchr(input_base,'.'):NULL;
        g_autofree char *input_ext=g_ascii_strdown(input_dot?input_dot+1:"",-1);
        TcMenuFile input={g_strdup(input_ext),g_strdup(mime),g_file_is_native(input_file)};
        g_array_append_val(inputs,input);
        if (!first_uri) {
            first_uri = g_strdup(uri);
            g_autoptr(GFile) gfile = g_file_new_for_uri(uri);
            g_autofree char *path = g_file_get_path(gfile);
            if (path) {
                char *dot = strrchr(path, '.');
                if (dot && *(dot + 1) != '\0') {
                    first_ext = g_ascii_strdown(dot + 1, -1);
                }
            }
        }

        if (mime && (g_str_has_prefix(mime, "application/vnd.openxmlformats")||g_str_has_prefix(mime,"application/vnd.oasis.opendocument"))) ooxml_count++;
        if (mime) {
            if (g_str_has_prefix(mime, "image/")) {
                image_count++;
            } else if (g_str_has_prefix(mime, "video/")) {
                video_count++;
            } else if (g_str_has_prefix(mime, "audio/")) {
                audio_count++;
            } else if (g_strcmp0(mime, "application/pdf") == 0) {
                pdf_count++;
            } else if (g_str_has_prefix(mime, "application/vnd.oasis.opendocument") ||
                       g_str_has_prefix(mime, "application/vnd.openxmlformats") ||
                       g_str_has_prefix(mime, "application/msword")) {
                doc_count++;
            }
        } else if (input_ext) {
            if (strcmp(input_ext, "png") == 0 || strcmp(input_ext, "jpg") == 0 ||
                strcmp(input_ext, "jpeg") == 0 || strcmp(input_ext, "webp") == 0 ||
                strcmp(input_ext, "bmp") == 0 || strcmp(input_ext, "tiff") == 0 ||
                strcmp(input_ext, "tif") == 0 || strcmp(input_ext, "ico") == 0 ||
                strcmp(input_ext, "avif") == 0) {
                image_count++;
            } else if (strcmp(input_ext, "mp4") == 0 || strcmp(input_ext, "mkv") == 0 ||
                       strcmp(input_ext, "webm") == 0 || strcmp(input_ext, "avi") == 0 ||
                       strcmp(input_ext, "mov") == 0 || strcmp(input_ext, "flv") == 0) {
                video_count++;
            } else if (strcmp(input_ext, "mp3") == 0 || strcmp(input_ext, "wav") == 0 ||
                       strcmp(input_ext, "flac") == 0 || strcmp(input_ext, "m4a") == 0 ||
                       strcmp(input_ext, "ogg") == 0 || strcmp(input_ext, "opus") == 0) {
                audio_count++;
            } else if (strcmp(input_ext, "pdf") == 0) {
                pdf_count++;
            } else if (strcmp(input_ext, "docx") == 0 || strcmp(input_ext, "xlsx") == 0 ||
                       strcmp(input_ext, "pptx") == 0 || strcmp(input_ext, "doc") == 0 ||
                       strcmp(input_ext, "xls") == 0 || strcmp(input_ext, "ppt") == 0 ||
                       strcmp(input_ext, "odt") == 0 || strcmp(input_ext, "ods") == 0) {
                doc_count++;
            }
        }
    }

    if (!uri_list) return NULL;
    TcMenuSelection selection={(const TcMenuFile*)inputs->data,inputs->len,menu_capabilities()};

    /* Update standalone native actions state (Tách nền ảnh & Thay đổi kích thước) */
    bool has_images = (image_count == total_count && total_count > 0);
    bool single_image = (has_images && total_count == 1);

    if(rembg_action)g_simple_action_set_enabled(rembg_action,tc_menu_action_supported(&selection,TC_ACTION_REMOVE_BG,"rmbg"));
    if(resize_action)g_simple_action_set_enabled(resize_action,single_image&&tc_menu_action_supported(&selection,TC_ACTION_RESIZE,"resize"));

    if (selected_rembg_uris) {
        g_list_free_full(selected_rembg_uris, g_free);
        selected_rembg_uris = NULL;
    }
    if (has_images) {
        for (GList *l = uri_list; l != NULL; l = l->next) {
            selected_rembg_uris = g_list_append(selected_rembg_uris, g_strdup((char *)l->data));
        }
    }

    g_clear_pointer(&selected_resize_uri, g_free);
    if (single_image && first_uri) {
        selected_resize_uri = g_strdup(first_uri);
    }

    GList *result_items = NULL;

    /* 1. Quick Preview (Space) standalone item */
    if (first_uri && tc_menu_preview_supported(&selection)) {
        NautilusMenuItem *preview_item = nautilus_menu_item_new(
            "TeraConvert::QuickPreview",
            vi ? "Xem nhanh (Space)" : "Quick Preview (Space)",
            vi ? "Xem trước tệp tin với Quick Look (Phím Space)" : "Quick preview file with Space key",
            "quickview"
        );
        g_signal_connect_data(
            preview_item,
            "activate",
            G_CALLBACK(on_preview_activated),
            g_strdup(first_uri),
            string_closure_notify,
            0
        );
        result_items = g_list_append(result_items, preview_item);
    }

    /* 2. Standalone Convert Submenu ("Chuyển đổi sang..." / "Convert to...") */
    NautilusMenuItem *root_item = nautilus_menu_item_new(
        "TeraConvert::MenuRoot",
        vi ? "Chuyển đổi sang..." : "Convert to...",
        vi ? "Chuyển đổi định dạng tệp tin với TeraConvert" : "Convert file format with TeraConvert",
        "document-save-as"
    );
    NautilusMenu *sub = nautilus_menu_new();

    if (has_images) {
        /* Comprehensive Image Options */
            append_supported(sub, create_action_item("TeraConvert::ToWebP", vi ? ".webp  (Ảnh WebP)" : ".webp  (WebP Image)", NULL, TC_ACTION_CONVERT, "webp", uri_list, &selection));
            append_supported(sub, create_action_item("TeraConvert::ToPNG",  vi ? ".png  (Ảnh PNG)" : ".png  (PNG Image)",   NULL, TC_ACTION_CONVERT, "png", uri_list, &selection));
            append_supported(sub, create_action_item("TeraConvert::ToJPG",  vi ? ".jpg  (Ảnh JPEG)" : ".jpg  (JPEG Image)",  NULL, TC_ACTION_CONVERT, "jpg", uri_list, &selection));
            append_supported(sub, create_action_item("TeraConvert::ToAVIF", vi ? ".avif  (Ảnh AVIF)" : ".avif  (AVIF Image)", NULL, TC_ACTION_CONVERT, "avif", uri_list, &selection));
            append_supported(sub, create_action_item("TeraConvert::ToBMP",  vi ? ".bmp  (Ảnh Bitmap)" : ".bmp  (BMP Bitmap)", NULL, TC_ACTION_CONVERT, "bmp", uri_list, &selection));
            append_supported(sub, create_action_item("TeraConvert::ToTIFF", vi ? ".tiff  (Ảnh TIFF)" : ".tiff  (TIFF Image)", NULL, TC_ACTION_CONVERT, "tiff", uri_list, &selection));
            append_supported(sub, create_action_item("TeraConvert::ToICO",  vi ? ".ico  (Icon Favicon)" : ".ico  (ICO Favicon)", NULL, TC_ACTION_CONVERT, "ico", uri_list, &selection));

        if (total_count > 1) {
            g_autofree char *merge_label = vi ?
                g_strdup_printf("Ghép %u ảnh thành 1 file .pdf", total_count) :
                g_strdup_printf("Merge %u images into 1 .pdf file", total_count);
            append_supported(sub, create_action_item("TeraConvert::MergeToPDF", merge_label, NULL, TC_ACTION_MERGE, "pdf", uri_list, &selection));
            append_supported(sub, create_action_item("TeraConvert::ToPDF", vi ? ".pdf  (Mỗi ảnh 1 file PDF)" : ".pdf  (Individual PDF files)", NULL, TC_ACTION_CONVERT, "pdf", uri_list, &selection));
        } else {
            append_supported(sub, create_action_item("TeraConvert::ToPDF",  vi ? ".pdf  (Tài liệu PDF)" : ".pdf  (PDF Document)", NULL, TC_ACTION_CONVERT, "pdf", uri_list, &selection));
        }

    } else if (video_count > 0 && image_count == 0 && audio_count == 0 && pdf_count == 0 && doc_count == 0) {
        /* Comprehensive Video Options */
            append_supported(sub, create_action_item("TeraConvert::ToMP4",  vi ? ".mp4  (Video MP4 H.264)" : ".mp4  (MP4 Video)", NULL, TC_ACTION_CONVERT, "mp4", uri_list, &selection));
            append_supported(sub, create_action_item("TeraConvert::ToWebM", vi ? ".webm  (Video WebM VP9)" : ".webm  (WebM Video)", NULL, TC_ACTION_CONVERT, "webm", uri_list, &selection));
            append_supported(sub, create_action_item("TeraConvert::ToMKV",  vi ? ".mkv  (Video MKV)" : ".mkv  (MKV Video)", NULL, TC_ACTION_CONVERT, "mkv", uri_list, &selection));
            append_supported(sub, create_action_item("TeraConvert::ToAVI",  vi ? ".avi  (Video AVI)" : ".avi  (AVI Video)", NULL, TC_ACTION_CONVERT, "avi", uri_list, &selection));
            append_supported(sub, create_action_item("TeraConvert::ToGIF",  vi ? ".gif  (Ảnh động GIF Clip)" : ".gif  (Animated GIF)", NULL, TC_ACTION_CONVERT, "gif", uri_list, &selection));
        append_supported(sub, create_action_item("TeraConvert::ToMP3",  vi ? ".mp3  (Trích xuất MP3)" : ".mp3  (Extract MP3 Audio)", NULL, TC_ACTION_CONVERT, "mp3", uri_list, &selection));
        append_supported(sub, create_action_item("TeraConvert::ToWAV",  vi ? ".wav  (Trích xuất WAV)" : ".wav  (Extract WAV Audio)", NULL, TC_ACTION_CONVERT, "wav", uri_list, &selection));
        append_supported(sub, create_action_item("TeraConvert::ToFLAC", vi ? ".flac (Trích xuất FLAC)" : ".flac (Extract FLAC Audio)", NULL, TC_ACTION_CONVERT, "flac", uri_list, &selection));
        append_supported(sub, create_action_item("TeraConvert::ToM4A",  vi ? ".m4a  (Trích xuất M4A)" : ".m4a  (Extract M4A Audio)", NULL, TC_ACTION_CONVERT, "m4a", uri_list, &selection));

    } else if (audio_count > 0 && image_count == 0 && video_count == 0 && pdf_count == 0 && doc_count == 0) {
        /* Comprehensive Audio Options */
            append_supported(sub, create_action_item("TeraConvert::ToMP3",  vi ? ".mp3  (Âm thanh MP3 320k)" : ".mp3  (MP3 Audio 320k)", NULL, TC_ACTION_CONVERT, "mp3", uri_list, &selection));
            append_supported(sub, create_action_item("TeraConvert::ToWAV",  vi ? ".wav  (Âm thanh WAV, có thể đổi độ sâu bit)" : ".wav  (WAV Audio, May Change Bit Depth)", NULL, TC_ACTION_CONVERT, "wav", uri_list, &selection));
            append_supported(sub, create_action_item("TeraConvert::ToFLAC", vi ? ".flac (Âm thanh FLAC thông thường)" : ".flac (Standard FLAC Conversion)", NULL, TC_ACTION_CONVERT, "flac", uri_list, &selection));
            append_supported(sub, create_action_item("TeraConvert::ToM4A",  vi ? ".m4a  (Âm thanh M4A / AAC)" : ".m4a  (M4A / AAC Audio)", NULL, TC_ACTION_CONVERT, "m4a", uri_list, &selection));
            append_supported(sub, create_action_item("TeraConvert::ToOGG",  vi ? ".ogg  (Âm thanh OGG Vorbis)" : ".ogg  (OGG Vorbis)", NULL, TC_ACTION_CONVERT, "ogg", uri_list, &selection));
            append_supported(sub, create_action_item("TeraConvert::ToOpus", vi ? ".opus (Âm thanh Opus)" : ".opus (Opus Audio)", NULL, TC_ACTION_CONVERT, "opus", uri_list, &selection));

    } else if (pdf_count > 0 && image_count == 0 && video_count == 0 && audio_count == 0 && doc_count == 0) {
        /* PDF specific conversions */
        append_supported(sub, create_action_item("TeraConvert::PdfToPNG", vi ? ".png  (Xuất ảnh PNG)" : ".png  (Export PNG Images)", NULL, TC_ACTION_CONVERT, "png", uri_list, &selection));
        append_supported(sub, create_action_item("TeraConvert::PdfToJPG", vi ? ".jpg  (Xuất ảnh JPG)" : ".jpg  (Export JPG Images)", NULL, TC_ACTION_CONVERT, "jpg", uri_list, &selection));
        append_supported(sub, create_action_item("TeraConvert::PdfToTXT", vi ? ".txt  (Trích xuất chữ TXT)" : ".txt  (Extract Text TXT)", NULL, TC_ACTION_CONVERT, "txt", uri_list, &selection));

    } else if (doc_count > 0 && image_count == 0 && video_count == 0 && audio_count == 0) {
        append_supported(sub, create_action_item("TeraConvert::DocToPDF", vi ? ".pdf  (Xuất tài liệu PDF)" : ".pdf  (Export to PDF)", NULL, TC_ACTION_CONVERT, "pdf", uri_list, &selection));

    } else if ((image_count + video_count + audio_count + doc_count + pdf_count) > 0) {
        if (total_count > 1) {
            g_autofree char *merge_label = vi ?
                g_strdup_printf("Ghép %u tệp thành 1 file .pdf", total_count) :
                g_strdup_printf("Merge %u files into 1 .pdf file", total_count);
            append_supported(sub, create_action_item("TeraConvert::BatchPDF", merge_label, NULL, TC_ACTION_MERGE, "pdf", uri_list, &selection));
        } else {
            append_supported(sub, create_action_item("TeraConvert::BatchPDF", vi ? ".pdf  (Tài liệu PDF)" : ".pdf  (PDF Document)", NULL, TC_ACTION_CONVERT, "pdf", uri_list, &selection));
        }
    }

    result_items=append_submenu(result_items,root_item,sub);

    /* 3. Standalone Smart Compression Submenu ("Nén tệp tin..." / "Compress...") */
    if (video_count > 0 && image_count == 0 && audio_count == 0 && pdf_count == 0 && doc_count == 0) {
        NautilusMenuItem *comp_root = nautilus_menu_item_new(
            "TeraConvert::CompressVideoRoot",
            vi ? "Nén video..." : "Compress Video...",
            vi ? "Tối ưu vùng đệm hoặc mã hóa lại video theo chế độ đã chọn" : "Optimize padding or re-encode video according to the selected mode",
            "media-record"
        );
        NautilusMenu *comp_sub = nautilus_menu_new();
        append_supported(comp_sub, create_action_item("TeraConvert::CompPadding",
            vi ? "Bỏ vùng đệm dư cuối MP4/MOV (Giữ nguyên dữ liệu)" : "Remove Trailing MP4/MOV Padding (Preserve Data)",
            vi ? "Giữ nguyên từng byte còn lại; chỉ tạo bản mới khi bỏ được vùng đệm dư. Định dạng không hỗ trợ sẽ được bỏ qua."
               : "Keep every retained byte unchanged; create an output only when trailing padding can be removed. Unsupported layouts are skipped.",
            TC_ACTION_COMPRESS, "comp-padding", uri_list, &selection));
        append_supported(comp_sub,create_action_item("TeraConvert::CompRemux",
            vi?"Tối ưu cấu trúc MP4/MOV (Kiểm tra toàn bộ luồng)":"Optimize MP4/MOV Structure (Validate All Streams)",
            vi?"Giữ nguyên packet, thời gian và metadata được hỗ trợ. Cấu trúc chưa xác minh sẽ được bỏ qua.":"Preserve packets, timing and supported metadata. Unverified layouts are skipped.",
            TC_ACTION_COMPRESS,"comp-remux",uri_list, &selection));
        append_supported(comp_sub, create_action_item("TeraConvert::CompWeb",   vi ? "Nén nhanh để gửi (Có giảm chất lượng)" : "Fast Compression (Lossy)", NULL, TC_ACTION_COMPRESS, "comp-web", uri_list, &selection));
        append_supported(comp_sub, create_action_item("TeraConvert::CompSaver", vi ? "Ưu tiên dung lượng nhỏ (Có giảm chất lượng)" : "Prioritize Small Size (Lossy)", NULL, TC_ACTION_COMPRESS, "comp-saver", uri_list, &selection));
        append_supported(comp_sub, create_action_item("TeraConvert::CompHQ",    vi ? "Nén nhẹ (Có thể giảm độ phân giải và chất lượng)" : "Light Compression (May Reduce Resolution and Quality)", NULL, TC_ACTION_COMPRESS, "comp-hq", uri_list, &selection));
        result_items=append_submenu(result_items,comp_root,comp_sub);

    } else if(audio_count==total_count && total_count>0) {
        result_items=append_result(result_items,create_action_item("TeraConvert::SafeFlac",
            vi?"Chuyển FLAC (Giữ nguyên mẫu âm thanh)":"Convert to FLAC (Preserve Audio Samples)",
            vi?"Kiểm tra toàn bộ PCM, độ sâu bit, tần số và bố trí kênh. Nguồn không hỗ trợ sẽ được bỏ qua.":"Validate every PCM sample, depth, rate and channel layout. Unsupported sources are skipped.",
            TC_ACTION_CONVERT,"safe-flac",uri_list, &selection));
    } else if(image_count==total_count && total_count>0) {
        result_items=append_result(result_items,create_action_item("TeraConvert::CompImage",
            vi?"Tối ưu PNG/JPEG (Giữ nguyên nội dung)":"Optimize PNG/JPEG (Preserve Content)",
            vi?"Kiểm tra dữ liệu ảnh và metadata; định dạng chưa hỗ trợ sẽ được bỏ qua.":"Verify image data and metadata; unsupported formats are skipped.",
            TC_ACTION_COMPRESS,"comp-image",uri_list, &selection));
    } else if (pdf_count > 0 && image_count == 0 && video_count == 0 && audio_count == 0 && doc_count == 0) {
        NautilusMenuItem *comp_root = nautilus_menu_item_new(
            "TeraConvert::CompressPdfRoot",
            vi ? "Nén PDF..." : "Compress PDF...",
            vi ? "Nén tối ưu dung lượng tệp PDF" : "Optimize and compress PDF file",
            "x-office-document"
        );
        NautilusMenu *comp_sub = nautilus_menu_new();
        append_supported(comp_sub,create_action_item("TeraConvert::CompPdfSafe",
            vi?"Tối ưu PDF (Giữ nguyên nội dung)":"Optimize PDF (Preserve Content)",
            vi?"Kiểm tra cấu trúc, nội dung và từng trang; bỏ qua tính năng chưa hỗ trợ.":"Validate structure, content and every page; skip unsupported features.",
            TC_ACTION_COMPRESS,"comp-pdf-safe",uri_list, &selection));
        append_supported(comp_sub, create_action_item("TeraConvert::CompPdfWeb",   vi ? "Nén nhỏ để gửi Email / Web (Có giảm chất lượng)" : "Compact Size (Lossy)", NULL, TC_ACTION_COMPRESS, "comp-pdf-web", uri_list, &selection));
        append_supported(comp_sub, create_action_item("TeraConvert::CompPdfPrint", vi ? "Nén cân bằng PDF (Có giảm chất lượng)" : "Balanced PDF Compression (Lossy)", NULL, TC_ACTION_COMPRESS, "comp-pdf-print", uri_list, &selection));
        result_items=append_submenu(result_items,comp_root,comp_sub);

    } else if (doc_count == total_count && ooxml_count == total_count && total_count > 0 &&
               image_count == 0 && video_count == 0 && audio_count == 0 && pdf_count == 0) {
        NautilusMenuItem *comp_doc_item = create_action_item(
            "TeraConvert::CompressDoc",
            vi ? "Tối ưu tài liệu (OOXML / ODF)" : "Optimize Document (OOXML / ODF)",
            vi ? "Nén lại cấu trúc ZIP và kiểm tra nội dung; không nén lại ảnh nhúng" : "Recompress ZIP entries and verify contents; embedded images are unchanged",
            TC_ACTION_COMPRESS,
            "comp-office",
            uri_list, &selection
        );
        result_items = append_result(result_items, comp_doc_item);
    }

    g_list_free_full(uri_list, g_free);
    return result_items;
}

static GList *tc_nautilus_ext_get_background_items (
    NautilusMenuProvider *provider,
    NautilusFileInfo     *current_folder
) {
    (void)provider;
    (void)current_folder;
    return NULL;
}

static void tc_nautilus_ext_menu_provider_iface_init (NautilusMenuProviderInterface *iface) {
    iface->get_file_items = tc_nautilus_ext_get_file_items;
    iface->get_background_items = tc_nautilus_ext_get_background_items;
}

/* Nautilus dynamic module entry points */
void nautilus_module_initialize (GTypeModule *module) {
    if (tc_is_vietnamese ()) {
        g_resources_register (teraconvert_vi_get_resource ());
    } else {
        g_resources_register (teraconvert_en_get_resource ());
    }
    tc_nautilus_ext_register_type(module);
}

void nautilus_module_shutdown (void) {
    if (selected_rembg_uris) {
        g_list_free_full (selected_rembg_uris, g_free);
        selected_rembg_uris = NULL;
    }
    g_clear_pointer (&selected_resize_uri, g_free);
    if (tc_is_vietnamese ()) {
        g_resources_unregister (teraconvert_vi_get_resource ());
    } else {
        g_resources_unregister (teraconvert_en_get_resource ());
    }
}

void nautilus_module_list_types (const GType **types, int *num_types) {
    static GType type_list[1];
    type_list[0] = tc_nautilus_ext_get_type();
    *types = type_list;
    *num_types = 1;
}
