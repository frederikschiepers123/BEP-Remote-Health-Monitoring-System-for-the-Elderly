#define LOG_TAG "UI_OLED"
#include "log.h"

#include "ui_oled.h"
#include "sh1106.h"
#include "board_pico2wh.h"
#include "app_config.h"
#include "err.h"

/* Sensor queue headers — peeked for latest samples */
#include "sensor_env.h"
#include "radar_driver.h"
#include "sensor_light.h"

/* Identity (for UUID display) */
#include "identity.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "pico/time.h"

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* ── Internal event queue ────────────────────────────────────────────────── */

typedef enum {
    UI_EVT_NEXT_PAGE = 0,
    UI_EVT_SET_PAGE,
    UI_EVT_REDRAW,
} UiEventType;

typedef struct {
    UiEventType type;
    UiPage      page;   /* used when type == UI_EVT_SET_PAGE */
} UiEvent;

#define UI_EVENT_QUEUE_DEPTH  8

static QueueHandle_t s_ui_event_q = NULL;

/* ── Current page ────────────────────────────────────────────────────────── */

static volatile UiPage s_current_page = UI_PAGE_STATUS;

/* ── Transport / MQTT state strings ─────────────────────────────────────── */
/*
 * The UI peeks at the global event group to determine transport state.
 * We use weak stubs so the UI compiles even if transport_selector is not
 * yet wired; in a complete build these are overridden by transport_selector.
 */
#include "transport_selector.h"

static const char *transport_state_string(void)
{
    /* Derive a display string from which event bits are set. */
    if (!g_transport_event_group) {
        return "INIT";
    }
    /* If the event bit is set a swap just occurred; otherwise use the
     * active stream pointer as a proxy for readiness. */
    stream_t *s = transport_selector_active_stream();
    return s ? "LINKED" : "NO LINK";
}

static bool mqtt_is_connected(void)
{
    /* Proxy: if there is an active TLS stream the MQTT client is likely up.
     * A more precise answer requires a shared flag in mqtt_client — add that
     * when the transport_task is wired. */
    return (transport_selector_active_stream() != NULL);
}

/* ── ui_oled_init ────────────────────────────────────────────────────────── */

err_t ui_oled_init(void)
{
    s_ui_event_q = xQueueCreate(UI_EVENT_QUEUE_DEPTH, sizeof(UiEvent));
    if (s_ui_event_q == NULL) {
        LOG_E("Failed to create UI event queue");
        return ERR_NO_MEM;
    }
    s_current_page = UI_PAGE_STATUS;
    return ERR_OK;
}

/* ── ui_oled_set_page / ui_oled_next_page ────────────────────────────────── */

void ui_oled_set_page(UiPage page)
{
    if (!s_ui_event_q) { return; }
    UiEvent evt = { .type = UI_EVT_SET_PAGE, .page = page };
    (void)xQueueSendToBack(s_ui_event_q, &evt, 0);
}

void ui_oled_next_page(void)
{
    if (!s_ui_event_q) { return; }
    UiEvent evt = { .type = UI_EVT_NEXT_PAGE, .page = UI_PAGE_STATUS };
    (void)xQueueSendToBack(s_ui_event_q, &evt, 0);
}

/* ── Page rendering helpers ──────────────────────────────────────────────── */

static void render_status(Sh1106 *oled)
{
    char line[22];

    sh1106_draw_text(oled, 0, 0, "RMMS SENSOR MODULE");

    /* Display first 8 chars of UUID */
    /* Identity is loaded into a global by app_main; we read it directly. */
    extern Identity g_identity;  /* declared in app_main.c */
    snprintf(line, sizeof(line), "ID:%.8s", g_identity.uuid);
    sh1106_draw_text(oled, 0, 2, line);

    /* Transport state */
    const char *ts = transport_state_string();
    snprintf(line, sizeof(line), "TR:%s", ts ? ts : "?");
    sh1106_draw_text(oled, 0, 4, line);

    /* MQTT connection */
    const char *mq = mqtt_is_connected() ? "MQTT:OK" : "MQTT:--";
    sh1106_draw_text(oled, 0, 6, mq);
}

