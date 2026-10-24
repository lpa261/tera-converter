#ifndef TC_COMMON_H
#define TC_COMMON_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#else
#include "config.h"
#endif

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/statvfs.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <glib.h>
#include <gio/gio.h>

#define TC_APP_ID               "org.gnome.teraconvert"
#ifndef TC_VERSION
#define TC_VERSION              "2.1.0"
#endif
#define TC_SOCKET_NAME          "teraconvert.socket"
#define TC_MEMFD_SEAL_NAME      "tc_bulk_paths"

/* SLAs and Memory Limits */
#define TC_CLIENT_POLL_TIMEOUT_MS   20
#define TC_MAX_IMAGE_DIMENSION      16384
#define TC_MIN_FREE_DISK_BYTES      (200ULL * 1024 * 1024)   /* 200 MB */
#define TC_INLINE_PAYLOAD_MAX_BYTES 32768                    /* 32 KB */
#define TC_AI_IDLE_TIMEOUT_SEC      60
#define TC_HUD_ADAPTIVE_THRESHOLD_MS 400
#define TC_HUD_AUTO_DISMISS_MS      2500

/* Return and status codes */
typedef enum {
    TC_STATUS_OK = 0,
    TC_STATUS_ERROR = 1,
    TC_STATUS_ERROR_CORRUPT = 2,
    TC_STATUS_ERROR_UNSUPPORTED = 3,
    TC_STATUS_ERROR_NO_SPACE = 4,
    TC_STATUS_ERROR_MEM_LIMIT = 5,
    TC_STATUS_ERROR_TIMEOUT = 6,
    TC_STATUS_ERROR_CANCELLED = 7,
    TC_STATUS_MISSING_AI_RUNTIME = 8,
    TC_STATUS_SKIPPED = 9,
    TC_STATUS_NO_BENEFIT = 10
} TcStatusCode;

/* Target Action Types */
typedef enum {
    TC_ACTION_CONVERT = 0,
    TC_ACTION_RESIZE = 1,
    TC_ACTION_REMOVE_BG = 2,
    TC_ACTION_BATCH = 3,
    TC_ACTION_MERGE = 4,
    TC_ACTION_COMPRESS = 5
} TcActionType;

/* Child process setup helper: runs at normal system scheduling priority for maximum performance */
static inline void tc_child_setup_low_priority(gpointer user_data) {
    (void)user_data;
}

/* Helper to check if user environment prefers Vietnamese */
static inline bool tc_is_vietnamese(void) {
    const char * const *langs = g_get_language_names();
    if (!langs) return false;
    for (int i = 0; langs[i] != NULL; i++) {
        if (g_str_has_prefix(langs[i], "vi")) return true;
        if (g_str_has_prefix(langs[i], "en") || g_strcmp0(langs[i], "C") == 0) return false;
    }
    return false;
}

/* Target Format Categories */
typedef enum {
    TC_CATEGORY_UNKNOWN = 0,
    TC_CATEGORY_IMAGE = 1,
    TC_CATEGORY_MEDIA = 2,
    TC_CATEGORY_DOCUMENT = 3
} TcCategory;

/* File Overwrite Policy */
typedef enum {
    TC_OVERWRITE_AUTO_NUMBER = 0,  /* default: name (1).ext */
    TC_OVERWRITE_SKIP = 1,
    TC_OVERWRITE_REPLACE = 2
} TcOverwritePolicy;

/* GTK labels and wire strings must end on a valid UTF-8 boundary. */
static inline void tc_utf8_copy(char *destination,size_t capacity,const char *source) {
    if(!capacity)return;
    g_autofree char *valid=g_utf8_make_valid(source?source:"",-1);
    g_strlcpy(destination,valid,capacity);
    const char *end=NULL;
    if(!g_utf8_validate(destination,-1,&end))*((char*)end)=0;
}

/* Safe-write helper declaration */
static inline char *tc_create_temp_sibling_path(const char *target_path, int *out_suffix_len) {
    if (!target_path) return NULL;
    char *dir = g_path_get_dirname(target_path);
    char *base = g_path_get_basename(target_path);
    char *dot = strrchr(base, '.');
    char *temp_path = NULL;
    int suffix_len = 0;

    if (dot && *(dot + 1) != '\0') {
        const char *ext = dot + 1;
        suffix_len = (int)strlen(dot); /* includes the dot */
        *dot = '\0';
        temp_path = g_strdup_printf("%s/.%s-XXXXXX.%s", dir, base, ext);
    } else {
        temp_path = g_strdup_printf("%s/.%s-XXXXXX", dir, base);
    }

    if (out_suffix_len) *out_suffix_len = suffix_len;
    g_free(dir);
    g_free(base);
    return temp_path;
}

#endif /* TC_COMMON_H */
