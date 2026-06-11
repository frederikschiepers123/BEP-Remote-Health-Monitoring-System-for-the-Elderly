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
#include "json_parse.h"      /* json_parse_time_set */
#include "spool.h"           /* NV outbound FIFO (ADR-0003) */
#include "storage.h"         /* seq persistence (/state/last_seq.json) */
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
static void           (*s_start_sensors)(void) = NULL;  /* run after bring-up */

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
static char s_topic_time[88]   = "";   /* rmms/<uuid>/time/set (subscribed) */

/* Per-topic envelope sequence numbers (§9.2.1). Stamped at INGEST time and
 * stored in the spool record, so a re-send after an outage carries the same
 * seq → the SBC's Observation.identifier is stable → idempotent (§9.6). */
static uint32_t s_seq_env = 0, s_seq_air = 0, s_seq_radar = 0, s_seq_light = 0;
static uint32_t s_backoff_ms = 1000;

/* Radar is decimated to ~1 Hz on the way into the spool (the producer runs at
 * 10 Hz). This timestamps the last radar record we spooled. */
static uint64_t s_last_radar_us = 0;

/* ── Wall clock (tablet time-sync, §9.2.5 / §16-Q6) ──────────────────────────
 * Anchor set from rmms/<uuid>/time/set; wall_now_ms() derives the absolute
 * epoch time of the current monotonic clock. Both writes and reads happen in
 * transport_task (the time/set payload is handed over via s_q_ctrl), so no
 * lock is needed. Records sampled before the first sync carry wall_ms = -1
 * (§9.2.1 sentinel); the Radxa substitutes a receive-time estimate (§9.6). */
static bool    s_wall_valid     = false;
static int64_t s_wall_offset_ms = 0;

static int64_t wall_now_ms(void)
{
    if (!s_wall_valid) { return -1; }
    return s_wall_offset_ms + (int64_t)(time_us_64() / 1000u);
}

/* ── Inbound control (lwIP callback → transport_task) ─────────────────────────
 * The MQTT inpub callbacks run in the cyw43/lwIP context — which is the tcpip
 * task under the sys_freertos arch, but a background/IRQ-driven context under
 * the threadsafe_background arch. A non-ISR FreeRTOS call (e.g. xQueueSend) from
 * the latter is unsafe, so the callback makes NO FreeRTOS call: it just copies
 * the completed time/set payload into a buffer and sets a flag. transport_task
 * snapshots it under cyw43_arch_lwip_begin/end, which excludes the callback
 * context in BOTH archs. The wall-clock anchor is thus written only in
 * transport_task. */
#define CTRL_PAYLOAD_MAX 96

/* In-progress reassembly (touched only in the lwIP callback context). */
static bool     s_in_is_time = false;
static bool     s_in_overflow = false;   /* payload exceeded the reassembly buffer */
static uint16_t s_in_len     = 0;
static char     s_in_buf[CTRL_PAYLOAD_MAX];

/* Completed time/set payload handed to transport_task (latest wins). */
static volatile bool s_time_pending  = false;
static volatile bool s_time_oversize = false;   /* a payload was dropped (task logs it) */
static char          s_time_buf[CTRL_PAYLOAD_MAX];
static uint16_t      s_time_len     = 0;

/* ── PUBACK-driven delivery state (in-flight = 1, §SPOOL_MAX_IN_FLIGHT) ──────
 * pub_cb_spool runs in the lwIP context; transport_task reads the state and
 * advances the spool. A single 32-bit store/load is atomic on the M33.
 *
 * Each publish carries a monotonic TOKEN (passed as the mqtt_publish `arg`).
 * pub_cb_spool only acts on a callback whose token matches the current one, and
 * the token is bumped on timeout / disconnect / CONNACK. This prevents a stale
 * PUBACK — e.g. a late ack for a record whose publish timed out and was retired
 * — from being misattributed to the NEXT record (which would clear it from the
 * spool unsent: silent QoS-1 loss). On a timeout we also drop the MQTT session
 * so lwIP cannot still have the original publish pending. */
