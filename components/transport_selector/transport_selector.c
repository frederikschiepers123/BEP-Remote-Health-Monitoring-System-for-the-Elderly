#define LOG_TAG "SEL"
#include "transport_selector.h"

#include "log.h"
#include "err.h"
#include "identity.h"
#include "tls_context.h"
#include "stream_cdc.h"
#include "stream_tcp.h"
#include "transport_wifi.h"
#include "mqtt_client.h"

#include "FreeRTOS.h"
#include "task.h"
#include "event_groups.h"
#include "semphr.h"

#include "tusb.h"

#include <string.h>
#include <stdbool.h>
#include <stddef.h>

/* ── FSM states ─────────────────────────────────────────────────────── */
typedef enum {
    SEL_BOOT,
    SEL_USB_PROBE,
    SEL_USB_ACTIVE,
    SEL_WIFI_ACTIVE,
} SelState;

/* ── constants ──────────────────────────────────────────────────────── */
#define USB_PROBE_TIMEOUT_MS      8000u
#define KEEPALIVE_MISS_THRESHOLD  3u
#define WIFI_BROKER_HOST_MAX      128u
#define SELECTOR_LOOP_PERIOD_MS   200u  /* poll interval in steady state */

/* ── module state ───────────────────────────────────────────────────── */
EventGroupHandle_t  g_transport_event_group;
static SemaphoreHandle_t g_selector_mutex;

static const Identity  *g_identity;
static SelState         g_state = SEL_BOOT;

/* Two static TLS contexts — one per transport.  No malloc in steady state. */
static TlsContext   g_tls_usb;
static TlsContext   g_tls_wifi;
static stream_t     g_cdc_stream;          /* USB-CDC byte stream  */
static stream_t     g_tcp_stream;          /* lwIP TCP byte stream */

/* Pointer to whichever tls_stream is currently active (NULL = none). */
static stream_t    *g_active_tls_stream = NULL;

/* Number of consecutive missed keepalives on USB_ACTIVE path. */
static uint8_t      g_missed_keepalives = 0u;

/* ── helpers ────────────────────────────────────────────────────────── */

static void set_active_stream(stream_t *s)
{
    xSemaphoreTake(g_selector_mutex, portMAX_DELAY);
    g_active_tls_stream = s;
    xSemaphoreGive(g_selector_mutex);

    /* Signal MQTT client that the stream changed. */
    xEventGroupSetBits(g_transport_event_group, TRANSPORT_STREAM_CHANGED_BIT);
    LOG_I("active TLS stream updated (%s)", s ? "set" : "cleared");
}

/* Attempt TLS handshake over the given raw byte stream.
 * ctx must already be initialised via tls_context_init(). */
static err_t do_tls_handshake(TlsContext *ctx, stream_t *raw)
{
    err_t err = tls_context_handshake(ctx, raw);
    if (err != ERR_OK) {
        LOG_W("TLS handshake failed: %ld", (long)err);
    }
    return err;
}

/* Attempt a full USB probe: wait for CDC enumeration, TLS handshake,
 * and MQTT CONNACK — all within deadline_ticks. */
static err_t probe_usb(TickType_t deadline_ticks)
{
    /* Wait for CDC enumeration. */
    LOG_I("USB probe: waiting for CDC0 enumeration ...");
    while (!stream_cdc_connected()) {
        if (xTaskGetTickCount() >= deadline_ticks) {
            LOG_W("USB probe: CDC0 enumeration timeout");
            return ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(50u));
    }
    LOG_I("USB probe: CDC0 enumerated");

    /* Initialise CDC stream_t if not already done. */
    err_t err = stream_cdc_init(&g_cdc_stream);
    if (err != ERR_OK) {
        LOG_W("USB probe: stream_cdc_init failed: %ld", (long)err);
        return err;
    }

    /* TLS handshake. */
    if (xTaskGetTickCount() >= deadline_ticks) {
        return ERR_TIMEOUT;
    }
    err = do_tls_handshake(&g_tls_usb, &g_cdc_stream);
    if (err != ERR_OK) return err;

    /* MQTT CONNACK — the MQTT task will send CONNECT; we just need to verify
     * the active stream is set and the MQTT client can proceed.  The actual
     * CONNACK check lives in mqtt_client; here we mark the stream ready and
     * let the MQTT task run. */
    set_active_stream(&g_tls_usb.tls_stream);

    LOG_I("USB probe succeeded");
    return ERR_OK;
}

