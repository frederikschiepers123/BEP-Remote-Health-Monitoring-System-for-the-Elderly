#ifndef ERR_H
#define ERR_H

#include <stdint.h>

/* All fallible functions return err_t.  ERR_OK == 0 so callers can write
 * `if (err) { ... }`.  Never use errno.  Never use bool to signal failure. */
typedef int32_t err_t;

#define ERR_OK              ((err_t)  0)
#define ERR_FAIL            ((err_t) -1)    /* generic unspecified failure */
#define ERR_TIMEOUT         ((err_t) -2)
#define ERR_NOT_FOUND       ((err_t) -3)
#define ERR_NO_MEM          ((err_t) -4)
#define ERR_INVALID_ARG     ((err_t) -5)
#define ERR_IO              ((err_t) -6)    /* hardware I/O error */
#define ERR_BUSY            ((err_t) -7)
#define ERR_NOT_INIT        ((err_t) -8)
#define ERR_OVERFLOW        ((err_t) -9)
#define ERR_TLS             ((err_t) -10)
#define ERR_MQTT            ((err_t) -11)
#define ERR_FS              ((err_t) -12)
#define ERR_PARSE           ((err_t) -13)   /* malformed input (JSON, frame, etc.) */

/* Convenience: propagate an error up the call stack. */
#define TRY(expr)                   \
    do {                            \
        err_t _e = (expr);          \
        if (_e != ERR_OK) return _e;\
    } while (0)

#endif /* ERR_H */
