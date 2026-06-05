/*
 * Bring-up step 6 (CLAUDE.md §15 steps 14 + 16): mTLS to broker + MQTT CONNECT
 * + test publish. Wi-Fi-only (USB tablet link still deferred per §2.1).
 *
 * Path:
 *   storage_mount() → identity_load() + cfg_load_wifi/broker() from littlefs
 *   wifi_task: stdio → cyw43_arch_init → connect_to_AP → DHCP → IP
 *   mqtt_task: build altcp_tls config from /certs/* → mqtt_client_connect
 *              → on CONNACK, publish "hello" to rmms/<uuid>/log every 10 s
 *
 * Identity (UUID + cert chain) and per-deployment config (Wi-Fi creds,
 * broker address) live in littlefs — provisioned once with bringup_provision
 * + scripts/provision_device.py. Reflashing firmware never touches them.
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
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"

#include <stdio.h>
#include <string.h>

/* Platform implementation required by MBEDTLS_PLATFORM_MS_TIME_ALT. Returns
 * monotonic ms since boot — no wall clock until we wire up time/set (§16 Q6),
 * but session expiry timing only needs a monotonic source. */
mbedtls_ms_time_t mbedtls_ms_time(void)
{
    return (mbedtls_ms_time_t)(time_us_64() / 1000u);
}

/* mbedtls_hardware_poll is provided by pico-sdk's pico_mbedtls.c (linked via
 * the pico_mbedtls library), backed by get_rand_64(). Defining
 * MBEDTLS_ENTROPY_HARDWARE_ALT in mbedtls_config.h is enough to wire it. */

/* FreeRTOSConfig.h required hooks. */
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

/* MQTT state. lwIP MQTT callbacks fire on the lwIP/cyw43 background context
 * (threadsafe_background). We keep state in volatiles and only print from
 * callbacks — debug-only, not safe-for-prod logging. */
static mqtt_client_t *g_mqtt;
static volatile bool g_mqtt_connected = false;
static volatile uint32_t g_mqtt_pub_count = 0;

/* Identity + per-deployment config loaded from littlefs at task start. */
static Identity   g_id;
static CfgWifi    g_wifi;
static CfgBroker  g_broker;
static char       g_topic_log[80];

static const char *mqtt_status_str(mqtt_connection_status_t s)
{
    switch (s) {
    case MQTT_CONNECT_ACCEPTED:                 return "ACCEPTED";
    case MQTT_CONNECT_REFUSED_PROTOCOL_VERSION: return "REFUSED_PROTOCOL_VERSION";
    case MQTT_CONNECT_REFUSED_IDENTIFIER:       return "REFUSED_IDENTIFIER";
    case MQTT_CONNECT_REFUSED_SERVER:           return "REFUSED_SERVER";
    case MQTT_CONNECT_REFUSED_USERNAME_PASS:    return "REFUSED_USERNAME_PASS";
    case MQTT_CONNECT_REFUSED_NOT_AUTHORIZED_:  return "REFUSED_NOT_AUTHORIZED";
    case MQTT_CONNECT_DISCONNECTED:             return "DISCONNECTED";
    case MQTT_CONNECT_TIMEOUT:                  return "TIMEOUT";
    default:                                    return "UNKNOWN";
    }
}

static void mqtt_pub_cb(void *arg, err_t result)
{
    (void)arg;
    printf("[bringup] publish result=%d\n", (int)result);
}

static void mqtt_connect_cb(mqtt_client_t *c, void *arg, mqtt_connection_status_t status)
{
    (void)c; (void)arg;
    printf("[bringup] MQTT connect callback: %s (%d)\n", mqtt_status_str(status), status);
    if (status == MQTT_CONNECT_ACCEPTED) {
        g_mqtt_connected = true;
    } else {
        g_mqtt_connected = false;
    }
}

