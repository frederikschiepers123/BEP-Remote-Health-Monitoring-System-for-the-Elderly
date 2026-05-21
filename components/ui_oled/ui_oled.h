#ifndef UI_OLED_H
#define UI_OLED_H

/* 4-page OLED UI state machine.
 *
 * Pages:
 *   UI_PAGE_STATUS  — device UUID prefix, active transport, MQTT state.
 *   UI_PAGE_ENV     — last BME280 sample (temp, humidity, pressure).
 *   UI_PAGE_RADAR   — last radar sample (presence, HR, BR).
 *   UI_PAGE_LIGHT   — last light sample (lux).
 *
 * The UI task runs at Priority 2 on Core 0.  It peeks (does not consume)
 * the sensor queues to retrieve the latest samples for display.
 * Button presses advance the active page.
 *
 * Redraw triggers:
 *   1. Button press (immediate).
 *   2. Periodic 1 Hz tick.
 */

#include "err.h"

#include "FreeRTOS.h"

/* ── Page enumeration ────────────────────────────────────────────────────── */

typedef enum {
    UI_PAGE_STATUS = 0,
    UI_PAGE_ENV,
    UI_PAGE_RADAR,
    UI_PAGE_LIGHT,
    UI_PAGE_COUNT
} UiPage;

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * Initialise the OLED UI component.  Called from app_main before the
 * scheduler starts.  Creates internal event queue.
 *
 * @return ERR_OK on success.
 */
err_t ui_oled_init(void);

/**
 * FreeRTOS task entry point.  Never returns.
 * Created by app_main.c; arg is unused.
 */
void ui_task(void *arg);

/**
 * Set the active display page.  Thread-safe (posts an event to the UI task).
 */
void ui_oled_set_page(UiPage page);

/**
 * Advance to the next page (wraps around).  Thread-safe.
 */
void ui_oled_next_page(void);

#endif /* UI_OLED_H */
