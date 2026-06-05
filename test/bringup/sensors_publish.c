/*
 * Bring-up — sensor reads + MQTT publish over the proven mTLS path.
 *
 * This is the supervisor-demo target: extends bringup_mqtt with real sensor
 * data. Each sensor lives behind a simple #ifdef so we can flip them on/off
 * as drivers come online: BME280 → rmms/<uuid>/env, ENS160 → /air,
 * MR60BHA2 → /radar (NYI).
 *
 *   storage_mount() → identity_load() + cfg_load_wifi/broker() from littlefs
 *   one task: wait stdio → load identity/cfg → cyw43 init → wifi → mTLS
 *           → MQTT CONNECT → 1 Hz loop: read each enabled sensor, publish.
 *
 * Identity (UUID + cert chain) and per-deployment config (Wi-Fi creds,
 * broker address, radar choice) live in littlefs — provisioned once with
 * bringup_provision + scripts/provision_device.py. Reflashing firmware
 * never touches them.
 */
#include "FreeRTOS.h"
#include "task.h"

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/altcp_tls.h"
#include "lwip/apps/mqtt.h"

#include "storage.h"
#include "identity.h"
#include "cfg.h"

#include "mbedtls/platform_time.h"

#include "board_pico2wh.h"
#include "bme280.h"
#include "ens160.h"
#include "sh1122.h"
#include "radar_driver.h"
#include "err.h"

#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "hardware/uart.h"

#include <stdio.h>
#include <string.h>

/* Sensor-enable switches — flip to 0 to disable a sensor at compile time
 * (e.g. while bringing up another one). Default: all on. */
#ifndef SENSOR_ENV_ON
#define SENSOR_ENV_ON   1
#endif
#ifndef SENSOR_AIR_ON
#define SENSOR_AIR_ON   1
#endif
#ifndef SENSOR_RADAR_ON
#define SENSOR_RADAR_ON 1
#endif
#ifndef OLED_ON
#define OLED_ON         1
#endif

/* FreeRTOSConfig hooks. */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName);
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    printf("[bringup] STACK OVERFLOW in '%s'\n", pcTaskName ? pcTaskName : "?");
    for (;;) { tight_loop_contents(); }
}
void vApplicationMallocFailedHook(void);
void vApplicationMallocFailedHook(void)
{
    printf("[bringup] malloc failed\n");
    for (;;) { tight_loop_contents(); }
}

/* Platform shim for MBEDTLS_PLATFORM_MS_TIME_ALT. */
mbedtls_ms_time_t mbedtls_ms_time(void)
{
    return (mbedtls_ms_time_t)(time_us_64() / 1000u);
}

/* ── MQTT state ─────────────────────────────────────────────────────────── */
static mqtt_client_t *g_mqtt;
static volatile bool g_mqtt_connected = false;
/* Reconnect / LWT plumbing — the mqtt_connect_client_info_t and altcp_tls
 * config must outlive the publish loop because mqtt_client_connect doesn't
 * copy the struct, and we want to reconnect on broker drop without re-doing
 * cert parsing. All four are touched only from the sensors task or from
 * lwIP callbacks; lwIP callbacks fire under the cyw43 background context
 * which is single-threaded against the sensors task's cyw43_arch_lwip_begin
 * critical sections. */
static struct altcp_tls_config       *s_tls_cfg = NULL;
static struct mqtt_connect_client_info_t s_ci;
static ip_addr_t                      s_broker_addr;
static char                           s_status_topic[80] = "";
static char                           s_topic_env[80] = "";
static char                           s_topic_air[80] = "";
static char                           s_topic_radar[80] = "";
static volatile bool                  s_reconnect_pending = false;
static uint32_t                       s_reconnect_backoff_ms = 1000;

/* Boot-time identity + per-deployment config loaded from littlefs. */
static Identity   g_id;
static CfgWifi    g_wifi;
static CfgBroker  g_broker;

/* Resolves to broker.ip if present, else broker.host. The bring-up needs an
 * IP literal because mDNS isn't wired in here; the production stack adds
 * resolution per §8.2. */
static const char *broker_addr_str(void)
{
    return g_broker.ip[0] ? g_broker.ip : g_broker.host;
}

#if OLED_ON
/* ── OLED state ─────────────────────────────────────────────────────────── */
static Sh1122 s_oled;
static bool   s_oled_ok = false;
static char   s_wifi_ip[16] = "----";
/* Last samples shown on the OLED — copied from the publish-side encoders
 * via the shadow vars below so the render task doesn't need to re-read I²C. */
