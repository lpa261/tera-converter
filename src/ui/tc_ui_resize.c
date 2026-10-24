#include "tc_ui.h"
#include "tc_logging.h"
#include "tc_client.h"
#include <gdk-pixbuf/gdk-pixbuf.h>

typedef struct {
    GtkWidget *window;
    GtkWidget *width_spin;
    GtkWidget *height_spin;
    GtkWidget *ratio_check;
    GList *file_paths;
    int orig_width;
    int orig_height;
    double aspect_ratio;
    gboolean updating_spin;
} TcResizeDialog;

static void on_preset_clicked(GtkButton *btn, gpointer user_data) {
    TcResizeDialog *dlg = (TcResizeDialog *)user_data;
    const char *label = gtk_button_get_label(btn);
    if (!label) return;

    dlg->updating_spin = TRUE;
    if (g_str_has_prefix(label, "50%")) {
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(dlg->width_spin), (int)(dlg->orig_width * 0.5));
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(dlg->height_spin), (int)(dlg->orig_height * 0.5));
    } else if (g_str_has_prefix(label, "75%")) {
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(dlg->width_spin), (int)(dlg->orig_width * 0.75));
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(dlg->height_spin), (int)(dlg->orig_height * 0.75));
    } else if (g_str_has_prefix(label, "25%")) {
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(dlg->width_spin), (int)(dlg->orig_width * 0.25));
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(dlg->height_spin), (int)(dlg->orig_height * 0.25));
    } else if (g_str_has_prefix(label, "1080p")) {
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(dlg->width_spin), 1920);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(dlg->height_spin), 1080);
    }
    dlg->updating_spin = FALSE;
}

static void on_width_changed(GtkSpinButton *spin, gpointer user_data) {
    TcResizeDialog *dlg = (TcResizeDialog *)user_data;
    if (dlg->updating_spin) return;

    if (gtk_check_button_get_active(GTK_CHECK_BUTTON(dlg->ratio_check)) && dlg->aspect_ratio > 0.001) {
        dlg->updating_spin = TRUE;
        int new_w = gtk_spin_button_get_value_as_int(spin);
        int new_h = (int)(new_w / dlg->aspect_ratio);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(dlg->height_spin), new_h);
        dlg->updating_spin = FALSE;
    }
}

static void on_height_changed(GtkSpinButton *spin, gpointer user_data) {
    TcResizeDialog *dlg = (TcResizeDialog *)user_data;
    if (dlg->updating_spin) return;

    if (gtk_check_button_get_active(GTK_CHECK_BUTTON(dlg->ratio_check)) && dlg->aspect_ratio > 0.001) {
        dlg->updating_spin = TRUE;
        int new_h = gtk_spin_button_get_value_as_int(spin);
        int new_w = (int)(new_h * dlg->aspect_ratio);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(dlg->width_spin), new_w);
        dlg->updating_spin = FALSE;
    }
}

static void on_apply_resize(GtkButton *btn, gpointer user_data) {
    (void)btn;
    TcResizeDialog *dlg = (TcResizeDialog *)user_data;
    int target_w = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(dlg->width_spin));
    int target_h = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(dlg->height_spin));

    tc_log_info("Applying resize: %dx%d on %u files", target_w, target_h, g_list_length(dlg->file_paths));

    /* Convert paths to URIs for dispatch */
    GList *uris = NULL;
    for (GList *l = dlg->file_paths; l != NULL; l = l->next) {
        char *uri = g_filename_to_uri((const char *)l->data, NULL, NULL);
        if (uri) uris = g_list_append(uris, uri);
    }

    if (uris) {
        tc_client_dispatch_resize_request((uint16_t)target_w, (uint16_t)target_h, uris, TC_OVERWRITE_AUTO_NUMBER);
        g_list_free_full(uris, g_free);
    }

    gtk_window_close(GTK_WINDOW(dlg->window));
}

static void on_cancel_resize(GtkButton *btn, gpointer user_data) {
    (void)btn;
    TcResizeDialog *dlg = (TcResizeDialog *)user_data;
    gtk_window_close(GTK_WINDOW(dlg->window));
}

