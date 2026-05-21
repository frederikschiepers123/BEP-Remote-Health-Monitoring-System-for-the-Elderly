#define LOG_TAG "LOG"
#include "log.h"

#include <stdio.h>
#include <stdarg.h>
#include "pico/stdlib.h"

static const char *const level_str[] = {
    [LOG_LEVEL_ERROR]   = "E",
    [LOG_LEVEL_WARN]    = "W",
    [LOG_LEVEL_INFO]    = "I",
    [LOG_LEVEL_DEBUG]   = "D",
    [LOG_LEVEL_VERBOSE] = "V",
};

void log_write(int level, const char *tag, const char *fmt, ...) {
    if (level < LOG_LEVEL_ERROR || level > LOG_LEVEL_VERBOSE) return;

    /* Timestamp in milliseconds since boot. */
    uint32_t ms = (uint32_t)(to_ms_since_boot(get_absolute_time()));

    fprintf(stderr, "[%7lu][%s][%s] ", (unsigned long)ms, level_str[level], tag);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fprintf(stderr, "\r\n");
}
