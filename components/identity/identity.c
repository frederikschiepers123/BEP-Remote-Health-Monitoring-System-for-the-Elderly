#define LOG_TAG "IDENTITY"
#include "identity.h"

#include "log.h"
#include "storage.h"
#include "json_parse.h"
#include <stdlib.h>
#include <string.h>

static err_t load_file(const char *path, uint8_t **buf_out, size_t *len_out) {
    size_t len = 0;
    err_t err = storage_size(path, &len);
    if (err != ERR_OK) return err;
    if (len == 0) return ERR_NOT_FOUND;

    uint8_t *buf = (uint8_t *)malloc(len);
    if (!buf) return ERR_NO_MEM;

    size_t read_len = 0;
    err = storage_read(path, buf, len, &read_len);
    if (err != ERR_OK || read_len != len) {
        free(buf);
        return err != ERR_OK ? err : ERR_FS;
    }
    *buf_out = buf;
    *len_out = len;
    return ERR_OK;
}

err_t identity_load(Identity *out) {
    memset(out, 0, sizeof(*out));

    /* UUID from /cfg/device.json — provisioned as `{"_v":1,"uuid":"..."}`
     * per §11. Parse via the jsmn tokenizer; closed schema, top-level only. */
    char json_buf[128];
    size_t json_len = 0;
    err_t err = storage_read("/cfg/device.json",
                              json_buf, sizeof(json_buf), &json_len);
    if (err != ERR_OK) {
        LOG_E("cannot read /cfg/device.json: %ld", (long)err);
        return err;
    }
    JsonToken toks[8];
    int n = json_tokenize(json_buf, json_len, toks, 8);
    if (n < 1) {
        LOG_E("/cfg/device.json: tokenize failed (%d)", n);
        return ERR_PARSE;
    }
    err = json_get_string(json_buf, toks, n, "uuid",
                          out->uuid, IDENTITY_UUID_LEN);
    if (err != ERR_OK) {
        LOG_E("/cfg/device.json: 'uuid' field missing");
        return err;
    }
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
