#define LOG_TAG "WIFI"
#include "transport_wifi.h"

#include "log.h"
#include "err.h"
#include "storage.h"

#include "pico/cyw43_arch.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* Maximum SSID / PSK sizes from 802.11 spec. */
#define WIFI_SSID_MAX  33u   /* 32 chars + NUL */
#define WIFI_PSK_MAX   65u   /* 64 chars + NUL */

/* Backoff parameters (seconds). */
#define BACKOFF_INIT_S     1u
#define BACKOFF_MAX_S      32u

/* Size of the config scratch buffer for reading /cfg/wifi.json.
 * Stores "ssid\0psk\0" as two consecutive null-terminated strings for now;
 * full JSON parsing added once jsmn is integrated. */
#define WIFI_CFG_BUF_SIZE  128u

/* ── module-private state ─────────────────────────────────────────────── */

static SemaphoreHandle_t g_wifi_mutex;
static bool              g_initialised = false;
static bool              g_linked      = false;

/* ── helpers ──────────────────────────────────────────────────────────── */

/* Read SSID and PSK from /cfg/wifi.json.
 * Format (provisional, pending JSON parsing integration):
 *   raw bytes = "<ssid>\0<psk>\0"
 * Returns ERR_NOT_FOUND if the file is absent or malformed. */
static err_t read_wifi_config(char ssid_out[WIFI_SSID_MAX],
                               char psk_out[WIFI_PSK_MAX])
{
    static uint8_t buf[WIFI_CFG_BUF_SIZE]; /* static: no stack VLA */
    size_t len = 0u;

    err_t err = storage_read("/cfg/wifi.json", buf, sizeof(buf) - 1u, &len);
    if (err != ERR_OK) {
        LOG_E("cannot read /cfg/wifi.json: %ld", (long)err);
        return err;
    }
    buf[len] = '\0';

    /* First null-terminated field is the SSID. */
    const char *ssid = (const char *)buf;
    size_t ssid_len  = strnlen(ssid, WIFI_SSID_MAX - 1u);
    if (ssid_len == 0u || ssid_len >= WIFI_SSID_MAX) {
        LOG_E("wifi.json: SSID missing or too long");
        return ERR_NOT_FOUND;
    }
    memcpy(ssid_out, ssid, ssid_len + 1u);

    /* Second field starts after the first NUL. */
    if (ssid_len + 1u >= len) {
        LOG_E("wifi.json: PSK field missing");
        return ERR_NOT_FOUND;
    }
    const char *psk = ssid + ssid_len + 1u;
    size_t psk_len  = strnlen(psk, WIFI_PSK_MAX - 1u);
    if (psk_len >= WIFI_PSK_MAX) {
        LOG_E("wifi.json: PSK too long");
        return ERR_NOT_FOUND;
    }
    memcpy(psk_out, psk, psk_len + 1u);

    return ERR_OK;
}

/* ── public API ──────────────────────────────────────────────────────── */

err_t transport_wifi_init(void)
{
    g_wifi_mutex = xSemaphoreCreateMutex();
    if (!g_wifi_mutex) {
        LOG_E("failed to create wifi mutex");
        return ERR_NO_MEM;
    }

    /* CYW43_COUNTRY_NETHERLANDS = 'N','L' per the SDK. */
    if (cyw43_arch_init_with_country(CYW43_COUNTRY_NETHERLANDS) != 0) {
        LOG_E("cyw43_arch_init_with_country failed");
        return ERR_IO;
    }

    cyw43_arch_enable_sta_mode();

    g_initialised = true;
    LOG_I("CYW43 initialised (NL), STA mode enabled");
    return ERR_OK;
}