static volatile float    s_disp_temp = 0.0f, s_disp_hum = 0.0f, s_disp_pres = 0.0f;
static volatile bool     s_disp_env_have = false;
static volatile uint16_t s_disp_co2 = 0, s_disp_tvoc = 0;
static volatile uint8_t  s_disp_aqi = 0, s_disp_air_q = 3;
static volatile bool     s_disp_air_have = false;
#endif

static void mqtt_pub_status_cb(void *arg, err_t result)
{
    (void)arg;
    if (result != ERR_OK) {
        printf("[bringup] status publish rc=%d\n", (int)result);
    }
}

static void mqtt_connect_cb(mqtt_client_t *c, void *arg, mqtt_connection_status_t status)
{
    (void)arg;
    printf("[bringup] MQTT CONNACK status=%d %s\n", status,
           status == MQTT_CONNECT_ACCEPTED ? "ACCEPTED" : "(not accepted)");
    bool was_connected = g_mqtt_connected;
    g_mqtt_connected = (status == MQTT_CONNECT_ACCEPTED);

    if (g_mqtt_connected) {
        /* Reset backoff; reconnects after a long outage stabilise fast. */
        s_reconnect_backoff_ms = 1000;
        /* Publish status=online retained (matches the LWT shape so the mirror
         * sees a clean transition). We're already in the lwIP background
         * context here, so no cyw43_arch_lwip_begin() needed. */
        if (s_status_topic[0] != '\0') {
            (void)mqtt_publish(c, s_status_topic, "online", 6,
                               /*qos=*/1, /*retain=*/1,
                               mqtt_pub_status_cb, NULL);
        }
    } else if (was_connected) {
        /* Disconnected after being up — kick off reconnect from the main
         * loop (we can't call mqtt_client_connect from inside this callback;
         * it would re-enter the client state machine). */
        s_reconnect_pending = true;
    }
}

static void mqtt_pub_cb(void *arg, err_t result)
{
    (void)arg;
    if (result != ERR_OK) {
        printf("[bringup] publish rc=%d\n", (int)result);
    }
}

/* ── Sensor state ───────────────────────────────────────────────────────── */
#if SENSOR_ENV_ON
static Bme280 s_bme;
static bool   s_bme_ok = false;
static uint32_t s_env_seq = 0;
#endif
#if SENSOR_AIR_ON
static Ens160 s_ens;
static bool   s_ens_ok = false;
static uint32_t s_air_seq = 0;
#endif
#if SENSOR_RADAR_ON
static radar_driver_t *s_radar = NULL;
static bool            s_radar_ok = false;
static uint32_t        s_radar_seq = 0;
#if OLED_ON
static volatile float    s_disp_heart = 0.0f, s_disp_breath = 0.0f;
static volatile uint32_t s_disp_dist_mm = 0;
static volatile bool     s_disp_radar_pres = false;
static volatile bool     s_disp_radar_have = false;
static volatile uint8_t  s_disp_radar_q   = 3;
#endif
#endif

/* env_sample_encode follows the snprintf template in CLAUDE.md §9.2.
 * Returns chars written (excluding NUL), or -1 on overflow. */
#if SENSOR_ENV_ON
static int env_sample_encode(char *buf, size_t cap,
                             uint64_t ts_us, int64_t wall_ms,
                             uint32_t seq, uint8_t q,
                             float temp_c, float hum_pct, float pres_hpa)
{
    int n = snprintf(buf, cap,
        "{\"ts_us\":%llu,\"wall_ms\":%lld,\"seq\":%u,\"q\":%u,"
        "\"v\":{\"temp_c\":%.3f,\"hum_pct\":%.3f,\"pres_hpa\":%.3f}}",
        (unsigned long long)ts_us, (long long)wall_ms,
        (unsigned)seq, (unsigned)q,
        (double)temp_c, (double)hum_pct, (double)pres_hpa);
    return (n > 0 && (size_t)n < cap) ? n : -1;
}

