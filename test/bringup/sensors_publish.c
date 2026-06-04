/*
 * Bring-up — sensor reads + MQTT publish over the proven mTLS path.
 *
 * This is the supervisor-demo target: extends bringup_mqtt with real sensor
 * data. Each sensor lives behind a simple #ifdef so we can flip them on/off
 * as drivers come online. Initial commit: BME280 on I²C0 → rmms/<uuid>/env.
 * Next iterations add ENS160 → /air and MR60BHA2 → /radar.
 *
 *   bake_certs.py @ CMake-time → baked_certs.h (CA, cert, key, UUID)
 *   one task: wait stdio → cyw43 init → wifi → mTLS → MQTT CONNECT
 *           → 1 Hz loop: read each enabled sensor, publish its topic
 */
#include "FreeRTOS.h"
#include "task.h"

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/altcp_tls.h"
#include "lwip/apps/mqtt.h"

#include "baked_certs.h"

#include "mbedtls/platform_time.h"

#include "board_pico2wh.h"
#include "bme280.h"
#include "err.h"

#include "hardware/i2c.h"
#include "hardware/gpio.h"

#include <stdio.h>
#include <string.h>

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PSK
#define WIFI_PSK ""
#endif
#ifndef BROKER_IP
#define BROKER_IP "192.168.2.24"
#endif
#ifndef BROKER_PORT
#define BROKER_PORT 8883
#endif

/* Sensor-enable switches — flip to 0 to disable a sensor at compile time
 * (e.g. while bringing up another one). Default: all on. */
#ifndef SENSOR_ENV_ON
#define SENSOR_ENV_ON   1
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

static void mqtt_connect_cb(mqtt_client_t *c, void *arg, mqtt_connection_status_t status)
{
    (void)c; (void)arg;
    printf("[bringup] MQTT CONNACK status=%d %s\n", status,
           status == MQTT_CONNECT_ACCEPTED ? "ACCEPTED" : "(not accepted)");
    g_mqtt_connected = (status == MQTT_CONNECT_ACCEPTED);
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

    char topic[80];
    char payload[256];
    int tn = snprintf(topic, sizeof(topic), "rmms/%s/env", RMMS_DEVICE_UUID);
    int pn = env_sample_encode(payload, sizeof(payload),
                               time_us_64(), -1,    /* wall_ms unknown */
                               ++s_env_seq, q,
                               s.temp_c, s.humidity_pct, s.pressure_hpa);
    if (tn <= 0 || pn < 0) {
        printf("[bringup] env encode overflow\n");
        return;
    }

    cyw43_arch_lwip_begin();
    err_t pe = mqtt_publish(g_mqtt, topic, payload, (u16_t)pn,
                            /*qos=*/1, /*retain=*/0, mqtt_pub_cb, NULL);
    cyw43_arch_lwip_end();
    if (pe != ERR_OK) {
        printf("[bringup] env mqtt_publish immediate rc=%d\n", (int)pe);
    } else if (q == 0) {
        printf("[bringup] env T=%.2fC H=%.2f%% P=%.2fhPa seq=%u\n",
               (double)s.temp_c, (double)s.humidity_pct, (double)s.pressure_hpa,
               (unsigned)s_env_seq);
    }
}
#endif /* SENSOR_ENV_ON */

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
    struct altcp_tls_config *tls_cfg = altcp_tls_create_config_client_2wayauth(
        rmms_ca_der,  rmms_ca_der_len,
        rmms_dev_key, rmms_dev_key_len,
        NULL, 0,
        rmms_dev_crt, rmms_dev_crt_len);
    if (!tls_cfg) {
        for (;;) { printf("[bringup] altcp_tls FAILED\n"); vTaskDelay(pdMS_TO_TICKS(3000)); }
    }

    g_mqtt = mqtt_client_new();
    if (!g_mqtt) {
        for (;;) { printf("[bringup] mqtt_client_new FAILED\n"); vTaskDelay(pdMS_TO_TICKS(3000)); }
    }

    struct mqtt_connect_client_info_t ci;
    memset(&ci, 0, sizeof(ci));
    ci.client_id  = RMMS_DEVICE_UUID;
    ci.keep_alive = 60;
    ci.tls_config = tls_cfg;

    ip_addr_t broker_addr;
    if (!ipaddr_aton(BROKER_IP, &broker_addr)) {
        for (;;) { printf("[bringup] bad BROKER_IP\n"); vTaskDelay(pdMS_TO_TICKS(3000)); }
    }
    printf("[bringup] mqtt_client_connect → %s:%d\n", BROKER_IP, (int)BROKER_PORT);
    cyw43_arch_lwip_begin();
    err_t e = mqtt_client_connect(g_mqtt, &broker_addr, BROKER_PORT,
                                  mqtt_connect_cb, NULL, &ci);
    cyw43_arch_lwip_end();
    if (e != ERR_OK) {
        printf("[bringup] mqtt_client_connect immediate rc=%d\n", (int)e);
    }
}

/* ── Main task ──────────────────────────────────────────────────────────── */

static void sensors_task(void *arg)
{
    (void)arg;

    stdio_init_all();
    while (!stdio_usb_connected()) vTaskDelay(pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(500));

    printf("\n[bringup] sensors_task on core=%u\n", (unsigned)get_core_num());
    printf("[bringup] UUID = %s\n", RMMS_DEVICE_UUID);
    printf("[bringup] broker = %s:%d\n", BROKER_IP, (int)BROKER_PORT);

    /* ── I²C + sensor probes (independent of network) ────────────────── */
    i2c_bus_init();
    i2c_scan();

#if SENSOR_ENV_ON
    err_t e = bme280_init(&s_bme, BOARD_I2C_INST, BOARD_BME280_ADDR);
    s_bme_ok = (e == ERR_OK);
    printf("[bringup] BME280 init %s (rc=%d)\n", s_bme_ok ? "OK" : "FAILED", (int)e);
#endif

    /* ── Wi-Fi + mTLS + MQTT ─────────────────────────────────────────── */
    if (WIFI_SSID[0] == '\0') {
        for (;;) {
            printf("[bringup] NO WIFI CREDENTIALS COMPILED IN\n");
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }

    if (cyw43_arch_init_with_country(CYW43_COUNTRY_NETHERLANDS) != 0) {
        for (;;) { printf("[bringup] cyw43 init FAILED\n"); vTaskDelay(pdMS_TO_TICKS(3000)); }
    }
    cyw43_arch_enable_sta_mode();
    printf("[bringup] connecting to SSID '%s' ...\n", WIFI_SSID);
    int rc = cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PSK,
                                                CYW43_AUTH_WPA2_AES_PSK, 30000);
    if (rc != 0) {
        for (;;) { printf("[bringup] wifi connect FAILED rc=%d\n", rc); vTaskDelay(pdMS_TO_TICKS(3000)); }
    }
    const ip4_addr_t *ip = netif_ip4_addr(netif_default);
    printf("[bringup] CONNECTED — IP = %s\n", ip ? ip4addr_ntoa(ip) : "(none)");

    mqtt_setup_and_connect();

    /* ── 1 Hz publish loop ───────────────────────────────────────────── */
    for (uint32_t tick = 0; ; tick++) {
        if (!g_mqtt_connected) {
            if ((tick % 5) == 0) printf("[bringup] waiting for MQTT CONNACK ...\n");
        } else {
#if SENSOR_ENV_ON
            publish_env_sample();
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
    vTaskStartScheduler();
    for (;;) { tight_loop_contents(); }
}
