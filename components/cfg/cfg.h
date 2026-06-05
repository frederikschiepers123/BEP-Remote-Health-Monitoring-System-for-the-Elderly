#ifndef CFG_H
#define CFG_H

/* Per-deployment configuration loaded from littlefs at boot.
 *
 * Sourced from §11 canonical paths:
 *   /cfg/wifi.json      — {"_v":1,"ssid":"...","psk":"...","country":"NL"}
 *   /cfg/broker.json    — {"_v":1,"host":"tablet.local","ip":"...","port":8883}
 *   /cfg/sensors.json   — {"_v":1,"radar":"bha2"|"c1001"}
 *
 * All loaders return ERR_OK on success, ERR_NOT_FOUND if the file is absent
 * (device not yet provisioned), ERR_PARSE on malformed JSON, ERR_INVALID_ARG
 * if a required field is missing.
 *
 * No allocation — caller-owned structs only. Mount the filesystem
 * (storage_mount) before calling. */

#include "err.h"
#include <stdint.h>

/* WPA2-PSK limits per IEEE 802.11: SSID ≤ 32 bytes, PSK ≤ 63 bytes. */
typedef struct {
    char ssid[33];
    char psk[64];
    char country[3];   /* 2-letter ISO-3166-1 alpha-2 ("NL") */
} CfgWifi;

/* Broker address. Either `host` (mDNS / DNS name) or `ip` (literal) is enough;
 * production stack tries `ip` first, falls back to `host`. */
typedef struct {
    char     host[64];
    char     ip[40];     /* IPv4 dotted-quad or IPv6 literal; v1 uses v4 */
    uint16_t port;
} CfgBroker;

/* Radar driver selection (CLAUDE.md §3.2 / §7.4). */
typedef enum {
    CFG_RADAR_NONE  = 0,
    CFG_RADAR_BHA2,     /* Seeed MR60BHA2 — 60 GHz */
    CFG_RADAR_C1001,    /* DFRobot C1001  — 24 GHz */
} CfgRadarKind;

/* Environmental-sensor selection. The PCB exposes a single I²C footprint
 * that can be populated with either a BME280 (temp + humidity + pressure)
 * or an AHT21 (temp + humidity only). See CLAUDE.md §3.2. */
typedef enum {
    CFG_ENV_DEFAULT = 0,  /* sensors.json had no "env" field; treat as BME280 */
    CFG_ENV_BME280,
    CFG_ENV_AHT21,
} CfgEnvKind;

typedef struct {
    CfgRadarKind radar;
    CfgEnvKind   env;
} CfgSensors;

err_t cfg_load_wifi   (CfgWifi    *out);
err_t cfg_load_broker (CfgBroker  *out);
err_t cfg_load_sensors(CfgSensors *out);

#endif /* CFG_H */