void tc_ui_show_resize_dialog(GtkApplication *app, GList *file_paths) {
    TcResizeDialog *dlg = g_new0(TcResizeDialog, 1);
    dlg->file_paths = file_paths;
    dlg->orig_width = 1920;
    dlg->orig_height = 1080;

    /* Read actual dimensions from first file if available */
    if (file_paths && file_paths->data) {
        int w = 0, h = 0;
        GdkPixbufFormat *fmt = gdk_pixbuf_get_file_info((const char *)file_paths->data, &w, &h);
        if (fmt && w > 0 && h > 0) {
            dlg->orig_width = w;
            dlg->orig_height = h;
        }
    }
    dlg->aspect_ratio = (double)dlg->orig_width / (double)(dlg->orig_height > 0 ? dlg->orig_height : 1);

    dlg->window = adw_window_new();
    gtk_window_set_application(GTK_WINDOW(dlg->window), GTK_APPLICATION(app));
    gtk_window_set_title(GTK_WINDOW(dlg->window), "Resize Images — TeraConvert");
    gtk_window_set_default_size(GTK_WINDOW(dlg->window), 420, 360);
    gtk_window_set_resizable(GTK_WINDOW(dlg->window), FALSE);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_margin_top(box, 20);
    gtk_widget_set_margin_bottom(box, 20);
    gtk_widget_set_margin_start(box, 24);
    gtk_widget_set_margin_end(box, 24);

    GtkWidget *title = gtk_label_new("Resize Preset & Dimensions");
    gtk_widget_add_css_class(title, "title-3");
    gtk_box_append(GTK_BOX(box), title);

    /* Preset buttons */
    GtkWidget *preset_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(preset_box, GTK_ALIGN_CENTER);
    const char *presets[] = {"75%", "50%", "25%", "1080p", NULL};
    for (int i = 0; presets[i]; i++) {
        GtkWidget *btn = gtk_button_new_with_label(presets[i]);
        gtk_widget_add_css_class(btn, "pill");
        g_signal_connect(btn, "clicked", G_CALLBACK(on_preset_clicked), dlg);
        gtk_box_append(GTK_BOX(preset_box), btn);
    }
    gtk_box_append(GTK_BOX(box), preset_box);

    /* Dimensions spin buttons */
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_widget_set_halign(grid, GTK_ALIGN_CENTER);

    GtkWidget *w_label = gtk_label_new("Width (px):");
    dlg->width_spin = gtk_spin_button_new_with_range(16, 16384, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(dlg->width_spin), dlg->orig_width);
    g_signal_connect(dlg->width_spin, "value-changed", G_CALLBACK(on_width_changed), dlg);

    GtkWidget *h_label = gtk_label_new("Height (px):");
    dlg->height_spin = gtk_spin_button_new_with_range(16, 16384, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(dlg->height_spin), dlg->orig_height);
    g_signal_connect(dlg->height_spin, "value-changed", G_CALLBACK(on_height_changed), dlg);

    dlg->ratio_check = gtk_check_button_new_with_label("Lock Aspect Ratio (🔗)");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(dlg->ratio_check), TRUE);

    gtk_grid_attach(GTK_GRID(grid), w_label, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), dlg->width_spin, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), h_label, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), dlg->height_spin, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), dlg->ratio_check, 0, 2, 2, 1);
    gtk_box_append(GTK_BOX(box), grid);

    /* Action buttons: Cancel & Apply */
    GtkWidget *action_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_halign(action_box, GTK_ALIGN_END);
    gtk_widget_set_margin_top(action_box, 12);

    GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");
    g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_cancel_resize), dlg);

    GtkWidget *apply_btn = gtk_button_new_with_label("Apply Resize");
    gtk_widget_add_css_class(apply_btn, "suggested-action");
    g_signal_connect(apply_btn, "clicked", G_CALLBACK(on_apply_resize), dlg);

    gtk_box_append(GTK_BOX(action_box), cancel_btn);
    gtk_box_append(GTK_BOX(action_box), apply_btn);
    gtk_box_append(GTK_BOX(box), action_box);

    adw_window_set_content(ADW_WINDOW(dlg->window), box);
    gtk_window_present(GTK_WINDOW(dlg->window));
}
