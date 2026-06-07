#define LOG_TAG "CFG"
#include "cfg.h"

#include "log.h"
#include "storage.h"
#include "json_parse.h"

#include <string.h>
#include <stdint.h>

#define CFG_MAX_JSON 512

static err_t load_json(const char *path, char *buf, size_t cap, size_t *len_out,
                       JsonToken *toks, int max_toks, int *n_toks_out) {
    err_t e = storage_read(path, buf, cap, len_out);
    if (e != ERR_OK) return e;
    int n = json_tokenize(buf, *len_out, toks, (size_t)max_toks);
    if (n < 1) {
        LOG_E("%s: tokenize failed (%d)", path, n);
        return ERR_PARSE;
    }
    *n_toks_out = n;
    return ERR_OK;
}

err_t cfg_load_wifi(CfgWifi *out) {
    memset(out, 0, sizeof(*out));
    char buf[CFG_MAX_JSON];
    JsonToken toks[24];
    size_t len = 0;
    int n = 0;
    err_t e = load_json("/cfg/wifi.json", buf, sizeof(buf), &len, toks, 24, &n);
    if (e != ERR_OK) return e;

    e = json_get_string(buf, toks, n, "ssid", out->ssid, sizeof(out->ssid));
    if (e != ERR_OK) { LOG_E("wifi.json: 'ssid' missing"); return ERR_INVALID_ARG; }
    e = json_get_string(buf, toks, n, "psk",  out->psk,  sizeof(out->psk));
    if (e != ERR_OK) { LOG_E("wifi.json: 'psk' missing");  return ERR_INVALID_ARG; }
    /* country is optional; default to "NL" if absent. */
    if (json_get_string(buf, toks, n, "country",
                        out->country, sizeof(out->country)) != ERR_OK) {
        strcpy(out->country, "NL");
    }
    LOG_I("wifi: ssid='%s' country=%s", out->ssid, out->country);
    return ERR_OK;
}

err_t cfg_load_broker(CfgBroker *out) {
    memset(out, 0, sizeof(*out));
    char buf[CFG_MAX_JSON];
    JsonToken toks[24];
    size_t len = 0;
    int n = 0;
    err_t e = load_json("/cfg/broker.json", buf, sizeof(buf), &len, toks, 24, &n);
    if (e != ERR_OK) return e;

    /* host is optional, ip is optional, but at least one must be present. */
    (void)json_get_string(buf, toks, n, "host", out->host, sizeof(out->host));
    (void)json_get_string(buf, toks, n, "ip",   out->ip,   sizeof(out->ip));
    if (out->host[0] == '\0' && out->ip[0] == '\0') {
        LOG_E("broker.json: need at least one of 'host' or 'ip'");
        return ERR_INVALID_ARG;
    }
    int64_t port = 0;
    e = json_get_int64(buf, toks, n, "port", &port);
    if (e != ERR_OK || port <= 0 || port > 0xFFFF) {
        LOG_E("broker.json: invalid 'port'");
        return ERR_INVALID_ARG;
    }
    out->port = (uint16_t)port;
    LOG_I("broker: host='%s' ip='%s' port=%u",
          out->host, out->ip, (unsigned)out->port);
    return ERR_OK;
}

err_t cfg_load_sensors(CfgSensors *out) {
    memset(out, 0, sizeof(*out));
    char buf[CFG_MAX_JSON];
    JsonToken toks[16];
    size_t len = 0;
    int n = 0;
    err_t e = load_json("/cfg/sensors.json", buf, sizeof(buf), &len, toks, 16, &n);
    if (e != ERR_OK) return e;

    char kind[16];
    e = json_get_string(buf, toks, n, "radar", kind, sizeof(kind));
    if (e != ERR_OK) { LOG_E("sensors.json: 'radar' missing"); return ERR_INVALID_ARG; }
    if      (strcmp(kind, "bha2")  == 0) out->radar = CFG_RADAR_BHA2;
    else if (strcmp(kind, "c1001") == 0) out->radar = CFG_RADAR_C1001;
    else { LOG_E("sensors.json: unknown radar '%s'", kind); return ERR_INVALID_ARG; }

    /* `env` is optional; absent means BME280 (back-compat with existing
     * provisioned devices that pre-date the AHT21 footprint). */
    char env[16];
    out->env = CFG_ENV_DEFAULT;
    if (json_get_string(buf, toks, n, "env", env, sizeof(env)) == ERR_OK) {
        if      (strcmp(env, "bme280") == 0) out->env = CFG_ENV_BME280;
        else if (strcmp(env, "aht21")  == 0) out->env = CFG_ENV_AHT21;
        else { LOG_E("sensors.json: unknown env '%s'", env); return ERR_INVALID_ARG; }
    } else {
        snprintf(env, sizeof(env), "(default)");
    }

    /* `light` is optional; absent means BH1750 (the advanced-module default,
     * which is what's physically demoed in this project). See ADR-0001. */
    char light[16];
    out->light = CFG_LIGHT_DEFAULT;
    if (json_get_string(buf, toks, n, "light", light, sizeof(light)) == ERR_OK) {
        if      (strcmp(light, "bh1750") == 0) out->light = CFG_LIGHT_BH1750;
        else if (strcmp(light, "gl5516") == 0) out->light = CFG_LIGHT_GL5516;
        else { LOG_E("sensors.json: unknown light '%s'", light); return ERR_INVALID_ARG; }
    } else {
        snprintf(light, sizeof(light), "(default)");
    }

    LOG_I("sensors: radar=%s env=%s light=%s", kind, env, light);
    return ERR_OK;
}
