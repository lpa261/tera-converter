#ifndef TC_MENU_POLICY_H
#define TC_MENU_POLICY_H
#include "tc_common.h"
enum { TC_MENU_VIPS=1, TC_MENU_MEDIA=2, TC_MENU_PDF=4, TC_MENU_OFFICE=8,
       TC_MENU_IMAGES=16, TC_MENU_AI=32, TC_MENU_UI=64, TC_MENU_GS=128,
       TC_MENU_PDF_SAFE=256, TC_MENU_PREVIEW=512, TC_MENU_PDF_TEXT=1024,
       TC_MENU_PDF_RASTER=2048, TC_MENU_PYTHON=4096, TC_MENU_DAEMON=8192 };
typedef struct {char *extension,*mime;gboolean local;} TcMenuFile;
typedef struct {const TcMenuFile *files;guint count,capabilities;} TcMenuSelection;
void tc_menu_file_clear(gpointer data);
gboolean tc_menu_action_supported(const TcMenuSelection *selection,TcActionType action,const char *format);
gboolean tc_menu_preview_supported(const TcMenuSelection *selection);
#endif
