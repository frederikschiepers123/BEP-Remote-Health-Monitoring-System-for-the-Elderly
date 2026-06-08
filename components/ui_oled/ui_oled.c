#define LOG_TAG "UI_OLED"
#include "log.h"

#include "ui_oled.h"
#include "sh1122.h"
#include "board_pico2wh.h"
#include "i2c_bus.h"
#include "app_config.h"
#include "err.h"

/* Identity (UUID prefix on the status page) */
#include "identity.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

extern void wdt_task_alive(WdtTaskId id);
extern Identity g_identity;   /* loaded by app_main.c */

/* ── Internal page-event queue ───────────────────────────────────────────── */

typedef enum {
    UI_EVT_NEXT_PAGE = 0,
    UI_EVT_SET_PAGE,
} UiEventType;

typedef struct {
    UiEventType type;
    UiPage      page;
} UiEvent;

#define UI_EVENT_QUEUE_DEPTH  8

static QueueHandle_t s_ui_event_q = NULL;
static volatile UiPage s_current_page = UI_PAGE_STATUS;

/* ── Shadow state (written by transport_task, read by ui_task) ───────────── */

static char             s_net_ip[16] = "----";
static volatile bool    s_net_mqtt_ok = false;
static char             s_diag[24] = "boot";

static volatile float   s_env_temp = 0.0f, s_env_hum = 0.0f, s_env_pres = 0.0f;
static volatile bool    s_env_pres_valid = false;
static volatile uint8_t s_env_q = 3;
static volatile bool    s_env_have = false;

static volatile uint16_t s_air_co2 = 0, s_air_tvoc = 0;
static volatile uint8_t  s_air_aqi = 0, s_air_q = 3;
static volatile bool     s_air_have = false;

static volatile bool     s_radar_pres = false;
static volatile uint32_t s_radar_dist = 0;
static volatile float    s_radar_breath = 0.0f, s_radar_heart = 0.0f;
static volatile uint8_t  s_radar_q = 3;
static volatile bool     s_radar_have = false;

static volatile float    s_light_lux = 0.0f;
static volatile uint8_t  s_light_q = 3;
static volatile bool     s_light_have = false;

/* ── Setters ─────────────────────────────────────────────────────────────── */

void ui_oled_set_net(const char *ip, bool mqtt_ok)
{
    if (ip) {
        /* Single writer (transport_task); ui_task only reads. */
        snprintf(s_net_ip, sizeof(s_net_ip), "%s", ip);
    }
    s_net_mqtt_ok = mqtt_ok;
}

void ui_oled_set_diag(const char *msg)
{
    if (msg) {
        snprintf(s_diag, sizeof(s_diag), "%s", msg);
    }
}

void ui_oled_set_env(float temp_c, float hum_pct, float pres_hpa,
                     bool pres_valid, uint8_t q)
{
    s_env_temp = temp_c; s_env_hum = hum_pct; s_env_pres = pres_hpa;
    s_env_pres_valid = pres_valid; s_env_q = q; s_env_have = true;
}

void ui_oled_set_air(uint16_t co2_ppm, uint16_t tvoc_ppb, uint8_t aqi, uint8_t q)
{
    s_air_co2 = co2_ppm; s_air_tvoc = tvoc_ppb; s_air_aqi = aqi;
    s_air_q = q; s_air_have = true;
}

void ui_oled_set_radar(bool presence, uint32_t distance_mm,
                       float breath_bpm, float heart_bpm, uint8_t q)
{
    s_radar_pres = presence; s_radar_dist = distance_mm;
    s_radar_breath = breath_bpm; s_radar_heart = heart_bpm;
    s_radar_q = q; s_radar_have = true;
}

void ui_oled_set_light(float lux, uint8_t q)
{
    s_light_lux = lux; s_light_q = q; s_light_have = true;
}

/* ── Lifecycle / page control ────────────────────────────────────────────── */

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

/* ── Page rendering ──────────────────────────────────────────────────────── */

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

static void page_indicator(Sh1122 *o, uint8_t n)
{
    char s[8];
    snprintf(s, sizeof(s), "%u/5", (unsigned)n);
    sh1122_draw_text(o, 240, 56, 1, s);
}

static void render_status(Sh1122 *o)
{
    char line[40];
    sh1122_draw_text(o, 0, 0, 2, "RMMS SENSOR MODULE");
    snprintf(line, sizeof(line), "ID %.8s", g_identity.uuid);
    sh1122_draw_text(o, 0, 18, 2, line);
    snprintf(line, sizeof(line), "WIFI %s", s_net_ip);
    sh1122_draw_text(o, 0, 36, 2, line);
    snprintf(line, sizeof(line), "MQTT %s  %s",
             s_net_mqtt_ok ? "OK" : "--", s_diag);
    sh1122_draw_text(o, 0, 52, 1, line);
    page_indicator(o, 1);
}

