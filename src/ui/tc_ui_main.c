#include "tc_ui.h"
#include "tc_logging.h"

static char *opt_batch_id = NULL;
static char *opt_mode = NULL;
static int opt_total = 1;
static int opt_action = 0;
static char *opt_file = NULL;
static char **opt_files = NULL;

static const GOptionEntry entries[] = {
    {"batch-id", 0, 0, G_OPTION_ARG_STRING, &opt_batch_id, "Batch ID for HUD", "ID"},
    {"mode", 'm', 0, G_OPTION_ARG_STRING, &opt_mode, "UI Mode: hud, resize, setup-ai", "MODE"},
    {"hud", 0, 0, G_OPTION_ARG_NONE, NULL, "Show floating HUD", NULL},
    {"total", 't', 0, G_OPTION_ARG_INT, &opt_total, "Total files for HUD", "COUNT"},
    {"action", 'a', 0, G_OPTION_ARG_INT, &opt_action, "Action type for HUD", "ACTION"},
    {"file", 'f', 0, G_OPTION_ARG_STRING, &opt_file, "Active file name for HUD", "FILE"},
    {G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &opt_files, "Input files", "FILES..."},
    {NULL}
};

static void on_app_activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;
    if (g_strcmp0(opt_mode, "resize") == 0) {
        GList *file_list = NULL;
        if (opt_files) {
            for (int i = 0; opt_files[i] != NULL; i++) {
                file_list = g_list_append(file_list, g_strdup(opt_files[i]));
            }
        }
        tc_ui_show_resize_dialog(app, file_list);
    } else if (g_strcmp0(opt_mode, "setup-ai") == 0) {
        tc_ui_show_setup_ai(app);
    } else {
        const char *init_file = opt_file;
        if (!init_file && opt_files && opt_files[0]) {
            init_file = opt_files[0];
        }
        tc_ui_show_hud(app, (uint32_t)opt_total, opt_action, init_file, opt_batch_id ? g_ascii_strtoull(opt_batch_id,NULL,10) : 0);
    }
}

int main(int argc, char *argv[]) {
    g_autoptr(AdwApplication) app = adw_application_new("org.gnome.teraconvert.ui", G_APPLICATION_NON_UNIQUE);
    
    g_application_add_main_option_entries(G_APPLICATION(app), entries);
    g_signal_connect(app, "activate", G_CALLBACK(on_app_activate), NULL);

    return g_application_run(G_APPLICATION(app), argc, argv);
}