/* Bring up Wi-Fi and establish TLS.  Runs until success or indefinitely
 * (the exponential backoff in transport_wifi_connect_ssid handles retries). */
static err_t activate_wifi(void)
{
    /* Read SSID/PSK from storage. */
    static char ssid[33];
    static char psk[65];
    static uint8_t wifi_buf[128];
    size_t wlen = 0u;

    err_t err = storage_read("/cfg/wifi.json", wifi_buf,
                              sizeof(wifi_buf) - 1u, &wlen);
    if (err != ERR_OK) {
        LOG_E("cannot read /cfg/wifi.json: %ld", (long)err);
        return err;
    }
    wifi_buf[wlen] = '\0';

    /* Parse "ssid\0psk\0" layout. */
    const char *s = (const char *)wifi_buf;
    size_t slen = strnlen(s, 32u);
    memcpy(ssid, s, slen);
    ssid[slen] = '\0';
    const char *p = (slen + 1u < wlen) ? (s + slen + 1u) : "";
    size_t plen = strnlen(p, 64u);
    memcpy(psk, p, plen);
    psk[plen] = '\0';

    err = transport_wifi_connect_ssid(ssid, psk);
    if (err != ERR_OK) return err;

    /* Resolve broker address. */
    static char broker_host[WIFI_BROKER_HOST_MAX];
    uint16_t    broker_port = 8883u;
    err = transport_wifi_resolve_broker(broker_host, sizeof(broker_host),
                                         &broker_port);
    if (err != ERR_OK) return err;

    /* Open TCP connection. */
    err = stream_tcp_connect(broker_host, broker_port, &g_tcp_stream);
    if (err != ERR_OK) {
        LOG_E("TCP connect to %s:%u failed", broker_host, broker_port);
        return err;
    }

    /* TLS handshake. */
    err = do_tls_handshake(&g_tls_wifi, &g_tcp_stream);
    if (err != ERR_OK) {
        stream_tcp_close(&g_tcp_stream);
        return err;
    }

    set_active_stream(&g_tls_wifi.tls_stream);
    LOG_I("Wi-Fi transport active");
    return ERR_OK;
}

/* ── public API ─────────────────────────────────────────────────────── */

err_t transport_selector_init(const Identity *id)
{
    if (!id) return ERR_INVALID_ARG;
    g_identity = id;

    g_transport_event_group = xEventGroupCreate();
    if (!g_transport_event_group) {
        LOG_E("xEventGroupCreate failed");
        return ERR_NO_MEM;
    }

    g_selector_mutex = xSemaphoreCreateMutex();
    if (!g_selector_mutex) {
        LOG_E("xSemaphoreCreateMutex failed");
        return ERR_NO_MEM;
    }

    /* Initialise both TLS contexts at boot (heap allowed here). */
    err_t err = tls_context_init(&g_tls_usb,  g_identity);
    if (err != ERR_OK) return err;

    err = tls_context_init(&g_tls_wifi, g_identity);
    if (err != ERR_OK) return err;

    /* Initialise Wi-Fi subsystem (doesn't connect yet). */
    err = transport_wifi_init();
    if (err != ERR_OK) return err;

    g_state = SEL_USB_PROBE;
    LOG_I("transport selector initialised");
    return ERR_OK;
}

stream_t *transport_selector_active_stream(void)
{
    stream_t *s;
    xSemaphoreTake(g_selector_mutex, portMAX_DELAY);
    s = g_active_tls_stream;
    xSemaphoreGive(g_selector_mutex);
    return s;
}