static void render_env(Sh1106 *oled)
{
    sh1106_draw_text(oled, 0, 0, "ENV");

    EnvSample sample;
    if (xQueuePeek(q_env, &sample, 0) == pdTRUE && sample.q == 0) {
        char line[22];
        snprintf(line, sizeof(line), "T: %.1fC", (double)sample.v.temp_c);
        sh1106_draw_text(oled, 0, 2, line);
        snprintf(line, sizeof(line), "H: %.1f%%", (double)sample.v.humidity_pct);
        sh1106_draw_text(oled, 0, 4, line);
        snprintf(line, sizeof(line), "P: %.1fhPa", (double)sample.v.pressure_hpa);
        sh1106_draw_text(oled, 0, 6, line);
    } else {
        sh1106_draw_text(oled, 0, 3, "  NO DATA");
    }
}

static void render_radar(Sh1106 *oled)
{
    sh1106_draw_text(oled, 0, 0, "RADAR");

    RadarSample sample;
    if (xQueuePeek(q_radar, &sample, 0) == pdTRUE && sample.q <= 2) {
        char line[22];
        sh1106_draw_text(oled, 0, 1,
                         sample.presence ? "PRESENT" : "ABSENT");
        snprintf(line, sizeof(line), "HR: %.0f BPM", (double)sample.heart_bpm);
        sh1106_draw_text(oled, 0, 3, line);
        snprintf(line, sizeof(line), "BR: %.0f RPM", (double)sample.breath_rpm);
        sh1106_draw_text(oled, 0, 5, line);
        if (sample.q == 2) {
            sh1106_draw_text(oled, 0, 7, "DEGRADED");
        }
    } else {
        sh1106_draw_text(oled, 0, 3, "  NO DATA");
    }
}

static void render_light(Sh1106 *oled)
{
    sh1106_draw_text(oled, 0, 0, "LIGHT");

    LightSample sample;
    if (xQueuePeek(q_light, &sample, 0) == pdTRUE && sample.q <= 2) {
        char line[22];
        snprintf(line, sizeof(line), "%.1f LUX", (double)sample.lux);
        sh1106_draw_text(oled, 0, 3, line);
    } else {
        sh1106_draw_text(oled, 0, 3, "  NO DATA");
    }
}

/* ── ui_task ─────────────────────────────────────────────────────────────── */

void ui_task(void *arg)
{
    (void)arg;

    /* SH1106 is on I2C0 shared with BME280. */
    static Sh1106 s_oled;
    err_t err = sh1106_init(&s_oled, BOARD_I2C_INST, BOARD_OLED_ADDR);
    if (err != ERR_OK) {
        LOG_E("SH1106 init failed: %d — ui_task halting", err);
        vTaskSuspend(NULL);
    }

    for (;;) {
        /*
         * Wait for an event with a 1-second timeout for the periodic redraw.
         */
        UiEvent evt;
        BaseType_t got_event = xQueueReceive(s_ui_event_q, &evt,
                                             pdMS_TO_TICKS(1000));
        if (got_event == pdTRUE) {
            if (evt.type == UI_EVT_SET_PAGE) {
                if (evt.page < UI_PAGE_COUNT) {
                    s_current_page = evt.page;
                }
            } else if (evt.type == UI_EVT_NEXT_PAGE) {
                s_current_page = (UiPage)((s_current_page + 1) % UI_PAGE_COUNT);
            }
            /* UI_EVT_REDRAW just falls through to redraw */
        }

        /* Redraw current page */
        sh1106_clear(&s_oled);

        switch (s_current_page) {
        case UI_PAGE_STATUS: render_status(&s_oled); break;
        case UI_PAGE_ENV:    render_env(&s_oled);    break;
        case UI_PAGE_RADAR:  render_radar(&s_oled);  break;
        case UI_PAGE_LIGHT:  render_light(&s_oled);  break;
        default:             render_status(&s_oled); break;
        }

        sh1106_flush(&s_oled);
    }
}