typedef enum { PUB_IDLE = 0, PUB_PENDING, PUB_ACKED, PUB_FAILED } PubState;
static volatile PubState s_pub_state    = PUB_IDLE;
static volatile uint32_t s_pub_token    = 0;   /* identifies the in-flight publish */
static uint64_t          s_pending_ws   = 0;
static TickType_t        s_pub_deadline = 0;

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

/* ── seq persistence (§9.2.1 — survives reboots for idempotency) ─────────────
 * If seq restarted at 0 on every boot, post-reboot samples would collide with
 * pre-reboot Observation.identifiers and the FHIR server would drop them as
 * duplicates (data loss). We persist the counters periodically and, on boot,
 * resume a whole checkpoint interval AHEAD of the last saved value so no seq is
 * ever reused (at the cost of a harmless gap in the sequence per reboot). */
#define SEQ_CKPT 256u
/* If last_seq.json is present but corrupt, we cannot recover the true high-water
 * mark. Seeding here (≈1.07e9, ~8.5 years of runtime at 4 rec/s) is almost
 * certainly beyond any previously-used seq, so it avoids identifier reuse at the
 * cost of a one-time gap. Resetting to 0 would silently collide with prior
 * identifiers and make the FHIR server drop new readings as duplicates. */
#define SEQ_CORRUPT_SEED 0x40000000u
static uint32_t s_seq_ckpt_env, s_seq_ckpt_air, s_seq_ckpt_radar, s_seq_ckpt_light;

/* Persist the counters. Returns true only if the write actually hit flash, and
 * only then advances the checkpoint — so a failed persist is retried next pass
 * rather than being suppressed (which would widen the reuse window). */
static bool seq_persist(void)
{
    char buf[128];
    int n = snprintf(buf, sizeof(buf),
                     "{\"_v\":1,\"env\":%u,\"air\":%u,\"radar\":%u,\"light\":%u}",
                     (unsigned)s_seq_env, (unsigned)s_seq_air,
                     (unsigned)s_seq_radar, (unsigned)s_seq_light);
    if (n <= 0 || (size_t)n >= sizeof(buf)) { return false; }
    if (storage_write("/state/last_seq.json", buf, (size_t)n) != ERR_OK) {
        LOG_W("seq persist failed — will retry");
        return false;
    }
    s_seq_ckpt_env = s_seq_env;   s_seq_ckpt_air   = s_seq_air;
    s_seq_ckpt_radar = s_seq_radar; s_seq_ckpt_light = s_seq_light;
    return true;
}

static bool seq_field(const char *buf, const JsonToken *t, int n,
                      const char *key, uint32_t *out)
{
    int64_t v;
    if (json_get_int64(buf, t, n, key, &v) == ERR_OK && v >= 0) {
        *out = (uint32_t)v;
        return true;
    }
    return false;
}

static void seq_load(void)
{
    char buf[129];
    size_t len = 0;
    err_t e = storage_read("/state/last_seq.json", buf, sizeof(buf) - 1u, &len);

    if (e == ERR_NOT_FOUND) {
        s_seq_env = s_seq_air = s_seq_radar = s_seq_light = 0u;   /* fresh device */
    } else if (e == ERR_OK && len < sizeof(buf)) {
        buf[len] = '\0';
        JsonToken toks[24];
        int n = json_tokenize(buf, len, toks, 24);
        uint32_t ev, ai, ra, li;
        bool ok = n > 0
            && seq_field(buf, toks, n, "env",   &ev)
            && seq_field(buf, toks, n, "air",   &ai)
            && seq_field(buf, toks, n, "radar", &ra)
            && seq_field(buf, toks, n, "light", &li);
        if (ok) {
            /* Resume a whole checkpoint ahead so no value is ever reused. */
            s_seq_env = ev + SEQ_CKPT;   s_seq_air   = ai + SEQ_CKPT;
            s_seq_radar = ra + SEQ_CKPT; s_seq_light = li + SEQ_CKPT;
            LOG_I("seq resumed: env=%u air=%u radar=%u light=%u",
                  (unsigned)s_seq_env, (unsigned)s_seq_air,
                  (unsigned)s_seq_radar, (unsigned)s_seq_light);
        } else {
            LOG_E("last_seq.json corrupt — seeding seq high (0x%08x) to avoid reuse",
                  (unsigned)SEQ_CORRUPT_SEED);
            s_seq_env = s_seq_air = s_seq_radar = s_seq_light = SEQ_CORRUPT_SEED;
        }
    } else {
        LOG_E("last_seq.json read error %d — seeding seq high to avoid reuse", (int)e);
        s_seq_env = s_seq_air = s_seq_radar = s_seq_light = SEQ_CORRUPT_SEED;
    }

    /* The resumed/seeded counters MUST reach flash before we stamp anything, or
     * a quick reboot would reuse them. Retry a few times (atomic write). */
    bool persisted = false;
    for (int i = 0; i < 3 && !(persisted = seq_persist()); i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!persisted) {
        LOG_E("seq checkpoint not persisted at boot — reuse risk on reboot");
    }
}

