#ifndef TC_LOGGING_H
#define TC_LOGGING_H

#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>

#define TC_COLOR_RESET   "\033[0m"
#define TC_COLOR_RED     "\033[1;31m"
#define TC_COLOR_GREEN   "\033[1;32m"
#define TC_COLOR_YELLOW  "\033[1;33m"
#define TC_COLOR_BLUE    "\033[1;34m"
#define TC_COLOR_MAGENTA "\033[1;35m"
#define TC_COLOR_CYAN    "\033[1;36m"
#define TC_COLOR_GRAY    "\033[0;90m"

static inline void tc_log_header(const char *level, const char *color) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm_buf;
    localtime_r(&tv.tv_sec, &tm_buf);
    fprintf(stderr, "%s[%02d:%02d:%02d.%03ld] [%s]%s ",
            color,
            tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
            tv.tv_usec / 1000,
            level,
            TC_COLOR_RESET);
}

#define tc_log_debug(fmt, ...) do { \
    if (g_getenv("TC_DEBUG")) { \
        tc_log_header("DEBUG", TC_COLOR_GRAY); \
        fprintf(stderr, fmt "\n", ##__VA_ARGS__); \
    } \
} while(0)

#define tc_log_info(fmt, ...) do { \
    tc_log_header("INFO ", TC_COLOR_CYAN); \
    fprintf(stderr, fmt "\n", ##__VA_ARGS__); \
} while(0)

#define tc_log_warn(fmt, ...) do { \
    tc_log_header("WARN ", TC_COLOR_YELLOW); \
    fprintf(stderr, fmt "\n", ##__VA_ARGS__); \
} while(0)

#define tc_log_error(fmt, ...) do { \
    tc_log_header("ERROR", TC_COLOR_RED); \
    fprintf(stderr, fmt "\n", ##__VA_ARGS__); \
} while(0)

#endif /* TC_LOGGING_H */
