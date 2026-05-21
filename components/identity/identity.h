#ifndef IDENTITY_H
#define IDENTITY_H

/* Loads the device's factory-provisioned identity from littlefs.
 *
 * Paths (factory-written, never modified in the field):
 *   /cfg/device.json   — UUID string
 *   /certs/dev.key     — ECDSA P-256 private key (DER)
 *   /certs/dev.crt     — device certificate (DER), signed by project CA
 *   /certs/ca.der      — project CA certificate (DER)
 *
 * No key generation, no CSR, no on-device enrollment.  See CLAUDE.md §9.4. */

#include "err.h"
#include <stddef.h>
#include <stdint.h>

#define IDENTITY_UUID_LEN   37   /* "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx\0" */

typedef struct {
    char    uuid[IDENTITY_UUID_LEN];
    uint8_t *dev_key_der;    /* heap-allocated; do not free after init */
    size_t   dev_key_len;
    uint8_t *dev_crt_der;
    size_t   dev_crt_len;
    uint8_t *ca_der;
    size_t   ca_len;
} Identity;

/* Load identity from littlefs.  Must be called after storage_mount().
 * Returns ERR_NOT_FOUND if any required file is absent (device not provisioned). */
err_t identity_load(Identity *out);

/* Release heap memory allocated by identity_load(). */
void  identity_free(Identity *id);

#endif /* IDENTITY_H */