static void seq_persist_maybe(void)
{
    if (s_seq_env   - s_seq_ckpt_env   >= SEQ_CKPT ||
        s_seq_air   - s_seq_ckpt_air   >= SEQ_CKPT ||
        s_seq_radar - s_seq_ckpt_radar >= SEQ_CKPT ||
        s_seq_light - s_seq_ckpt_light >= SEQ_CKPT) {
        seq_persist();
    }
}

/* ── MQTT callbacks ──────────────────────────────────────────────────────── */

static void pub_cb(void *arg, err_t result)   /* status/online — fire-and-forget */
{
    (void)arg;
    if (result != ERR_OK) { LOG_W("status publish rc=%d", (int)result); }
}

static void pub_cb_spool(void *arg, err_t result)   /* spool data — drives ack */
{
    /* Ignore a stale callback whose publish was already retired (timeout /
     * disconnect bumped the token) — otherwise its ack could be misattributed
     * to the record now in flight. */
    if ((uint32_t)(uintptr_t)arg != s_pub_token) { return; }
    if (s_pub_state == PUB_PENDING) {
        s_pub_state = (result == ERR_OK) ? PUB_ACKED : PUB_FAILED;
    }
}

static void sub_cb(void *arg, err_t result)
{
    (void)arg;
    if (result != ERR_OK) { LOG_W("subscribe rc=%d", (int)result); }
}

/* Inbound PUBLISH header: note whether it is the time/set topic, reset buffer. */
static void in_pub_cb(void *arg, const char *topic, u32_t tot_len)
{
    (void)arg; (void)tot_len;
    s_in_is_time = (s_topic_time[0] != '\0' && strcmp(topic, s_topic_time) == 0);
    s_in_len = 0;
    s_in_overflow = false;
}

/* Inbound payload chunk(s): reassemble, and on the last chunk hand it over. */
static void in_data_cb(void *arg, const u8_t *data, u16_t len, u8_t flags)
{
    (void)arg;
    if (s_in_is_time) {
        if ((size_t)(s_in_len + len) < sizeof(s_in_buf)) {
            memcpy(s_in_buf + s_in_len, data, len);
            s_in_len = (uint16_t)(s_in_len + len);
        } else {
            s_in_overflow = true;   /* too big — discard rather than parse a truncation */
        }
    }
    if (flags & MQTT_DATA_FLAG_LAST) {
        if (s_in_is_time && s_in_overflow) {
            s_time_oversize = true;            /* transport_task logs this */
        } else if (s_in_is_time && s_in_len < sizeof(s_time_buf)) {
            memcpy(s_time_buf, s_in_buf, s_in_len);
            s_time_len = s_in_len;
            s_time_pending = true;             /* no FreeRTOS call here — task polls it */
        }
        s_in_is_time = false;
        s_in_len = 0;
        s_in_overflow = false;
    }
}

