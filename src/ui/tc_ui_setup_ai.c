#include "tc_ui.h"
void tc_ui_show_setup_ai(GtkApplication *app) {
    bool vi=tc_is_vietnamese();
    GtkWidget *window=adw_window_new();
    gtk_window_set_application(GTK_WINDOW(window),app);
    gtk_window_set_title(GTK_WINDOW(window),vi?"Model AI cài sẵn":"Preinstalled AI model");
    gtk_window_set_default_size(GTK_WINDOW(window),460,200);
    GtkWidget *label=gtk_label_new(vi?
    "Tách nền dùng model RMBG ONNX cài sẵn và ONNX Runtime. Chỉ model kèm SHA-256 hợp lệ mới được dùng. Tính năng bị ẩn khi chưa có model; bản này không tự tải model.":
    "Background removal requires a preinstalled RMBG ONNX model and ONNX Runtime. Only a model with a valid SHA-256 receipt is accepted. The action is hidden without a model; this build does not download models.");
    gtk_label_set_wrap(GTK_LABEL(label),TRUE);
    gtk_widget_set_margin_top(label,24);
    gtk_widget_set_margin_bottom(label,24);
    gtk_widget_set_margin_start(label,24);
    gtk_widget_set_margin_end(label,24);
    adw_window_set_content(ADW_WINDOW(window),label);
    gtk_window_present(GTK_WINDOW(window));
}