static err_t mqtt_publish_hello(void)
{
    char payload[64];
    int pn = snprintf(payload, sizeof(payload), "hello %lu",
                      (unsigned long)g_mqtt_pub_count);
    if (pn <= 0) return ERR_VAL;

    cyw43_arch_lwip_begin();
    err_t e = mqtt_publish(g_mqtt, g_topic_log, payload, (u16_t)pn,
                           /*qos=*/1, /*retain=*/0, mqtt_pub_cb, NULL);
    cyw43_arch_lwip_end();
    return e;
}

#define BLOCK_FATAL(fmt, ...) \
    for (;;) { printf("[bringup] " fmt "\n", ##__VA_ARGS__); \
               vTaskDelay(pdMS_TO_TICKS(3000)); }

static void wifi_task(void *arg)
{
    (void)arg;

    stdio_init_all();
    while (!stdio_usb_connected()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelay(pdMS_TO_TICKS(500));
    printf("\n[bringup] mqtt_task: host connected (core=%u)\n", (unsigned)get_core_num());

    /* Load identity + config from littlefs (factory-provisioned via
     * bringup_provision + scripts/provision_device.py). */
    if (storage_mount() != ERR_OK)
        BLOCK_FATAL("storage_mount failed — was the device provisioned?");
    if (identity_load(&g_id) != ERR_OK)
        BLOCK_FATAL("identity_load failed — missing /certs or /cfg/device.json");
    if (cfg_load_wifi(&g_wifi) != ERR_OK)
        BLOCK_FATAL("cfg_load_wifi failed — missing /cfg/wifi.json");
    if (cfg_load_broker(&g_broker) != ERR_OK)
        BLOCK_FATAL("cfg_load_broker failed — missing /cfg/broker.json");

    snprintf(g_topic_log, sizeof(g_topic_log), "rmms/%s/log", g_id.uuid);
    printf("[bringup] UUID = %s\n", g_id.uuid);
    printf("[bringup] broker = %s:%u (host='%s')\n",
           g_broker.ip[0] ? g_broker.ip : g_broker.host,
           (unsigned)g_broker.port, g_broker.host);
    printf("[bringup] certs: ca=%u dev_crt=%u dev_key=%u bytes\n",
           (unsigned)g_id.ca_len, (unsigned)g_id.dev_crt_len,
           (unsigned)g_id.dev_key_len);

    int rc = cyw43_arch_init_with_country(CYW43_COUNTRY_NETHERLANDS);
    if (rc != 0) BLOCK_FATAL("cyw43_arch_init FAILED rc=%d", rc);
    cyw43_arch_enable_sta_mode();
    printf("[bringup] connecting to SSID '%s' ...\n", g_wifi.ssid);
    rc = cyw43_arch_wifi_connect_timeout_ms(g_wifi.ssid, g_wifi.psk,
                                            CYW43_AUTH_WPA2_AES_PSK, 30000);
    if (rc != 0) BLOCK_FATAL("wifi connect FAILED rc=%d", rc);
    const ip4_addr_t *ip = netif_ip4_addr(netif_default);
    printf("[bringup] CONNECTED — IP = %s\n", ip ? ip4addr_ntoa(ip) : "(none)");

    /* ── TLS config (mTLS, two-way) ─────────────────────────────────────────
     * lwIP/altcp_tls passes these buffers to mbedtls_x509_crt_parse and
     * mbedtls_pk_parse_key. The SDK swallows the mbedtls error code (only
     * logs it under ALTCP_MBEDTLS_DEBUG). Probe the same parsers directly
     * first so a failure surfaces with a concrete return code.
     */
    {
        mbedtls_x509_crt probe_crt;
        mbedtls_x509_crt_init(&probe_crt);
        int prc = mbedtls_x509_crt_parse(&probe_crt, g_id.ca_der, g_id.ca_len);
        printf("[bringup] probe: mbedtls_x509_crt_parse(CA)  rc=%d (-0x%04x)\n", prc, (unsigned)-prc);
        prc = mbedtls_x509_crt_parse(&probe_crt, g_id.dev_crt_der, g_id.dev_crt_len);
        printf("[bringup] probe: mbedtls_x509_crt_parse(crt) rc=%d (-0x%04x)\n", prc, (unsigned)-prc);
        mbedtls_x509_crt_free(&probe_crt);

        mbedtls_pk_context probe_pk;
        mbedtls_pk_init(&probe_pk);
        prc = mbedtls_pk_parse_key(&probe_pk, g_id.dev_key_der, g_id.dev_key_len,
                                   NULL, 0, NULL, NULL);
        printf("[bringup] probe: mbedtls_pk_parse_key        rc=%d (-0x%04x)\n", prc, (unsigned)-prc);
        mbedtls_pk_free(&probe_pk);
    }

    printf("[bringup] building altcp_tls config (mTLS, ECDSA P-256)\n");
    struct altcp_tls_config *tls_cfg = altcp_tls_create_config_client_2wayauth(
        g_id.ca_der,      g_id.ca_len,
        g_id.dev_key_der, g_id.dev_key_len,
        NULL, 0,                                  /* private key password */
        g_id.dev_crt_der, g_id.dev_crt_len);
    if (!tls_cfg) BLOCK_FATAL("altcp_tls_create_config_client_2wayauth FAILED");
    printf("[bringup] altcp_tls config ok\n");

    g_mqtt = mqtt_client_new();
    if (!g_mqtt) BLOCK_FATAL("mqtt_client_new FAILED");

    struct mqtt_connect_client_info_t ci;
    memset(&ci, 0, sizeof(ci));
    ci.client_id  = g_id.uuid;        /* must match cert CN per CLAUDE.md §9.4 */
    ci.keep_alive = 60;
    ci.tls_config = tls_cfg;

    /* Use literal IP when present (no DNS); host-only path is left for the
     * production firmware that wires up mDNS resolution per §8.2. */
    const char *broker_addr_str = g_broker.ip[0] ? g_broker.ip : g_broker.host;
    ip_addr_t broker_addr;
    if (!ipaddr_aton(broker_addr_str, &broker_addr))
        BLOCK_FATAL("broker addr '%s' not an IP literal — mDNS NYI in bringup",
                    broker_addr_str);
    printf("[bringup] mqtt_client_connect → %s:%u (TLS + MQTT CONNECT)\n",
           broker_addr_str, (unsigned)g_broker.port);
    cyw43_arch_lwip_begin();
    err_t e = mqtt_client_connect(g_mqtt, &broker_addr, g_broker.port,
                                  mqtt_connect_cb, NULL, &ci);
    cyw43_arch_lwip_end();
    if (e != ERR_OK) BLOCK_FATAL("mqtt_client_connect immediate err=%d", (int)e);
    printf("[bringup] mqtt_client_connect issued, waiting for CONNACK ...\n");

    /* ── Steady-state heartbeat ─────────────────────────────────────────────
     * Once CONNACK arrives via mqtt_connect_cb, publish "hello N" every 10 s.
     * Watch `mosquitto -v` on the tablet for the inbound publishes.
     */
    for (;;) {
        if (g_mqtt_connected) {
            err_t pe = mqtt_publish_hello();
            printf("[bringup] published 'hello %lu' (rc=%d)\n",
                   (unsigned long)g_mqtt_pub_count, (int)pe);
            g_mqtt_pub_count++;
        } else {
            printf("[bringup] waiting for MQTT CONNACK (connected=%d)\n", g_mqtt_connected);
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

int main(void)
{
    /* Pin to core 0 — same rationale as bringup_wifi. */
    xTaskCreateAffinitySet(wifi_task, "mqtt", 8192, NULL, 2, 0x01, NULL);
    vTaskStartScheduler();
    for (;;) { tight_loop_contents(); }
}