void transport_selector_task(void *arg)
{
    (void)arg;

    for (;;) {
        switch (g_state) {

        /* ── BOOT: never reached after init, but guard it ─────────── */
        case SEL_BOOT:
            g_state = SEL_USB_PROBE;
            break;

        /* ── USB_PROBE: 8-second budget ──────────────────────────── */
        case SEL_USB_PROBE: {
            TickType_t deadline = xTaskGetTickCount() +
                                  pdMS_TO_TICKS(USB_PROBE_TIMEOUT_MS);
            err_t err = probe_usb(deadline);
            if (err == ERR_OK) {
                g_missed_keepalives = 0u;
                g_state = SEL_USB_ACTIVE;
                LOG_I("state → USB_ACTIVE");
            } else {
                LOG_W("USB probe failed (%ld); falling back to Wi-Fi", (long)err);
                g_state = SEL_WIFI_ACTIVE;
                err_t werr = activate_wifi();
                if (werr != ERR_OK) {
                    /* activate_wifi() retries internally; this path only
                     * reaches here if the config is absent. */
                    LOG_E("Wi-Fi activation failed: %ld", (long)werr);
                    vTaskDelay(pdMS_TO_TICKS(5000u));
                    g_state = SEL_USB_PROBE;
                } else {
                    LOG_I("state → WIFI_ACTIVE");
                }
            }
            break;
        }

        /* ── USB_ACTIVE: monitor for unplug + missed keepalives ─── */
        case SEL_USB_ACTIVE: {
            vTaskDelay(pdMS_TO_TICKS(SELECTOR_LOOP_PERIOD_MS));

            if (!stream_cdc_connected()) {
                g_missed_keepalives++;
                LOG_W("CDC0 disconnected (missed=%u)", g_missed_keepalives);
            } else {
                /* Connection is healthy — reset counter. */
                g_missed_keepalives = 0u;
            }

            if (g_missed_keepalives >= KEEPALIVE_MISS_THRESHOLD) {
                LOG_I("USB unplug + %u missed keepalives → swap to Wi-Fi",
                      g_missed_keepalives);

                /* Save TLS session ticket on dormant USB context. */
                (void)tls_context_save_session(&g_tls_usb);

                /* Tear down USB TLS cleanly via the vtable close callback. */
                if (g_tls_usb.tls_stream.close) {
                    g_tls_usb.tls_stream.close(g_tls_usb.tls_stream.ctx);
                }

                g_missed_keepalives = 0u;
                g_state = SEL_WIFI_ACTIVE;
                set_active_stream(NULL);

                err_t werr = activate_wifi();
                if (werr == ERR_OK) {
                    LOG_I("state → WIFI_ACTIVE");
                } else {
                    LOG_E("Wi-Fi activation failed: %ld", (long)werr);
                    vTaskDelay(pdMS_TO_TICKS(5000u));
                    g_state = SEL_USB_PROBE;
                }
            }
            break;
        }

        /* ── WIFI_ACTIVE: monitor for USB return ─────────────────── */
        case SEL_WIFI_ACTIVE: {
            vTaskDelay(pdMS_TO_TICKS(SELECTOR_LOOP_PERIOD_MS));

            /* Also check that Wi-Fi link is still up. */
            if (!transport_wifi_is_linked()) {
                LOG_W("Wi-Fi link lost; re-activating");
                set_active_stream(NULL);
                err_t werr = activate_wifi();
                if (werr != ERR_OK) {
                    LOG_E("Wi-Fi re-activation failed: %ld", (long)werr);
                    /* Keep retrying; vTaskDelay inside activate_wifi(). */
                }
                break;
            }

            /* If USB returns, prefer it. */
            if (tud_cdc_n_connected(0u)) {
                LOG_I("USB returned; probing USB ...");

                /* Save Wi-Fi session ticket before swapping away. */
                (void)tls_context_save_session(&g_tls_wifi);
                if (g_tls_wifi.tls_stream.close) {
                    g_tls_wifi.tls_stream.close(g_tls_wifi.tls_stream.ctx);
                }
                stream_tcp_close(&g_tcp_stream);

                set_active_stream(NULL);
                g_state = SEL_USB_PROBE;
            }
            break;
        }

        default:
            /* Should never reach; reset to probe state. */
            g_state = SEL_USB_PROBE;
            break;
        }
    }
}