static void publish_env_sample(void)
{
    if (!s_bme_ok) return;

    Bme280Sample s;
    uint8_t q = 0;
    err_t e = bme280_read_sample(&s_bme, &s);
    if (e != ERR_OK) {
        printf("[bringup] BME280 read failed rc=%d\n", (int)e);
        q = 3;                     /* invalid */
        s.temp_c = s.humidity_pct = s.pressure_hpa = 0.0f;
    }

    char payload[256];
    int pn = env_sample_encode(payload, sizeof(payload),
                               time_us_64(), -1,    /* wall_ms unknown */
                               ++s_env_seq, q,
                               s.temp_c, s.humidity_pct, s.pressure_hpa);
    if (pn < 0) {
        printf("[bringup] env encode overflow\n");
        return;
    }

    cyw43_arch_lwip_begin();
    err_t pe = mqtt_publish(g_mqtt, s_topic_env, payload, (u16_t)pn,
                            /*qos=*/1, /*retain=*/0, mqtt_pub_cb, NULL);
    cyw43_arch_lwip_end();
    if (pe != ERR_OK) {
        printf("[bringup] env mqtt_publish immediate rc=%d\n", (int)pe);
    } else if (q == 0) {
        printf("[bringup] env T=%.2fC H=%.2f%% P=%.2fhPa seq=%u\n",
               (double)s.temp_c, (double)s.humidity_pct, (double)s.pressure_hpa,
               (unsigned)s_env_seq);
    }
#if OLED_ON
    /* Update OLED shadow regardless of mqtt success — the display is local
     * UX, not gated on the broker round-trip. */
    s_disp_temp = s.temp_c;
    s_disp_hum  = s.humidity_pct;
    s_disp_pres = s.pressure_hpa;
    s_disp_env_have = (q == 0);
#endif
}
#endif /* SENSOR_ENV_ON */

#if SENSOR_AIR_ON
/* air JSON encoder per CLAUDE.md §9.2.2:
 * {"ts_us":...,"wall_ms":...,"seq":...,"q":...,"v":{"co2_ppm":...,"tvoc_ppb":...,"aqi":...}} */
static int air_sample_encode(char *buf, size_t cap,
                             uint64_t ts_us, int64_t wall_ms,
                             uint32_t seq, uint8_t q,
                             uint16_t co2_ppm, uint16_t tvoc_ppb, uint8_t aqi)
{
    int n = snprintf(buf, cap,
        "{\"ts_us\":%llu,\"wall_ms\":%lld,\"seq\":%u,\"q\":%u,"
        "\"v\":{\"co2_ppm\":%u,\"tvoc_ppb\":%u,\"aqi\":%u}}",
        (unsigned long long)ts_us, (long long)wall_ms,
        (unsigned)seq, (unsigned)q,
        (unsigned)co2_ppm, (unsigned)tvoc_ppb, (unsigned)aqi);
    return (n > 0 && (size_t)n < cap) ? n : -1;
}

static void publish_air_sample(void)
{
    if (!s_ens_ok) return;

    Ens160Sample s;
    uint8_t q = 0;
    err_t e = ens160_read_sample(&s_ens, &s);
    if (e != ERR_OK) {
        printf("[bringup] ENS160 read failed rc=%d\n", (int)e);
        q = 3;
        s.aqi = 0; s.co2_ppm = 0; s.tvoc_ppb = 0;
    } else {
        /* Map ENS160 validity flag to the q field per CLAUDE.md §9.2.2:
         *   normal       → 0 ok
         *   warmup       → 2 degraded
         *   init-startup → 2 degraded
         *   invalid      → 3 invalid                                          */
        Ens160Validity v = ens160_validity(s.status);
        if      (v == ENS160_VALIDITY_NORMAL)  q = 0;
        else if (v == ENS160_VALIDITY_INVALID) q = 3;
        else                                   q = 2;
    }

    char payload[200];
    int pn = air_sample_encode(payload, sizeof(payload),
                               time_us_64(), -1,
                               ++s_air_seq, q,
                               s.co2_ppm, s.tvoc_ppb, s.aqi);
    if (pn < 0) {
        printf("[bringup] air encode overflow\n");
        return;
    }

    cyw43_arch_lwip_begin();
    err_t pe = mqtt_publish(g_mqtt, s_topic_air, payload, (u16_t)pn,
                            /*qos=*/1, /*retain=*/0, mqtt_pub_cb, NULL);
    cyw43_arch_lwip_end();
    if (pe != ERR_OK) {
        printf("[bringup] air mqtt_publish immediate rc=%d\n", (int)pe);
    } else {
        printf("[bringup] air AQI=%u CO2=%u TVOC=%u q=%u seq=%u\n",
               s.aqi, s.co2_ppm, s.tvoc_ppb, q, (unsigned)s_air_seq);
    }
#if OLED_ON
    s_disp_co2  = s.co2_ppm;
    s_disp_tvoc = s.tvoc_ppb;
    s_disp_aqi  = s.aqi;
    s_disp_air_q = q;
    s_disp_air_have = true;
#endif
}
#endif /* SENSOR_AIR_ON */

