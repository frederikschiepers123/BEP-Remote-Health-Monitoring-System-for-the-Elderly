#define LOG_TAG "IDENTITY"
#include "identity.h"

#include "log.h"
#include "storage.h"
#include <stdlib.h>
#include <string.h>

static err_t load_file(const char *path, uint8_t **buf_out, size_t *len_out) {
    /* First read: get size. */
    size_t len = 0;
    err_t err = storage_read(path, NULL, 0, &len);
    /* littlefs read with NULL buf returns the file size. */
    if (err != ERR_OK && err != ERR_NOT_FOUND) return err;
    if (len == 0) return ERR_NOT_FOUND;

    uint8_t *buf = (uint8_t *)malloc(len);
    if (!buf) return ERR_NO_MEM;

    err = storage_read(path, buf, len, &len);
    if (err != ERR_OK) {
        free(buf);
        return err;
    }
    *buf_out = buf;
    *len_out = len;
    return ERR_OK;
}

err_t identity_load(Identity *out) {
    memset(out, 0, sizeof(*out));

    /* UUID from /cfg/device.json (stored as a plain UTF-8 string for now;
     * full JSON parsing can be added once jsmn is integrated). */
    size_t uuid_len = 0;
    err_t err = storage_read("/cfg/device.json",
                              out->uuid, IDENTITY_UUID_LEN - 1, &uuid_len);
    if (err != ERR_OK) {
        LOG_E("cannot read /cfg/device.json: %ld", (long)err);
        return err;
    }
    out->uuid[uuid_len] = '\0';
    LOG_I("UUID: %s", out->uuid);

    err = load_file("/certs/dev.key", &out->dev_key_der, &out->dev_key_len);
    if (err != ERR_OK) { LOG_E("/certs/dev.key missing"); goto fail; }

    err = load_file("/certs/dev.crt", &out->dev_crt_der, &out->dev_crt_len);
    if (err != ERR_OK) { LOG_E("/certs/dev.crt missing"); goto fail; }

    err = load_file("/certs/ca.der",  &out->ca_der, &out->ca_len);
    if (err != ERR_OK) { LOG_E("/certs/ca.der missing"); goto fail; }

    return ERR_OK;

fail:
    identity_free(out);
    return err;
}

void identity_free(Identity *id) {
    free(id->dev_key_der); id->dev_key_der = NULL;
    free(id->dev_crt_der); id->dev_crt_der = NULL;
    free(id->ca_der);      id->ca_der      = NULL;
}
