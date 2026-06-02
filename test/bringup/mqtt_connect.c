/*
 * Bring-up step 6 (CLAUDE.md §15 steps 14 + 16): mTLS to broker + MQTT CONNECT
 * + test publish. Wi-Fi-only (USB tablet link still deferred per §2.1).
 *
 * Path:
 *   bake_certs.py @ CMake-time → baked_certs.h (CA, cert, key, UUID)
 *   wifi_task: stdio → cyw43_arch_init → connect_to_AP → DHCP → IP
 *   mqtt_task: build altcp_tls config from baked certs → mqtt_client_connect
 *              → on CONNACK, publish "hello" to rmms/<uuid>/log every 10 s
 *
 * This is the FAST bring-up: certs are compiled in. The real firmware reads
 * /certs/* from littlefs (uploaded via the deferred bringup_provision flow).
 * Same security stance as the WiFi creds in bringup_wifi: env-var at build
 * time, build/ is gitignored.
 */
#include "FreeRTOS.h"
#include "task.h"

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/altcp_tls.h"
#include "lwip/apps/mqtt.h"

#include "baked_certs.h"   /* generated header: rmms_ca_der / dev_crt / dev_key + RMMS_DEVICE_UUID */

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

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PSK
#define WIFI_PSK ""
#endif
#ifndef BROKER_IP
#define BROKER_IP "192.168.2.21"
#endif
#ifndef BROKER_PORT
#define BROKER_PORT 8883
#endif

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
    char topic[80];
    char payload[64];
    int tn = snprintf(topic, sizeof(topic), "rmms/%s/log", RMMS_DEVICE_UUID);
    int pn = snprintf(payload, sizeof(payload), "hello %lu", (unsigned long)g_mqtt_pub_count);
    if (tn <= 0 || pn <= 0) return ERR_VAL;

    cyw43_arch_lwip_begin();
    err_t e = mqtt_publish(g_mqtt, topic, payload, (u16_t)pn,
                           /*qos=*/1, /*retain=*/0, mqtt_pub_cb, NULL);
    cyw43_arch_lwip_end();
    return e;
}

static void wifi_task(void *arg)
{
    (void)arg;

    stdio_init_all();
    while (!stdio_usb_connected()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelay(pdMS_TO_TICKS(500));
    printf("\n[bringup] mqtt_task: host connected (core=%u)\n", (unsigned)get_core_num());
    printf("[bringup] UUID = %s\n", RMMS_DEVICE_UUID);
    printf("[bringup] broker = %s:%d\n", BROKER_IP, (int)BROKER_PORT);
    printf("[bringup] baked: ca=%u dev_crt=%u dev_key=%u bytes\n",
           rmms_ca_der_len, rmms_dev_crt_len, rmms_dev_key_len);

    if (WIFI_SSID[0] == '\0') {
        for (;;) {
            printf("[bringup] NO WIFI CREDENTIALS — re-build with WIFI_SSID/WIFI_PSK set\n");
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }

    int rc = cyw43_arch_init_with_country(CYW43_COUNTRY_NETHERLANDS);
    if (rc != 0) {
        for (;;) {
            printf("[bringup] cyw43_arch_init FAILED rc=%d\n", rc);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
    cyw43_arch_enable_sta_mode();
    printf("[bringup] connecting to SSID '%s' ...\n", WIFI_SSID);
    rc = cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PSK,
                                            CYW43_AUTH_WPA2_AES_PSK, 30000);
    if (rc != 0) {
        for (;;) {
            printf("[bringup] wifi connect FAILED rc=%d\n", rc);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
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
        int prc = mbedtls_x509_crt_parse(&probe_crt, rmms_ca_der, rmms_ca_der_len);
        printf("[bringup] probe: mbedtls_x509_crt_parse(CA)  rc=%d (-0x%04x)\n", prc, (unsigned)-prc);
        prc = mbedtls_x509_crt_parse(&probe_crt, rmms_dev_crt, rmms_dev_crt_len);
        printf("[bringup] probe: mbedtls_x509_crt_parse(crt) rc=%d (-0x%04x)\n", prc, (unsigned)-prc);
        mbedtls_x509_crt_free(&probe_crt);

        mbedtls_pk_context probe_pk;
        mbedtls_pk_init(&probe_pk);
        prc = mbedtls_pk_parse_key(&probe_pk, rmms_dev_key, rmms_dev_key_len, NULL, 0, NULL, NULL);
        printf("[bringup] probe: mbedtls_pk_parse_key        rc=%d (-0x%04x)\n", prc, (unsigned)-prc);
        mbedtls_pk_free(&probe_pk);
    }

    printf("[bringup] building altcp_tls config (mTLS, ECDSA P-256)\n");
    struct altcp_tls_config *tls_cfg = altcp_tls_create_config_client_2wayauth(
        rmms_ca_der,  rmms_ca_der_len,
        rmms_dev_key, rmms_dev_key_len,
        NULL, 0,                          /* private key password */
        rmms_dev_crt, rmms_dev_crt_len);
    if (!tls_cfg) {
        for (;;) {
            printf("[bringup] altcp_tls_create_config_client_2wayauth FAILED\n");
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }
    printf("[bringup] altcp_tls config ok\n");

    g_mqtt = mqtt_client_new();
    if (!g_mqtt) {
        for (;;) {
            printf("[bringup] mqtt_client_new FAILED\n");
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }

    struct mqtt_connect_client_info_t ci;
    memset(&ci, 0, sizeof(ci));
    ci.client_id  = RMMS_DEVICE_UUID;     /* must match cert CN per CLAUDE.md §9.4 */
    ci.keep_alive = 60;
    ci.tls_config = tls_cfg;

    ip_addr_t broker_addr;
    if (!ipaddr_aton(BROKER_IP, &broker_addr)) {
        for (;;) {
            printf("[bringup] BROKER_IP '%s' not parseable\n", BROKER_IP);
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }
    printf("[bringup] mqtt_client_connect → %s:%d (TLS + MQTT CONNECT)\n",
           BROKER_IP, (int)BROKER_PORT);
    cyw43_arch_lwip_begin();
    err_t e = mqtt_client_connect(g_mqtt, &broker_addr, BROKER_PORT,
                                  mqtt_connect_cb, NULL, &ci);
    cyw43_arch_lwip_end();
    if (e != ERR_OK) {
        for (;;) {
            printf("[bringup] mqtt_client_connect immediate err=%d\n", (int)e);
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }
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