static void connect_cb(mqtt_client_t *c, void *arg, mqtt_connection_status_t st)
{
    (void)arg;
    LOG_I("MQTT CONNACK status=%d %s", st,
          st == MQTT_CONNECT_ACCEPTED ? "ACCEPTED" : "(rejected)");
    s_connected = (st == MQTT_CONNECT_ACCEPTED);
    if (st == MQTT_CONNECT_ACCEPTED) {
        ui_oled_set_diag("MQTT up");
    } else {
        char d[24];
        snprintf(d, sizeof(d), "CONNACK rej %d", (int)st);
        ui_oled_set_diag(d);
    }
    if (s_connected) {
        s_backoff_ms = 1000;       /* reset backoff on success */
        s_pub_token++;             /* retire any publish from a dropped session */
        s_pub_state  = PUB_IDLE;

        /* status=online retained (matches LWT shape). Already in lwIP ctx. */
        if (s_topic_status[0]) {
            (void)mqtt_publish(c, s_topic_status, "online", 6,
                               /*qos=*/1, /*retain=*/1, pub_cb, NULL);
        }
        /* Subscribe to the time-sync downlink and route inbound payloads. */
        mqtt_set_inpub_callback(c, in_pub_cb, in_data_cb, NULL);
        if (s_topic_time[0]) {
            (void)mqtt_subscribe(c, s_topic_time, 1, sub_cb, NULL);
        }
    }
}

static void mqtt_connect_now(void)
{
    LOG_I("mqtt_client_connect -> %s:%u (backoff %lu ms)",
          ipaddr_ntoa(&s_broker_addr), (unsigned)s_broker->port,
          (unsigned long)s_backoff_ms);
    ui_oled_set_diag("mqtt connect..");
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
    char diag[24];

    ui_oled_set_diag("cyw43 init..");
    if (cyw43_arch_init_with_country(CYW43_COUNTRY_NETHERLANDS) != 0) {
        LOG_E("cyw43 init FAILED");
        ui_oled_set_diag("CYW43 INIT FAIL");
        return false;
    }
    cyw43_arch_enable_sta_mode();

    if (s_wifi->ssid[0] == '\0') {
        LOG_E("no SSID in /cfg/wifi.json");
        ui_oled_set_diag("NO SSID CFG");
        return false;
    }

    LOG_I("connecting to SSID '%s' ...", s_wifi->ssid);
    /* Bounded retry: the join legitimately fails on transient conditions
     * (rc=-2 NONET, the AP's beacon not yet seen) — the proven bring-up retries
     * for exactly this reason. rc=-7 (BADAUTH) is terminal (wrong PSK); stop.
     * This whole loop runs with no other cyw43-touching task contending the
     * async-context (the producer tasks are not created until after bring-up;
     * the ui task is network-inert), so the cyw43 join handshake completes. */
    int rc = -1;
    for (unsigned attempt = 1u; attempt <= WIFI_CONNECT_ATTEMPTS; attempt++) {
        snprintf(diag, sizeof(diag), "assoc %.8s %u/%u",
                 s_wifi->ssid, attempt, (unsigned)WIFI_CONNECT_ATTEMPTS);
        ui_oled_set_diag(diag);
        rc = cyw43_arch_wifi_connect_timeout_ms(s_wifi->ssid, s_wifi->psk,
                                                CYW43_AUTH_WPA2_AES_PSK,
                                                WIFI_CONNECT_TIMEOUT_MS);
        if (rc == 0) { break; }
        /* rc=-7 BADAUTH, -8 CONNECT_FAILED (5 GHz/WPA3-only AP), -2 NONET. */
        LOG_W("wifi connect attempt %u/%u FAILED rc=%d",
              attempt, (unsigned)WIFI_CONNECT_ATTEMPTS, rc);
        if (rc == -7) { break; }   /* bad password — retrying cannot help */
        if (attempt < WIFI_CONNECT_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(WIFI_CONNECT_RETRY_DELAY_MS));
        }
    }
    if (rc != 0) {
        LOG_W("wifi connect gave up rc=%d — sensor-only mode", rc);
        snprintf(diag, sizeof(diag), "WIFI FAIL rc=%d", rc);
        ui_oled_set_diag(diag);
        return false;
    }
    const ip4_addr_t *ip = netif_ip4_addr(netif_default);
    LOG_I("CONNECTED — IP = %s", ip ? ip4addr_ntoa(ip) : "(none)");
    gpio_put(BOARD_LED_WIFI_PIN, ip ? 1 : 0);
    ui_oled_set_net(ip ? ip4addr_ntoa(ip) : "----", false);
    ui_oled_set_diag("wifi ok");
    return true;
}