static void render_env(Sh1122 *o)
{
    char line[40];
    sh1122_draw_text(o, 0, 0, 2, "ENV  /env");
    if (s_env_have && s_env_q == 0) {
        snprintf(line, sizeof(line), "T %5.1f C", (double)s_env_temp);
        sh1122_draw_text(o, 0, 18, 2, line);
        snprintf(line, sizeof(line), "H %5.1f %%", (double)s_env_hum);
        sh1122_draw_text(o, 0, 36, 2, line);
        if (s_env_pres_valid) {
            snprintf(line, sizeof(line), "P %6.1f HPA", (double)s_env_pres);
        } else {
            snprintf(line, sizeof(line), "P --");   /* AHT21: no barometer */
        }
        sh1122_draw_text(o, 124, 18, 2, line);
    } else {
        sh1122_draw_text(o, 0, 18, 2, "WAITING FOR SAMPLE");
    }
    page_indicator(o, 2);
}

static void render_air(Sh1122 *o)
{
    char line[40];
    sh1122_draw_text(o, 0, 0, 2, "AIR  /air");
    if (s_air_have && s_air_aqi >= 1) {
        const char *flag = (s_air_q == 0) ? "" : "*";   /* * = not yet NORMAL */
        snprintf(line, sizeof(line), "AQI %u (%s)%s",
                 s_air_aqi, aqi_str(s_air_aqi), flag);
        sh1122_draw_text(o, 0, 18, 2, line);
        snprintf(line, sizeof(line), "CO2 %u PPM", s_air_co2);
        sh1122_draw_text(o, 0, 36, 2, line);
        snprintf(line, sizeof(line), "TVOC %u PPB", s_air_tvoc);
        sh1122_draw_text(o, 0, 50, 1, line);
    } else if (s_air_have) {
        snprintf(line, sizeof(line), "WARMING UP  q=%u", (unsigned)s_air_q);
        sh1122_draw_text(o, 0, 18, 2, line);
    } else {
        sh1122_draw_text(o, 0, 18, 2, "WAITING FOR SAMPLE");
    }
    page_indicator(o, 3);
}

static void render_radar(Sh1122 *o)
{
    char line[40];
    sh1122_draw_text(o, 0, 0, 2, "RADAR  /radar");
    if (s_radar_have && s_radar_q != 3) {
        sh1122_draw_text(o, 0, 18, 2, s_radar_pres ? "PRESENT" : "ABSENT");
        snprintf(line, sizeof(line), "HR %3.0f BPM", (double)s_radar_heart);
        sh1122_draw_text(o, 0, 36, 2, line);
        snprintf(line, sizeof(line), "BR %3.0f RPM", (double)s_radar_breath);
        sh1122_draw_text(o, 140, 36, 2, line);
        if (s_radar_q == 2) {
            sh1122_draw_text(o, 0, 50, 1, "DEGRADED");
        }
    } else {
        sh1122_draw_text(o, 0, 18, 2, "WAITING FOR SAMPLE");
    }
    page_indicator(o, 4);
}

static void render_light(Sh1122 *o)
{
    char line[40];
    sh1122_draw_text(o, 0, 0, 2, "LIGHT  /light");
    if (s_light_have && s_light_q != 3) {
        snprintf(line, sizeof(line), "%.1f LUX", (double)s_light_lux);
        sh1122_draw_text(o, 0, 18, 2, line);
    } else {
        sh1122_draw_text(o, 0, 18, 2, "WAITING FOR SAMPLE");
    }
    page_indicator(o, 5);
}

/* ── ui_task ─────────────────────────────────────────────────────────────── */

void ui_task(void *arg)
{
    (void)arg;

    static Sh1122 s_oled;
    i2c_bus_lock();
    err_t err = sh1122_init(&s_oled, BOARD_I2C_INST, BOARD_OLED_ADDR);
    i2c_bus_unlock();
    if (err != ERR_OK) {
        LOG_E("SH1122 init failed: %d — ui_task halting", err);
        /* Keep heartbeating so the watchdog supervisor does not panic the
         * whole device just because the (non-critical) display is absent. */
        for (;;) {
            wdt_task_alive(WDT_TASK_UI);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    LOG_I("ui task started (SH1122 @ 0x%02X)", BOARD_OLED_ADDR);

    for (;;) {
        wdt_task_alive(WDT_TASK_UI);

        /* Wait for a page event, or fall through after 1 s for a periodic
         * redraw (so shadow-state updates appear without a button press). */
        UiEvent evt;
        if (xQueueReceive(s_ui_event_q, &evt, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (evt.type == UI_EVT_SET_PAGE && evt.page < UI_PAGE_COUNT) {
                s_current_page = evt.page;
            } else if (evt.type == UI_EVT_NEXT_PAGE) {
                s_current_page = (UiPage)((s_current_page + 1) % UI_PAGE_COUNT);
            }
        }

        sh1122_clear(&s_oled);
        switch (s_current_page) {
        case UI_PAGE_STATUS: render_status(&s_oled); break;
        case UI_PAGE_ENV:    render_env(&s_oled);    break;
        case UI_PAGE_AIR:    render_air(&s_oled);    break;
        case UI_PAGE_RADAR:  render_radar(&s_oled);  break;
        case UI_PAGE_LIGHT:  render_light(&s_oled);  break;
        default:             render_status(&s_oled); break;
        }

        /* Only the flush touches I²C; draw ops are framebuffer-only. */
        i2c_bus_lock();
        (void)sh1122_flush(&s_oled);
        i2c_bus_unlock();
    }
}