err_t transport_wifi_connect_ssid(const char *ssid, const char *psk)
{
    if (!g_initialised) {
        LOG_E("transport_wifi_init() not called");
        return ERR_NOT_INIT;
    }
    if (!ssid || !psk) {
        return ERR_INVALID_ARG;
    }

    uint32_t backoff_s = BACKOFF_INIT_S;

    for (;;) {
        LOG_I("connecting to SSID '%s' ...", ssid);

        int rc = cyw43_arch_wifi_connect_timeout_ms(
                     ssid, psk,
                     CYW43_AUTH_WPA2_AES_PSK,
                     10000u /* 10 s connection timeout */);

        if (rc == 0) {
            xSemaphoreTake(g_wifi_mutex, portMAX_DELAY);
            g_linked = true;
            xSemaphoreGive(g_wifi_mutex);
            LOG_I("Wi-Fi link up");
            return ERR_OK;
        }

        LOG_W("connect failed (rc=%d), retry in %lu s", rc,
              (unsigned long)backoff_s);

        vTaskDelay(pdMS_TO_TICKS(backoff_s * 1000u));

        /* Exponential backoff capped at BACKOFF_MAX_S. */
        if (backoff_s < BACKOFF_MAX_S) {
            backoff_s *= 2u;
            if (backoff_s > BACKOFF_MAX_S) {
                backoff_s = BACKOFF_MAX_S;
            }
        }
    }
}

bool transport_wifi_is_linked(void)
{
    if (!g_initialised) {
        return false;
    }

    bool linked;
    xSemaphoreTake(g_wifi_mutex, portMAX_DELAY);
    linked = g_linked;
    xSemaphoreGive(g_wifi_mutex);
    return linked;
}

/* ── mDNS broker resolution ──────────────────────────────────────────── */

/*
 * TODO(spec): Resolve "_mqtt._tcp.local" via mDNS using the lwIP mdns_resp
 * API.  The resolved IP + port are written into /cfg/broker.json (or used
 * transiently).  If mDNS resolution fails or times out, fall back to the
 * static IP already stored in /cfg/broker.json.
 *
 * UDP broadcast discovery (the previous group's scheme) is explicitly
 * prohibited (CLAUDE.md §8.2 / audit §D.2).  mDNS is a link-local multicast
 * protocol and is the only permitted dynamic-discovery mechanism.
 *
 * Implementation deferred until the lwIP mDNS integration is validated on
 * bench (bring-up step 13 in CLAUDE.md §15).
 */

/* Read the static broker address from /cfg/broker.json.
 * Format (provisional): "<hostname_or_ip>\0<port_decimal>\0"
 * Returns ERR_NOT_FOUND if the file is absent. */
err_t transport_wifi_get_broker(char *host_out, size_t host_max,
                                 uint16_t *port_out)
{
    static uint8_t buf[128];
    size_t len = 0u;

    err_t err = storage_read("/cfg/broker.json", buf, sizeof(buf) - 1u, &len);
    if (err != ERR_OK) {
        LOG_E("cannot read /cfg/broker.json: %ld", (long)err);
        return err;
    }
    buf[len] = '\0';

    const char *host = (const char *)buf;
    size_t hlen      = strnlen(host, host_max - 1u);
    if (hlen == 0u) {
        LOG_E("broker.json: hostname empty");
        return ERR_NOT_FOUND;
    }
    memcpy(host_out, host, hlen + 1u);

    /* Port field follows the first NUL. */
    if (hlen + 1u >= len) {
        /* Default MQTT-over-TLS port if not specified. */
        *port_out = 8883u;
        return ERR_OK;
    }
    const char *port_str = host + hlen + 1u;
    uint32_t port = 0u;
    for (size_t i = 0u; port_str[i] != '\0' && i < 6u; i++) {
        char c = port_str[i];
        if (c < '0' || c > '9') break;
        port = port * 10u + (uint32_t)(c - '0');
    }
    *port_out = (port > 0u && port <= 65535u) ? (uint16_t)port : 8883u;

    LOG_I("broker: %s:%u", host_out, (unsigned)*port_out);
    return ERR_OK;
}

/* Attempt mDNS resolution; fall back to static config on failure.
 * Public entry point used by the transport selector. */
err_t transport_wifi_resolve_broker(char *host_out, size_t host_max,
                                     uint16_t *port_out)
{
    /* TODO(spec): implement mDNS resolve of "_mqtt._tcp.local" here using
     * lwIP's mdns_resp / dns_gethostbyname with a ".local" name.  On success,
     * fill host_out with the dotted-decimal address and *port_out with the
     * SRV port.  On failure or timeout, fall through to the static config. */

    LOG_I("mDNS resolution not yet implemented; using static broker config");
    return transport_wifi_get_broker(host_out, host_max, port_out);
}
