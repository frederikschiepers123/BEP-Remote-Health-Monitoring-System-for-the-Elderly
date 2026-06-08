#define LOG_TAG "UI_INPUT"
#include "log.h"

#include "ui_input.h"
#include "board_pico2wh.h"
#include "err.h"

#include "hardware/gpio.h"

#include "FreeRTOS.h"
#include "timers.h"

#include <stdint.h>
#include <stdbool.h>

/* ── Debounce period ─────────────────────────────────────────────────────── */
#define DEBOUNCE_MS  50U

/* ── Module-level state ──────────────────────────────────────────────────── */
/* The v1 PCB has a single user button (SW2 → BOARD_BTN_DISPLAY_PIN). */

static TimerHandle_t s_timer    = NULL;
static bool          s_pending  = false;   /* debounce timer in flight */
static button_cb_t   s_user_cb  = NULL;
static void         *s_user_ptr = NULL;

/* ── Timer callback (timer daemon task context) ──────────────────────────── */

static void debounce_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    s_pending = false;

    /* Re-sample: still low (pressed) for a genuine press, not a bounce. */
    if (!gpio_get(BOARD_BTN_DISPLAY_PIN) && s_user_cb) {   /* active-low */
        s_user_cb(BTN_DISPLAY, s_user_ptr);
        LOG_D("SW2 (display) pressed");
    }
}

/* ── GPIO IRQ handler ────────────────────────────────────────────────────── */

static void gpio_irq_handler(uint gpio, uint32_t events)
{
    if (gpio != BOARD_BTN_DISPLAY_PIN || !(events & GPIO_IRQ_EDGE_FALL)) {
        return;
    }
    if (s_pending) {
        return;   /* already debouncing — ignore additional edges */
    }
    s_pending = true;

    BaseType_t higher_prio_woken = pdFALSE;
    xTimerResetFromISR(s_timer, &higher_prio_woken);
    portYIELD_FROM_ISR(higher_prio_woken);
}

/* ── ui_input_init ───────────────────────────────────────────────────────── */

err_t ui_input_init(button_cb_t cb, void *user)
{
    s_user_cb  = cb;
    s_user_ptr = user;
    s_pending  = false;

    s_timer = xTimerCreate("debounce_disp",
                           pdMS_TO_TICKS(DEBOUNCE_MS),
                           pdFALSE,            /* one-shot */
                           NULL,
                           debounce_timer_cb);
    if (!s_timer) {
        LOG_E("Failed to create debounce timer");
        return ERR_NO_MEM;
    }

    /* SW2 has an external 1 kΩ pull-up (schematic); configure input only. */
    gpio_init(BOARD_BTN_DISPLAY_PIN);
    gpio_set_dir(BOARD_BTN_DISPLAY_PIN, GPIO_IN);

    gpio_set_irq_enabled_with_callback(BOARD_BTN_DISPLAY_PIN,
                                       GPIO_IRQ_EDGE_FALL,
                                       true,
                                       gpio_irq_handler);

    LOG_I("Button input init OK (SW2=GPIO%u, debounce=%u ms)",
          BOARD_BTN_DISPLAY_PIN, DEBOUNCE_MS);
    return ERR_OK;
}
