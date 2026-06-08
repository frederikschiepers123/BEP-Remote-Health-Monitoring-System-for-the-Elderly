#ifndef LOG_H
#define LOG_H

#include <stdint.h>

/* Compile-time log level filter.
 * Override in CMake: target_compile_definitions(foo PRIVATE LOG_LEVEL=LOG_LEVEL_DEBUG)
 * Default: INFO for release, DEBUG for dev. */
#define LOG_LEVEL_ERROR     1
#define LOG_LEVEL_WARN      2
#define LOG_LEVEL_INFO      3
#define LOG_LEVEL_DEBUG     4
#define LOG_LEVEL_VERBOSE   5

#ifndef LOG_LEVEL
#  ifdef NDEBUG
#    define LOG_LEVEL LOG_LEVEL_INFO
#  else
#    define LOG_LEVEL LOG_LEVEL_DEBUG
#  endif
#endif

/* Output goes to CDC1 (log UART, not the MQTT data stream on CDC0).
 * Critical errors are also published to rmms/<uuid>/log via mqtt_client. */

void log_write(int level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Per-module tag macros — define LOG_TAG before including this header. */
#ifndef LOG_TAG
#  define LOG_TAG "?"
#endif

#define LOG_E(fmt, ...) \
    do { if (LOG_LEVEL >= LOG_LEVEL_ERROR)   log_write(LOG_LEVEL_ERROR,   LOG_TAG, fmt, ##__VA_ARGS__); } while(0)
#define LOG_W(fmt, ...) \
    do { if (LOG_LEVEL >= LOG_LEVEL_WARN)    log_write(LOG_LEVEL_WARN,    LOG_TAG, fmt, ##__VA_ARGS__); } while(0)
#define LOG_I(fmt, ...) \
    do { if (LOG_LEVEL >= LOG_LEVEL_INFO)    log_write(LOG_LEVEL_INFO,    LOG_TAG, fmt, ##__VA_ARGS__); } while(0)
#define LOG_D(fmt, ...) \
    do { if (LOG_LEVEL >= LOG_LEVEL_DEBUG)   log_write(LOG_LEVEL_DEBUG,   LOG_TAG, fmt, ##__VA_ARGS__); } while(0)
#define LOG_V(fmt, ...) \
    do { if (LOG_LEVEL >= LOG_LEVEL_VERBOSE) log_write(LOG_LEVEL_VERBOSE, LOG_TAG, fmt, ##__VA_ARGS__); } while(0)

#endif /* LOG_H */
