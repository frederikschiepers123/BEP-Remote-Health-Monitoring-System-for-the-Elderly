#define LOG_TAG "TRANSPORT"

/* lwIP/pico/mbedTLS headers MUST come before the board "err.h": board's err.h
 * #defines ERR_OK as a macro, which would clobber lwIP's `enum err_enum_t`
 * (ERR_OK, ERR_INPROGRESS, …) if seen first.  lwIP first → its enum is fully
 * defined; the board macro (same value, 0) then shadows ERR_OK harmlessly.
 * (Same ordering the proven bring-up uses.) */
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/gpio.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/altcp_tls.h"
#include "lwip/apps/mqtt.h"
#include "lwip/dns.h"
#include "mbedtls/platform_time.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

/* Component headers (pull board err.h) — AFTER lwIP. */
#include "log.h"
#include "transport_mqtt.h"
#include "app_config.h"
#include "err.h"
#include "sensor_env.h"      /* q_env, EnvMsg */
#include "sensor_air.h"      /* q_air, AirMsg */
#include "radar_driver.h"    /* q_radar, RadarSample */
#include "sensor_light.h"    /* q_light, LightMsg */
#include "json_encode.h"
#include "ui_oled.h"
#include "board_pico2wh.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

extern void wdt_task_alive(WdtTaskId id);

/* Platform shim for MBEDTLS_PLATFORM_MS_TIME_ALT — the production firmware no
 * longer links the (USB-era) tls_context component, so the shim lives here,
 * with the sole remaining mbedTLS-over-lwIP user. */
mbedtls_ms_time_t mbedtls_ms_time(void)
{
    return (mbedtls_ms_time_t)(time_us_64() / 1000u);
}

/* ── Configuration references (set by transport_mqtt_init) ───────────────── */

static const Identity  *s_id     = NULL;
static const CfgWifi   *s_wifi   = NULL;
static const CfgBroker *s_broker = NULL;

/* ── MQTT / TLS state ────────────────────────────────────────────────────── */

static mqtt_client_t                     *s_mqtt = NULL;
static volatile bool                      s_connected = false;
static struct altcp_tls_config           *s_tls_cfg = NULL;
static struct mqtt_connect_client_info_t  s_ci;
static ip_addr_t                          s_broker_addr;
static bool                               s_wifi_ok = false;

static char s_topic_status[80] = "";
static char s_topic_env[80]    = "";
static char s_topic_air[80]    = "";
static char s_topic_radar[80]  = "";
static char s_topic_light[80]  = "";

static uint32_t s_seq_env = 0, s_seq_air = 0, s_seq_radar = 0, s_seq_light = 0;
static uint32_t s_backoff_ms = 1000;

/* ── Broker address resolution ───────────────────────────────────────────── */

static const char *broker_addr_str(void)
{
    return s_broker->ip[0] ? s_broker->ip : s_broker->host;
}

typedef struct {
    volatile bool done;
    ip_addr_t     addr;
    bool          found;
} dns_wait_t;

static void dns_found_cb(const char *name, const ip_addr_t *ipaddr, void *arg)
{
    (void)name;
    dns_wait_t *w = (dns_wait_t *)arg;
    if (ipaddr) { w->addr = *ipaddr; w->found = true; }
    w->done = true;
}

