#ifndef UI_OLED_H
#define UI_OLED_H

/* 5-page OLED UI state machine (SH1122 256×64).
 *
 * Pages:
 *   UI_PAGE_STATUS — UUID prefix, Wi-Fi IP, MQTT state.
 *   UI_PAGE_ENV    — last env sample (temp, humidity, pressure or "—").
 *   UI_PAGE_AIR    — last air sample (AQI label, CO₂, TVOC).
 *   UI_PAGE_RADAR  — last radar sample (presence, HR, BR).
 *   UI_PAGE_LIGHT  — last light sample (lux).
 *
 * The UI task does NOT read the sensor queues (that would race the transport's
 * consume).  Instead the transport_task — the single queue consumer — pushes
 * each fresh sample into this module's shadow state via the ui_oled_set_*
 * setters below, and ui_task renders from that shadow.  Setters are
 * lock-free: volatile scalars written by transport_task, read by ui_task; a
 * torn read at worst shows last-tick's value, which is fine for a display.
 *
 * Redraw triggers: button press (immediate, via SW2), or the 1 Hz tick.
 *
 * ui_task takes the i2c_bus lock around the SH1122 flush (shared I²C0). */

#include "err.h"

#include "FreeRTOS.h"

#include <stdint.h>
#include <stdbool.h>

/* ── Page enumeration ────────────────────────────────────────────────────── */

typedef enum {
    UI_PAGE_STATUS = 0,
    UI_PAGE_ENV,
    UI_PAGE_AIR,
    UI_PAGE_RADAR,
    UI_PAGE_LIGHT,
    UI_PAGE_COUNT
} UiPage;

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

/** Create the internal event queue.  Call from app_main before scheduler. */
err_t ui_oled_init(void);

/** FreeRTOS task entry point.  Never returns.  arg unused. */
void ui_task(void *arg);

/* ── Page control (thread-safe) ──────────────────────────────────────────── */

void ui_oled_set_page(UiPage page);
void ui_oled_next_page(void);

/* ── Shadow-state setters (called by transport_task) ─────────────────────── */

/** Network/broker line: IP string (≤15 chars, copied) and MQTT-up flag. */
void ui_oled_set_net(const char *ip, bool mqtt_ok);

/** Latest env sample.  pres_valid=false renders pressure as "—" (AHT21). */
void ui_oled_set_env(float temp_c, float hum_pct, float pres_hpa,
                     bool pres_valid, uint8_t q);

/** Latest air sample (AQI 1..5; 0 = warming up). */
void ui_oled_set_air(uint16_t co2_ppm, uint16_t tvoc_ppb, uint8_t aqi,
                     uint8_t q);

/** Latest radar sample. */
void ui_oled_set_radar(bool presence, uint32_t distance_mm,
                       float breath_bpm, float heart_bpm, uint8_t q);

/** Latest light sample. */
void ui_oled_set_light(float lux, uint8_t q);

#endif /* UI_OLED_H */