static bool mqtt_setup(void)
{
    LOG_I("building altcp_tls config (mTLS, ECDSA P-256)");
    ui_oled_set_diag("tls cfg..");
    s_tls_cfg = altcp_tls_create_config_client_2wayauth(
        s_id->ca_der,      s_id->ca_len,
        s_id->dev_key_der, s_id->dev_key_len,
        NULL, 0,
        s_id->dev_crt_der, s_id->dev_crt_len);
    if (!s_tls_cfg) {
        LOG_E("altcp_tls config FAILED");
        ui_oled_set_diag("TLS CFG FAIL");
        return false;
    }

    s_mqtt = mqtt_client_new();
    if (!s_mqtt) {
        LOG_E("mqtt_client_new FAILED");
        ui_oled_set_diag("MQTT NEW FAIL");
        return false;
    }

    snprintf(s_topic_status, sizeof(s_topic_status), "rmms/%s/status",   s_id->uuid);
    snprintf(s_topic_env,    sizeof(s_topic_env),    "rmms/%s/env",      s_id->uuid);
    snprintf(s_topic_air,    sizeof(s_topic_air),    "rmms/%s/air",      s_id->uuid);
    snprintf(s_topic_radar,  sizeof(s_topic_radar),  "rmms/%s/radar",    s_id->uuid);
    snprintf(s_topic_light,  sizeof(s_topic_light),  "rmms/%s/light",    s_id->uuid);
    snprintf(s_topic_time,   sizeof(s_topic_time),   "rmms/%s/time/set", s_id->uuid);

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

/* ── Ingest: stamp every sample with wall_ms + seq and persist to the spool ──
 * env/air/light drain their queue fully (no intermediate sample dropped); radar
 * is decimated to ~1 Hz (its producer runs at 10 Hz) to keep the wire cadence
 * and bound flash wear (ADR-0003). The OLED shadow mirrors the latest sample. */

static void ingest_env(void)
{
    EnvMsg m; bool have = false;
    while (xQueueReceive(q_env, &m, 0) == pdTRUE) {
        have = true;
        SpoolRecord r;
        spool_make_env(&r, &m.v, m.q, time_us_64(), wall_now_ms(), s_seq_env++);
        (void)spool_push(&r);
    }
    if (have) {
        ui_oled_set_env(m.v.temp_c, m.v.humidity_pct, m.v.pressure_hpa,
                        m.v.pressure_valid, m.q);
    }
}

static void ingest_air(void)
{
    AirMsg m; bool have = false;
    while (xQueueReceive(q_air, &m, 0) == pdTRUE) {
        have = true;
        SpoolRecord r;
        spool_make_air(&r, &m.v, m.q, time_us_64(), wall_now_ms(), s_seq_air++);
        (void)spool_push(&r);
    }
    if (have) { ui_oled_set_air(m.v.co2_ppm, m.v.tvoc_ppb, m.v.aqi, m.q); }
}

static void ingest_light(void)
{
    LightMsg m; bool have = false;
    while (xQueueReceive(q_light, &m, 0) == pdTRUE) {
        have = true;
        SpoolRecord r;
        spool_make_light(&r, &m.v, m.q, time_us_64(), wall_now_ms(), s_seq_light++);
        (void)spool_push(&r);
    }
    if (have) { ui_oled_set_light(m.v.lux, m.q); }
}

static void ingest_radar(void)
{
    RadarSample m; bool have = false;
    while (xQueueReceive(q_radar, &m, 0) == pdTRUE) { have = true; }   /* drain all */
    if (!have) { return; }

    uint64_t now = time_us_64();
    if (now - s_last_radar_us >= ((uint64_t)RADAR_SPOOL_INTERVAL_MS * 1000u)) {
        s_last_radar_us = now;
        SpoolRecord r;
        spool_make_radar(&r, &m, m.q, now, wall_now_ms(), s_seq_radar++);
        (void)spool_push(&r);
    }
    ui_oled_set_radar(m.presence, m.distance_mm, m.breath_rpm, m.heart_bpm, m.q);
}

static void ingest_all(void)
{
    ingest_env();
    ingest_air();
    ingest_light();
    ingest_radar();
    seq_persist_maybe();
}

/* ── Encode a stored record back to its §9.2 JSON wire form ──────────────── */

static int encode_record(const SpoolRecord *r, char *buf, size_t cap)
{
    switch (r->kind) {
    case SPOOL_KIND_ENV: {
        JsonEnvBody b = { .temp_c = r->body.env.temp_c, .hum_pct = r->body.env.humidity_pct,
                          .pres_hpa = r->body.env.pressure_hpa,
                          .pres_valid = r->body.env.pressure_valid };
        return json_encode_env(buf, cap, r->ts_us, r->wall_ms, r->seq, r->q, &b);
    }
    case SPOOL_KIND_AIR: {
        JsonAirBody b = { .co2_ppm = r->body.air.co2_ppm, .tvoc_ppb = r->body.air.tvoc_ppb,
                          .aqi = r->body.air.aqi };
        return json_encode_air(buf, cap, r->ts_us, r->wall_ms, r->seq, r->q, &b);
    }
    case SPOOL_KIND_RADAR: {
        /* Driver sentinels → §9.2.2 nulls: distance 0 / rates ≤ 0 = "not measured". */
        JsonRadarBody b = {
            .presence    = r->body.radar.presence,
            .distance_mm = (r->body.radar.distance_mm == 0u) ? -1 : (int)r->body.radar.distance_mm,
            .breath_bpm  = (r->body.radar.breath_rpm > 0.0f) ? r->body.radar.breath_rpm : -1.0f,
            .heart_bpm   = (r->body.radar.heart_bpm  > 0.0f) ? r->body.radar.heart_bpm  : -1.0f,
        };
        return json_encode_radar(buf, cap, r->ts_us, r->wall_ms, r->seq, r->q, &b);
    }
    case SPOOL_KIND_LIGHT: {
        JsonLightBody b = { .lux = r->body.light.lux };
        return json_encode_light(buf, cap, r->ts_us, r->wall_ms, r->seq, r->q, &b);
    }
    default:
        return -1;
    }
}

static const char *topic_for_kind(uint8_t kind)
{
    switch (kind) {
    case SPOOL_KIND_ENV:   return s_topic_env;
    case SPOOL_KIND_AIR:   return s_topic_air;
    case SPOOL_KIND_RADAR: return s_topic_radar;
    case SPOOL_KIND_LIGHT: return s_topic_light;
    default:               return NULL;
    }
}

/* ── Drain one record from the spool head (in-flight = 1) ────────────────────
 * Non-blocking state machine, one publish outstanding at a time:
 *   IDLE  → publish the head, go PENDING
 *   ACKED → clear the head (spool_ack), back to IDLE
 *   FAILED/timeout → back to IDLE (head stays → retried; QoS-1 at-least-once)
 * A record is removed only on its PUBACK, so an outage/crash never loses it. */
static void drain_step(void)
{
    if (s_pub_state == PUB_ACKED) {
        (void)spool_ack(s_pending_ws);
        s_pub_state = PUB_IDLE;
    } else if (s_pub_state == PUB_FAILED) {
        s_pub_state = PUB_IDLE;
    } else if (s_pub_state == PUB_PENDING) {
        if (xTaskGetTickCount() >= s_pub_deadline) {
            /* No PUBACK in time. Retire this publish (a late ack is now ignored)
             * and DROP the session: lwIP may still have the original publish
             * pending (its MQTT_REQ_TIMEOUT is 30 s), and re-publishing into a
             * live request risks a duplicate/aliased ack. The head record is
             * un-acked in the spool, so it is re-sent fresh after reconnect. */
            LOG_W("PUBACK timeout — dropping session to clear stale publish");
            s_pub_token++;
            s_pub_state = PUB_IDLE;
            cyw43_arch_lwip_begin();
            mqtt_disconnect(s_mqtt);
            cyw43_arch_lwip_end();
            s_connected = false;   /* housekeeping reconnects */
        }
        return;   /* still waiting on the in-flight publish */
    }

    if (!s_connected || s_pub_state != PUB_IDLE) { return; }

    SpoolRecord r;
    uint64_t ws;
    if (spool_peek(&r, &ws) != ERR_OK) { return; }   /* spool empty */

    char buf[256];
    int n = encode_record(&r, buf, sizeof(buf));
    if (n <= 0 || (size_t)n >= sizeof(buf)) {
        LOG_W("encode failed kind=%u seq=%u — discarding", r.kind, r.seq);
        (void)spool_ack(ws);   /* cannot transmit garbage; drop just this record */
        return;
    }
    const char *topic = topic_for_kind(r.kind);
    if (!topic || topic[0] == '\0') { (void)spool_ack(ws); return; }

    /* Take a fresh token BEFORE marking PENDING/publishing, so any prior
     * in-flight callback (older token) can no longer match. */
    uint32_t tok = ++s_pub_token;
    s_pending_ws   = ws;
    s_pub_state    = PUB_PENDING;
    s_pub_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(PUB_ACK_TIMEOUT_MS);

    cyw43_arch_lwip_begin();
    err_t e = mqtt_publish(s_mqtt, topic, buf, (u16_t)n,
                           /*qos=*/1, /*retain=*/0, pub_cb_spool,
                           (void *)(uintptr_t)tok);
    cyw43_arch_lwip_end();
    if (e != ERR_OK) {
        LOG_W("mqtt_publish(%s) rc=%d", topic, (int)e);
        s_pub_state = PUB_IDLE;   /* retry on the next pump */
    }
}

/* ── Process inbound control (time/set) ──────────────────────────────────── */

static void process_ctrl(void)
{
    if (s_time_oversize) {
        s_time_oversize = false;
        LOG_W("time/set payload too large (>%u B) — discarded",
              (unsigned)(sizeof(s_in_buf) - 1u));
    }
    if (!s_time_pending) { return; }   /* fast path; also avoids the lock pre-Wi-Fi */

    char     buf[CTRL_PAYLOAD_MAX];
    uint16_t len;
    cyw43_arch_lwip_begin();           /* excludes the inbound callback in both archs */
    len = s_time_len;
    if (len >= sizeof(buf)) { len = (uint16_t)(sizeof(buf) - 1u); }
    memcpy(buf, s_time_buf, len);
    s_time_pending = false;
    cyw43_arch_lwip_end();

    int64_t epoch_ms;
    if (json_parse_time_set(buf, len, &epoch_ms) == ERR_OK && epoch_ms > 0) {
        s_wall_offset_ms = epoch_ms - (int64_t)(time_us_64() / 1000u);
        s_wall_valid = true;
        LOG_I("RTC synced via time/set: epoch_ms=%lld", (long long)epoch_ms);
    } else {
        LOG_W("bad time/set payload");
    }
}

/* ── transport_mqtt_init ─────────────────────────────────────────────────── */

err_t transport_mqtt_init(const Identity *id, const CfgWifi *wifi,
                          const CfgBroker *broker,
                          void (*start_sensors)(void))
{
    if (!id || !wifi || !broker) { return ERR_INVALID_ARG; }
    s_id = id; s_wifi = wifi; s_broker = broker;
    s_start_sensors = start_sensors;   /* may be NULL */
    return ERR_OK;
}

/* ── transport_task ──────────────────────────────────────────────────────── */

void transport_task(void *arg)
{
    (void)arg;

    /* Initialise USB-serial stdio HERE — after vTaskStartScheduler, before cyw43
     * init — NOT in board_hw_init().  Under the SDK's pico_async_context_freertos
     * / pico_time_adapter integration, stdio_init_all() called pre-scheduler
     * breaks the time integration the cyw43 threadsafe_background poll relies on,
     * so the Wi-Fi association never completes (and pico_stdio_usb fails to
     * enumerate — the "semaphore timeout" COM port).  Doing it here, as the
     * highest-priority task's first act, matches the proven bring-up
     * (test/bringup/wifi_connect.c §stdio ordering).  No blocking wait for a host
     * to attach: a deployed device must boot without a console. */
    stdio_init_all();
    vTaskDelay(pdMS_TO_TICKS(500));   /* let a connected host finish CDC line-coding */

    /* NOTE: seq_load() is intentionally NOT called here — it does a flash write
     * (seq checkpoint via flash_safe_execute), and under FreeRTOS SMP that runs
     * a Core-1 lockout + disables interrupts on Core 0.  Doing that immediately
     * before cyw43_arch_init wedges the cyw43 association (the "assoc N/4" hang).
     * It is deferred to just after bring-up, below — it only needs to complete
     * before the first sample is stamped, which happens after the producers
     * start.  The proven bring-up does no such pre-connect flash write. */

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
            ui_oled_set_diag("broker resolve?");
        }
    } else {
        LOG_W("no Wi-Fi — sensors still spooled to flash; publishing on reconnect");
    }

    /* Now that cyw43 association is done, the pre-connect flash-write hazard is
     * past: resume the per-topic seq counters (this writes the checkpoint via
     * flash_safe_execute) BEFORE any producer starts stamping samples. */
    seq_load();

    /* Bring-up is done (success or failure): the cyw43 association no longer
     * needs an uncontended async-context, so it is now safe to start the
     * producer/UI tasks.  UNCONDITIONAL — on the Wi-Fi-failure path the sensors
     * must still run and spool offline (publishing resumes on reconnect). */
    if (s_start_sensors) { s_start_sensors(); }

    TickType_t next_attempt = 0;
    TickType_t last_house   = 0;

    for (;;) {
        wdt_task_alive(WDT_TASK_TRANSPORT);

        /* Always ingest (≈10 Hz pump): persist every sample to the spool even
         * with no link, so an outage loses nothing. Keeps producer queues
         * drained well within their depth. */
        ingest_all();

        /* Housekeeping at the slower cadence: link health, reconnect, time-sync,
         * OLED. */
        TickType_t now = xTaskGetTickCount();
        if (now - last_house >= pdMS_TO_TICKS(TRANSPORT_LOOP_MS)) {
            last_house = now;

            /* Some drops (broker hard-kill, blip) don't fire the connect cb. */
            if (s_connected && !mqtt_client_is_connected(s_mqtt)) {
                LOG_W("mqtt link went down — marking offline");
                s_connected = false;
                s_pub_token++;          /* retire the in-flight publish */
                s_pub_state = PUB_IDLE;
            }

            /* Reconnect with backoff; re-resolve each attempt (covers tablet/mDNS
             * coming back, or the broker IP moving). */
            if (s_wifi_ok && s_mqtt && !s_connected && now >= next_attempt) {
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

            process_ctrl();

            ui_oled_set_net(s_wifi_ok ? ip4addr_ntoa(netif_ip4_addr(netif_default))
                                      : "----",
                            s_connected);
        }

        /* Drain the spool toward the broker (one in-flight publish). */
        drain_step();

        /* Pump faster while a backlog is draining so the next record publishes
         * promptly after each PUBACK (in-flight=1 is round-trip-bound; the short
         * pump just removes the idle gap between an ack and the next publish);
         * idle at ~10 Hz when the spool is empty. */
        vTaskDelay(pdMS_TO_TICKS((s_connected && spool_count() > 0u) ? 20u : 100u));
    }
}