static bool resolve_host(const char *host, ip_addr_t *out, uint32_t timeout_ms)
{
    dns_wait_t w = { .done = false, .found = false };

    cyw43_arch_lwip_begin();
    err_t e = dns_gethostbyname(host, &w.addr, dns_found_cb, &w);
    cyw43_arch_lwip_end();

    if (e == ERR_OK) { *out = w.addr; return true; }   /* cached */
    if (e != ERR_INPROGRESS) {
        LOG_W("dns_gethostbyname('%s') immediate err=%d", host, (int)e);
        return false;
    }
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (!w.done && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (w.found) { *out = w.addr; }
    return w.found;
}

/* IP literal first (fast path, no DNS), else host via DNS/mDNS. */
static bool resolve_broker(uint32_t timeout_ms)
{
    if (s_broker->ip[0] && ipaddr_aton(s_broker->ip, &s_broker_addr)) {
        return true;
    }
    if (s_broker->host[0]) {
        LOG_I("resolving broker '%s' via DNS/mDNS ...", s_broker->host);
        if (resolve_host(s_broker->host, &s_broker_addr, timeout_ms)) {
            LOG_I("resolved %s -> %s", s_broker->host, ipaddr_ntoa(&s_broker_addr));
            return true;
        }
    }
    return false;
}

/* ── MQTT callbacks ──────────────────────────────────────────────────────── */

static void pub_cb(void *arg, err_t result)
{
    (void)arg;
    if (result != ERR_OK) { LOG_W("publish rc=%d", (int)result); }
}

static void connect_cb(mqtt_client_t *c, void *arg, mqtt_connection_status_t st)
{
    (void)arg;
    LOG_I("MQTT CONNACK status=%d %s", st,
          st == MQTT_CONNECT_ACCEPTED ? "ACCEPTED" : "(rejected)");
    s_connected = (st == MQTT_CONNECT_ACCEPTED);
    if (s_connected) {
        s_backoff_ms = 1000;   /* reset backoff on success */
        /* status=online retained (matches LWT shape). Already in lwIP ctx. */
        if (s_topic_status[0]) {
            (void)mqtt_publish(c, s_topic_status, "online", 6,
                               /*qos=*/1, /*retain=*/1, pub_cb, NULL);
        }
    }
}

static void mqtt_connect_now(void)
{
    LOG_I("mqtt_client_connect -> %s:%u (backoff %lu ms)",
          ipaddr_ntoa(&s_broker_addr), (unsigned)s_broker->port,
          (unsigned long)s_backoff_ms);
    cyw43_arch_lwip_begin();
    err_t e = mqtt_client_connect(s_mqtt, &s_broker_addr, s_broker->port,
                                  connect_cb, NULL, &s_ci);
    cyw43_arch_lwip_end();
    if (e != ERR_OK) { LOG_W("mqtt_client_connect immediate rc=%d", (int)e); }
    s_backoff_ms = (s_backoff_ms * 2u);
    if (s_backoff_ms > 30000u) { s_backoff_ms = 30000u; }
}

/* ── Network bring-up ────────────────────────────────────────────────────── */

static bool wifi_bring_up(void)
{
    if (cyw43_arch_init_with_country(CYW43_COUNTRY_NETHERLANDS) != 0) {
        LOG_E("cyw43 init FAILED");
        return false;
    }
    cyw43_arch_enable_sta_mode();
    LOG_I("connecting to SSID '%s' ...", s_wifi->ssid);
    int rc = cyw43_arch_wifi_connect_timeout_ms(s_wifi->ssid, s_wifi->psk,
                                                CYW43_AUTH_WPA2_AES_PSK, 30000);
    if (rc != 0) {
        /* rc=-7 BADAUTH, -8 CONNECT_FAILED (5 GHz/WPA3-only AP), -2 NONET. */
        LOG_W("wifi connect FAILED rc=%d — sensor-only mode", rc);
        return false;
    }
    const ip4_addr_t *ip = netif_ip4_addr(netif_default);
    LOG_I("CONNECTED — IP = %s", ip ? ip4addr_ntoa(ip) : "(none)");
    gpio_put(BOARD_LED_WIFI_PIN, ip ? 1 : 0);
    ui_oled_set_net(ip ? ip4addr_ntoa(ip) : "----", false);
    return true;
}

static bool mqtt_setup(void)
{
    LOG_I("building altcp_tls config (mTLS, ECDSA P-256)");
    s_tls_cfg = altcp_tls_create_config_client_2wayauth(
        s_id->ca_der,      s_id->ca_len,
        s_id->dev_key_der, s_id->dev_key_len,
        NULL, 0,
        s_id->dev_crt_der, s_id->dev_crt_len);
    if (!s_tls_cfg) { LOG_E("altcp_tls config FAILED"); return false; }

    s_mqtt = mqtt_client_new();
    if (!s_mqtt) { LOG_E("mqtt_client_new FAILED"); return false; }

    snprintf(s_topic_status, sizeof(s_topic_status), "rmms/%s/status", s_id->uuid);
    snprintf(s_topic_env,    sizeof(s_topic_env),    "rmms/%s/env",    s_id->uuid);
    snprintf(s_topic_air,    sizeof(s_topic_air),    "rmms/%s/air",    s_id->uuid);
    snprintf(s_topic_radar,  sizeof(s_topic_radar),  "rmms/%s/radar",  s_id->uuid);
    snprintf(s_topic_light,  sizeof(s_topic_light),  "rmms/%s/light",  s_id->uuid);

    memset(&s_ci, 0, sizeof(s_ci));
    s_ci.client_id   = s_id->uuid;
    s_ci.keep_alive  = MQTT_KEEPALIVE_WIFI;
    s_ci.tls_config  = s_tls_cfg;
    s_ci.will_topic  = s_topic_status;
    s_ci.will_msg    = (const u8_t *)"offline";
    s_ci.will_qos    = 1;
    s_ci.will_retain = 1;
    return true;
}

/* ── Per-sensor drain + publish ──────────────────────────────────────────── */

static void publish_payload(const char *topic, const char *buf, int n)
{
    if (n <= 0) { return; }
    if (!s_mqtt || !s_connected) { return; }
    cyw43_arch_lwip_begin();
    err_t e = mqtt_publish(s_mqtt, topic, buf, (u16_t)n,
                           /*qos=*/1, /*retain=*/0, pub_cb, NULL);
    cyw43_arch_lwip_end();
    if (e != ERR_OK) { LOG_W("mqtt_publish(%s) immediate rc=%d", topic, (int)e); }
}

static void drain_env(void)
{
    EnvMsg m; bool have = false;
    while (xQueueReceive(q_env, &m, 0) == pdTRUE) { have = true; }   /* latest */
    if (!have) { return; }

    JsonEnvBody b = {
        .temp_c = m.v.temp_c, .hum_pct = m.v.humidity_pct,
        .pres_hpa = m.v.pressure_hpa, .pres_valid = m.v.pressure_valid,
    };
    char buf[256];
    int n = json_encode_env(buf, sizeof(buf), time_us_64(), -1,
                            ++s_seq_env, m.q, &b);
    if (n > 0 && (size_t)n < sizeof(buf)) { publish_payload(s_topic_env, buf, n); }
    ui_oled_set_env(m.v.temp_c, m.v.humidity_pct, m.v.pressure_hpa,
                    m.v.pressure_valid, m.q);
}

static void drain_air(void)
{
    AirMsg m; bool have = false;
    while (xQueueReceive(q_air, &m, 0) == pdTRUE) { have = true; }
    if (!have) { return; }

    JsonAirBody b = { .co2_ppm = m.v.co2_ppm, .tvoc_ppb = m.v.tvoc_ppb,
                      .aqi = m.v.aqi };
    char buf[200];
    int n = json_encode_air(buf, sizeof(buf), time_us_64(), -1,
                            ++s_seq_air, m.q, &b);
    if (n > 0 && (size_t)n < sizeof(buf)) { publish_payload(s_topic_air, buf, n); }
    ui_oled_set_air(m.v.co2_ppm, m.v.tvoc_ppb, m.v.aqi, m.q);
}

static void drain_radar(void)
{
    RadarSample m; bool have = false;
    while (xQueueReceive(q_radar, &m, 0) == pdTRUE) { have = true; }
    if (!have) { return; }

    /* Driver sentinels → §9.2.2 nulls: distance 0 / rates ≤ 0 = "not measured". */
    JsonRadarBody b = {
        .presence    = m.presence,
        .distance_mm = (m.distance_mm == 0u) ? -1 : (int)m.distance_mm,
        .breath_bpm  = (m.breath_rpm > 0.0f) ? m.breath_rpm : -1.0f,
        .heart_bpm   = (m.heart_bpm  > 0.0f) ? m.heart_bpm  : -1.0f,
    };
    char buf[256];
    int n = json_encode_radar(buf, sizeof(buf), time_us_64(), -1,
                              ++s_seq_radar, m.q, &b);
    if (n > 0 && (size_t)n < sizeof(buf)) { publish_payload(s_topic_radar, buf, n); }
    ui_oled_set_radar(m.presence, m.distance_mm, m.breath_rpm, m.heart_bpm, m.q);
}

static void drain_light(void)
{
    LightMsg m; bool have = false;
    while (xQueueReceive(q_light, &m, 0) == pdTRUE) { have = true; }
    if (!have) { return; }

    JsonLightBody b = { .lux = m.v.lux };
    char buf[128];
    int n = json_encode_light(buf, sizeof(buf), time_us_64(), -1,
                              ++s_seq_light, m.q, &b);
    if (n > 0 && (size_t)n < sizeof(buf)) { publish_payload(s_topic_light, buf, n); }
    ui_oled_set_light(m.v.lux, m.q);
}

/* ── transport_mqtt_init ─────────────────────────────────────────────────── */

err_t transport_mqtt_init(const Identity *id, const CfgWifi *wifi,
                          const CfgBroker *broker)
{
    if (!id || !wifi || !broker) { return ERR_INVALID_ARG; }
    s_id = id; s_wifi = wifi; s_broker = broker;
    return ERR_OK;
}

/* ── transport_task ──────────────────────────────────────────────────────── */

void transport_task(void *arg)
{
    (void)arg;

    /* Do NOT heartbeat before bring-up: cyw43 init + Wi-Fi associate (up to
     * 30 s) blocks longer than the 2 s watchdog window.  The supervisor gives
     * un-armed tasks a free pass, so transport stays un-armed until its first
     * steady-state loop iteration below. */
    s_wifi_ok = wifi_bring_up();
    if (s_wifi_ok) {
        if (mqtt_setup() && resolve_broker(5000)) {
            mqtt_connect_now();
        } else {
            LOG_W("broker '%s' not resolvable yet — will keep retrying",
                  broker_addr_str());
        }
    } else {
        LOG_W("no Wi-Fi — sensor tasks still run; publishing disabled");
    }

    TickType_t next_attempt = 0;

    for (;;) {
        wdt_task_alive(WDT_TASK_TRANSPORT);

        /* Some drops (broker hard-kill, blip) don't fire the connect cb. */
        if (s_connected && !mqtt_client_is_connected(s_mqtt)) {
            LOG_W("mqtt link went down — marking offline");
            s_connected = false;
        }

        /* Reconnect with backoff; re-resolve each attempt (covers tablet/mDNS
         * coming back, or the broker IP moving). */
        if (s_wifi_ok && s_mqtt && !s_connected) {
            TickType_t now = xTaskGetTickCount();
            if (now >= next_attempt) {
                if (resolve_broker(2000)) {
                    mqtt_connect_now();
                } else {
                    LOG_W("broker '%s' still unresolved (backoff %lu ms)",
                          broker_addr_str(), (unsigned long)s_backoff_ms);
                    s_backoff_ms = (s_backoff_ms * 2u);
                    if (s_backoff_ms > 30000u) { s_backoff_ms = 30000u; }
                }
                next_attempt = now + pdMS_TO_TICKS(s_backoff_ms);
            }
        }

        /* Drain producer queues → encode → publish → OLED shadow. */
        drain_env();
        drain_air();
        drain_radar();
        drain_light();

        ui_oled_set_net(s_wifi_ok ? ip4addr_ntoa(netif_ip4_addr(netif_default))
                                  : "----",
                        s_connected);

        vTaskDelay(pdMS_TO_TICKS(1000));   /* ~1 Hz publish cadence */
    }
}
