#ifndef TC_UI_H
#define TC_UI_H

#include "tc_common.h"
#include <adwaita.h>

/* UI component entry points */
void tc_ui_show_hud(GtkApplication *app, uint32_t total_files, int action, const char *initial_file, uint64_t batch_id);
void tc_ui_show_resize_dialog(GtkApplication *app, GList *file_paths);
void tc_ui_show_setup_ai(GtkApplication *app);

#endif /* TC_UI_H */