#if SENSOR_RADAR_ON
/* radar JSON encoder per CLAUDE.md §9.2.2:
 * {"ts_us":...,"wall_ms":...,"seq":...,"q":...,
 *  "v":{"presence":bool,"distance_mm":int|null,"breath_bpm":float|null,
 *       "heart_bpm":float|null}}
 * §9.2.3 lets us emit `null` when the radar didn't produce that field
 * recently; we do that when distance_mm == 0 (sentinel from the driver). */
static int radar_sample_encode(char *buf, size_t cap,
                               uint64_t ts_us, int64_t wall_ms,
                               uint32_t seq, uint8_t q,
                               const RadarSample *s)
{
    char dist[16];
    if (s->distance_mm == 0u) snprintf(dist, sizeof(dist), "null");
    else                       snprintf(dist, sizeof(dist), "%lu",
                                        (unsigned long)s->distance_mm);
    /* Heart/breath: encode as null if the driver returned 0.0 (no fresh
     * latched value). Keeps the mirror tile in "—" instead of showing 0. */
    char hb[16], br[16];
    if (s->heart_bpm  > 0.0f) snprintf(hb, sizeof(hb), "%.1f", (double)s->heart_bpm);
    else                       snprintf(hb, sizeof(hb), "null");
    if (s->breath_rpm > 0.0f) snprintf(br, sizeof(br), "%.1f", (double)s->breath_rpm);
    else                       snprintf(br, sizeof(br), "null");

    int n = snprintf(buf, cap,
        "{\"ts_us\":%llu,\"wall_ms\":%lld,\"seq\":%u,\"q\":%u,"
        "\"v\":{\"presence\":%s,\"distance_mm\":%s,"
              "\"breath_bpm\":%s,\"heart_bpm\":%s}}",
        (unsigned long long)ts_us, (long long)wall_ms,
        (unsigned)seq, (unsigned)q,
        s->presence ? "true" : "false", dist, br, hb);
    return (n > 0 && (size_t)n < cap) ? n : -1;
}

static void publish_radar_sample(void)
{
    if (!s_radar_ok) return;

    RadarSample s;
    /* 800 ms budget — short enough to leave headroom in the 1 Hz tick,
     * long enough to almost always catch a fresh breath/heart frame. */
    err_t e = s_radar->read_sample(s_radar->ctx, &s, 800);
    uint8_t q = s.q;
    if (e == ERR_TIMEOUT) {
        /* No frames in this window: keep going but mark invalid. The driver
         * sets q=3 in this case already. */
    } else if (e != ERR_OK) {
        printf("[bringup] radar read failed rc=%d\n", (int)e);
        q = 3;
    }

    char payload[256];
    int pn = radar_sample_encode(payload, sizeof(payload),
                                 time_us_64(), -1,
                                 ++s_radar_seq, q, &s);
    if (pn < 0) {
        printf("[bringup] radar encode overflow\n");
        return;
    }

    cyw43_arch_lwip_begin();
    err_t pe = mqtt_publish(g_mqtt, s_topic_radar, payload, (u16_t)pn,
                            /*qos=*/1, /*retain=*/0, mqtt_pub_cb, NULL);
    cyw43_arch_lwip_end();
    if (pe != ERR_OK) {
        printf("[bringup] radar mqtt_publish immediate rc=%d\n", (int)pe);
    } else if (q == 0) {
        printf("[bringup] radar pres=%d dist=%lumm BR=%.1f HR=%.1f q=%u seq=%u\n",
               (int)s.presence, (unsigned long)s.distance_mm,
               (double)s.breath_rpm, (double)s.heart_bpm, q,
               (unsigned)s_radar_seq);
    }
#if OLED_ON
    s_disp_heart   = s.heart_bpm;
    s_disp_breath  = s.breath_rpm;
    s_disp_dist_mm = s.distance_mm;
    s_disp_radar_pres = s.presence;
    s_disp_radar_q    = q;
    s_disp_radar_have = (q != 3);
#endif
}
#endif /* SENSOR_RADAR_ON */

/* ── Initialisation helpers ─────────────────────────────────────────────── */

