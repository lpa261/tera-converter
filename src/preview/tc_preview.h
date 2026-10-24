#ifndef TC_PREVIEW_H
#define TC_PREVIEW_H
#include "tc_common.h"
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <cairo.h>
typedef enum { TC_PREVIEW_INFO, TC_PREVIEW_IMAGE, TC_PREVIEW_TEXT, TC_PREVIEW_PDF, TC_PREVIEW_MEDIA } TcPreviewKind;
typedef struct {
    TcPreviewKind kind;
    char *path, *name, *description, *text;
    char *mime_type, *type_name, *modified;
    uint64_t file_size;
    int width, height;
    double page_width, page_height;
    GdkPixbuf *image;
    cairo_surface_t *page;
    unsigned pages, page_index;
} TcPreview;
/* Owned result; callers must release it. Only regular local files are read. */
TcPreview *tc_preview_load(const char *argument, unsigned page, GCancellable *cancel, GError **error);
void tc_preview_free(TcPreview *preview);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(TcPreview, tc_preview_free)
#endif