static void i2c_bus_init(void)
{
    i2c_init(BOARD_I2C_INST, BOARD_I2C_FREQ_HZ);
    gpio_set_function(BOARD_I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(BOARD_I2C_SCL_PIN, GPIO_FUNC_I2C);
    /* Most breakouts have onboard 10k pullups; enable internal pullups too
     * as belt-and-braces (RP2350 internal pullup ≈ 50–80 kΩ). */
    gpio_pull_up(BOARD_I2C_SDA_PIN);
    gpio_pull_up(BOARD_I2C_SCL_PIN);
    printf("[bringup] I²C0 init on SDA=GP%d SCL=GP%d @ %u Hz\n",
           BOARD_I2C_SDA_PIN, BOARD_I2C_SCL_PIN, (unsigned)BOARD_I2C_FREQ_HZ);
}

/* Quick I²C presence scan — useful diagnostic when a sensor init fails. */
static void i2c_scan(void)
{
    printf("[bringup] I²C scan:");
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        uint8_t dummy = 0;
        int rc = i2c_read_blocking(BOARD_I2C_INST, addr, &dummy, 1, false);
        if (rc >= 0) printf(" 0x%02x", addr);
    }
    printf("\n");
}

static void mqtt_setup_and_connect(void)
{
    printf("[bringup] building altcp_tls config (mTLS, ECDSA P-256)\n");
    s_tls_cfg = altcp_tls_create_config_client_2wayauth(
        g_id.ca_der,      g_id.ca_len,
        g_id.dev_key_der, g_id.dev_key_len,
        NULL, 0,
        g_id.dev_crt_der, g_id.dev_crt_len);
    if (!s_tls_cfg) {
        for (;;) { printf("[bringup] altcp_tls FAILED\n"); vTaskDelay(pdMS_TO_TICKS(3000)); }
    }

    g_mqtt = mqtt_client_new();
    if (!g_mqtt) {
        for (;;) { printf("[bringup] mqtt_client_new FAILED\n"); vTaskDelay(pdMS_TO_TICKS(3000)); }
    }

    /* Build per-UUID topics once — used for status (LWT + online), env, air. */
    snprintf(s_status_topic, sizeof(s_status_topic), "rmms/%s/status", g_id.uuid);
    snprintf(s_topic_env,    sizeof(s_topic_env),    "rmms/%s/env",    g_id.uuid);
    snprintf(s_topic_air,    sizeof(s_topic_air),    "rmms/%s/air",    g_id.uuid);
    snprintf(s_topic_radar,  sizeof(s_topic_radar),  "rmms/%s/radar",  g_id.uuid);

    memset(&s_ci, 0, sizeof(s_ci));
    s_ci.client_id  = g_id.uuid;
    s_ci.keep_alive = 60;
    s_ci.tls_config = s_tls_cfg;
    /* LWT: broker publishes status=offline (retained QoS 1) on our behalf
     * whenever it stops seeing keepalives. The mirror sees device-offline
     * within keepalive*1.5 = 90 s after a hard Pico drop. */
    s_ci.will_topic  = s_status_topic;
    s_ci.will_msg    = (const u8_t *)"offline";
    s_ci.will_qos    = 1;
    s_ci.will_retain = 1;

    if (!ipaddr_aton(broker_addr_str(), &s_broker_addr)) {
        for (;;) {
            printf("[bringup] broker addr '%s' not an IP literal\n", broker_addr_str());
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }
    printf("[bringup] mqtt_client_connect → %s:%u\n",
           broker_addr_str(), (unsigned)g_broker.port);
    cyw43_arch_lwip_begin();
    err_t e = mqtt_client_connect(g_mqtt, &s_broker_addr, g_broker.port,
                                  mqtt_connect_cb, NULL, &s_ci);
    cyw43_arch_lwip_end();
    if (e != ERR_OK) {
        printf("[bringup] mqtt_client_connect immediate rc=%d\n", (int)e);
    }
}

/* Reconnect helper — called from the main loop when s_reconnect_pending is
 * set OR when we notice mqtt_client_is_connected() returned false. Uses
 * exponential backoff capped at 30 s. */
static void mqtt_try_reconnect(void)
{
    if (g_mqtt_connected) return;   /* nothing to do */
    printf("[bringup] mqtt reconnect → %s:%u (backoff %lu ms)\n",
           broker_addr_str(), (unsigned)g_broker.port,
           (unsigned long)s_reconnect_backoff_ms);
    cyw43_arch_lwip_begin();
    err_t e = mqtt_client_connect(g_mqtt, &s_broker_addr, g_broker.port,
                                  mqtt_connect_cb, NULL, &s_ci);
    cyw43_arch_lwip_end();
    if (e != ERR_OK) {
        printf("[bringup] mqtt reconnect immediate rc=%d\n", (int)e);
    }
    s_reconnect_pending = false;
    /* Double next backoff (cap 30 s). Reset on CONNACK ACCEPTED. */
    s_reconnect_backoff_ms = (s_reconnect_backoff_ms * 2u);
    if (s_reconnect_backoff_ms > 30000u) s_reconnect_backoff_ms = 30000u;
}

/* ── Main task ──────────────────────────────────────────────────────────── */

#if OLED_ON
/* ── OLED render task — independent loop that polls shadow state and
 *    cycles through 4 status pages every 3 s. Runs at low priority so
 *    sensor + MQTT work isn't delayed by I²C frame writes. ────────────── */

static const char *aqi_str(uint8_t aqi)
{
    switch (aqi) {
    case 1: return "EXCELLENT";
    case 2: return "GOOD";
    case 3: return "MODERATE";
    case 4: return "POOR";
    case 5: return "UNHEALTHY";
    default: return "-";
    }
}

static void oled_page_indicator(uint8_t n)
{
    char line[8]; snprintf(line, sizeof(line), "%u/4", (unsigned)n);
    sh1122_draw_text(&s_oled, 240, 56, 1, line);
}

static void render_page_net(uint32_t uptime_s)
{
    sh1122_clear(&s_oled);
    sh1122_draw_text(&s_oled, 0, 0, 2, "NETWORK + BROKER");
    char line[40];
    snprintf(line, sizeof(line), "WIFI %s", s_wifi_ip);
    sh1122_draw_text(&s_oled, 0, 18, 2, line);
    snprintf(line, sizeof(line), "BROKER %s", broker_addr_str());
    sh1122_draw_text(&s_oled, 0, 36, 2, line);
    snprintf(line, sizeof(line), "MQTT %s  UP %lus",
             g_mqtt_connected ? "OK" : "--", (unsigned long)uptime_s);
    sh1122_draw_text(&s_oled, 0, 50, 1, line);
    oled_page_indicator(1);
}

static void render_page_env(uint32_t pub_count)
{
    sh1122_clear(&s_oled);
    sh1122_draw_text(&s_oled, 0, 0, 2, "ENV  /env");
    char line[40];
    if (s_disp_env_have) {
        snprintf(line, sizeof(line), "T %5.1f C", (double)s_disp_temp);
        sh1122_draw_text(&s_oled, 0, 18, 2, line);
        snprintf(line, sizeof(line), "H %5.1f %%", (double)s_disp_hum);
        sh1122_draw_text(&s_oled, 0, 36, 2, line);
        snprintf(line, sizeof(line), "P %6.1f HPA", (double)s_disp_pres);
        sh1122_draw_text(&s_oled, 124, 18, 2, line);
        snprintf(line, sizeof(line), "PUBLISHED %lu", (unsigned long)pub_count);
        sh1122_draw_text(&s_oled, 124, 36, 1, line);
    } else {
        sh1122_draw_text(&s_oled, 0, 18, 2, "WAITING FOR SAMPLE");
    }
    oled_page_indicator(2);
}

static void render_page_air(uint32_t pub_count)
{
    sh1122_clear(&s_oled);
    sh1122_draw_text(&s_oled, 0, 0, 2, "AIR  /air");
    char line[40];
    if (s_disp_air_have) {
        if (s_disp_air_q == 0) {
            snprintf(line, sizeof(line), "AQI %u (%s)", s_disp_aqi, aqi_str(s_disp_aqi));
            sh1122_draw_text(&s_oled, 0, 18, 2, line);
            snprintf(line, sizeof(line), "CO2 %u PPM", s_disp_co2);
            sh1122_draw_text(&s_oled, 0, 36, 2, line);
            snprintf(line, sizeof(line), "TVOC %u PPB", s_disp_tvoc);
            sh1122_draw_text(&s_oled, 140, 36, 2, line);
        } else {
            snprintf(line, sizeof(line), "WARMING UP  q=%u", (unsigned)s_disp_air_q);
            sh1122_draw_text(&s_oled, 0, 18, 2, line);
            snprintf(line, sizeof(line), "CO2 %u  TVOC %u", s_disp_co2, s_disp_tvoc);
            sh1122_draw_text(&s_oled, 0, 36, 2, line);
        }
        snprintf(line, sizeof(line), "PUBLISHED %lu", (unsigned long)pub_count);
        sh1122_draw_text(&s_oled, 0, 56, 1, line);
    } else {
        sh1122_draw_text(&s_oled, 0, 18, 2, "WAITING FOR SAMPLE");
    }
    oled_page_indicator(3);
}

static void render_page_build(void)
{
    sh1122_clear(&s_oled);
    sh1122_draw_text(&s_oled, 0, 0, 2, "BUILD INFO");
    sh1122_draw_text(&s_oled, 0, 18, 2, "BOARD PICO 2 WH");
    sh1122_draw_text(&s_oled, 0, 36, 2, "RP2350 / FREERTOS");
    sh1122_draw_text(&s_oled, 0, 50, 1, "BRINGUP_SENSORS");
    oled_page_indicator(4);
}

static void render_task(void *arg)
{
    (void)arg;
    /* Wait until the sensors task has had a chance to init the OLED. */
    while (!s_oled_ok) vTaskDelay(pdMS_TO_TICKS(200));

    TickType_t boot_tick = xTaskGetTickCount();
    TickType_t last_cycle = boot_tick;
    uint8_t page = 0;

    for (;;) {
        TickType_t now = xTaskGetTickCount();
        if ((now - last_cycle) >= pdMS_TO_TICKS(3000)) {
            page = (page + 1) & 0x03U;
            last_cycle = now;
        }
        uint32_t uptime_s = (uint32_t)((now - boot_tick) / configTICK_RATE_HZ);

        switch (page) {
        case 0: render_page_net(uptime_s); break;
        case 1: render_page_env(s_env_seq); break;
        case 2: render_page_air(s_air_seq); break;
        case 3: render_page_build(); break;
        default: render_page_net(uptime_s); break;
        }
        (void)sh1122_flush(&s_oled);

        vTaskDelay(pdMS_TO_TICKS(300));   /* ~3 Hz refresh */
    }
}
#endif /* OLED_ON */

static void sensors_task(void *arg)
{
    (void)arg;

    stdio_init_all();
    while (!stdio_usb_connected()) vTaskDelay(pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(500));

    printf("\n[bringup] sensors_task on core=%u\n", (unsigned)get_core_num());

    /* Load identity + per-deployment config from littlefs. Anything missing
     * means the device wasn't provisioned — block with a loud error rather
     * than fall through to Wi-Fi/MQTT with garbage. */
    if (storage_mount() != ERR_OK)
        for (;;) { printf("[bringup] storage_mount FAILED\n"); vTaskDelay(pdMS_TO_TICKS(3000)); }
    if (identity_load(&g_id) != ERR_OK)
        for (;;) { printf("[bringup] identity_load FAILED — not provisioned\n"); vTaskDelay(pdMS_TO_TICKS(3000)); }
    if (cfg_load_wifi(&g_wifi) != ERR_OK)
        for (;;) { printf("[bringup] cfg_load_wifi FAILED — /cfg/wifi.json missing\n"); vTaskDelay(pdMS_TO_TICKS(3000)); }
    if (cfg_load_broker(&g_broker) != ERR_OK)
        for (;;) { printf("[bringup] cfg_load_broker FAILED — /cfg/broker.json missing\n"); vTaskDelay(pdMS_TO_TICKS(3000)); }

    printf("[bringup] UUID = %s\n", g_id.uuid);
    printf("[bringup] broker = %s:%u\n", broker_addr_str(), (unsigned)g_broker.port);

    /* ── I²C + sensor probes (independent of network) ────────────────── */
    i2c_bus_init();
    i2c_scan();

#if SENSOR_ENV_ON
    err_t e = bme280_init(&s_bme, BOARD_I2C_INST, BOARD_BME280_ADDR);
    s_bme_ok = (e == ERR_OK);
    printf("[bringup] BME280 init %s (rc=%d)\n", s_bme_ok ? "OK" : "FAILED", (int)e);
#endif
#if SENSOR_AIR_ON
    err_t ea = ens160_init(&s_ens, BOARD_I2C_INST, BOARD_ENS160_ADDR);
    s_ens_ok = (ea == ERR_OK);
    printf("[bringup] ENS160 init %s (rc=%d)\n", s_ens_ok ? "OK" : "FAILED", (int)ea);
#endif
#if OLED_ON
    err_t eo = sh1122_init(&s_oled, BOARD_I2C_INST, BOARD_OLED_ADDR);
    s_oled_ok = (eo == ERR_OK);
    printf("[bringup] SH1122 init %s (rc=%d)\n", s_oled_ok ? "OK" : "FAILED", (int)eo);
#endif
#if SENSOR_RADAR_ON
    /* For now the bring-up only supports the BHA2; C1001 selection lives in
     * /cfg/sensors.json but is not exercised here yet. */
    s_radar = radar_bha2_driver();
    err_t er = s_radar ? s_radar->init(s_radar->ctx, BOARD_RADAR_UART_INST) : ERR_FAIL;
    s_radar_ok = (er == ERR_OK);
    printf("[bringup] radar(%s) init %s (rc=%d)\n",
           s_radar ? s_radar->name : "?",
           s_radar_ok ? "OK" : "FAILED", (int)er);
#endif

    /* ── Wi-Fi + mTLS + MQTT ─────────────────────────────────────────── */
    if (cyw43_arch_init_with_country(CYW43_COUNTRY_NETHERLANDS) != 0) {
        for (;;) { printf("[bringup] cyw43 init FAILED\n"); vTaskDelay(pdMS_TO_TICKS(3000)); }
    }
    cyw43_arch_enable_sta_mode();
    printf("[bringup] connecting to SSID '%s' ...\n", g_wifi.ssid);
    int rc = cyw43_arch_wifi_connect_timeout_ms(g_wifi.ssid, g_wifi.psk,
                                                CYW43_AUTH_WPA2_AES_PSK, 30000);
    if (rc != 0) {
        for (;;) { printf("[bringup] wifi connect FAILED rc=%d\n", rc); vTaskDelay(pdMS_TO_TICKS(3000)); }
    }
    const ip4_addr_t *ip = netif_ip4_addr(netif_default);
    printf("[bringup] CONNECTED — IP = %s\n", ip ? ip4addr_ntoa(ip) : "(none)");
#if OLED_ON
    snprintf(s_wifi_ip, sizeof(s_wifi_ip), "%s", ip ? ip4addr_ntoa(ip) : "----");
#endif

    mqtt_setup_and_connect();

    /* ── 1 Hz publish loop with auto-reconnect ───────────────────────── */
    TickType_t next_reconnect_attempt = 0;
    for (uint32_t tick = 0; ; tick++) {
        /* Some failure modes (broker hard-drop, network blip) take down the
         * MQTT TCP connection without firing the disconnect callback. Use
         * lwIP's own is_connected() as the source of truth. */
        if (g_mqtt_connected && !mqtt_client_is_connected(g_mqtt)) {
            printf("[bringup] mqtt_client_is_connected went false — marking down\n");
            g_mqtt_connected = false;
            s_reconnect_pending = true;
        }

        if (!g_mqtt_connected) {
            TickType_t now = xTaskGetTickCount();
            if (s_reconnect_pending && now >= next_reconnect_attempt) {
                mqtt_try_reconnect();
                next_reconnect_attempt = now + pdMS_TO_TICKS(s_reconnect_backoff_ms);
            } else if ((tick % 5) == 0) {
                printf("[bringup] waiting for MQTT CONNACK ...\n");
            }
        } else {
#if SENSOR_ENV_ON
            publish_env_sample();
#endif
#if SENSOR_AIR_ON
            /* Publish /air every 5 s — ENS160 produces a fresh reading
             * once a second, but the AQI / CO2 / TVOC change slowly and the
             * mirror UI doesn't need 1 Hz updates. Reduces broker load. */
            if ((tick % 5) == 0) publish_air_sample();
#endif
#if SENSOR_RADAR_ON
            /* Radar at 1 Hz — heart/breath change visibly that fast and the
             * tile should track. read_sample inside publish_radar_sample
             * consumes up to 800 ms of UART, dovetailing with the 1 Hz tick. */
            publish_radar_sample();
#endif
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

int main(void)
{
    /* Same core-0 affinity as bringup_mqtt (sys_freertos cyw43 is not SMP-clean
     * with this pico-sdk rev; see memory project-cyw43-sys-freertos-hang). */
    xTaskCreateAffinitySet(sensors_task, "sensors", 8192, NULL, 2, 0x01, NULL);
#if OLED_ON
    /* OLED render runs at lower priority + core 0 so the sensor+MQTT task
     * always pre-empts it. ~3 KB stack is enough for snprintf + sh1122. */
    xTaskCreateAffinitySet(render_task,  "oled",    3072, NULL, 1, 0x01, NULL);
#endif
    vTaskStartScheduler();
    for (;;) { tight_loop_contents(); }
}
