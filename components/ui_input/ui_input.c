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

typedef struct {
    TimerHandle_t timer;
    Button        btn;
    bool          pending;   /* true while debounce timer is running */
} BtnState;

static BtnState    s_btn_a;
static BtnState    s_btn_b;
static button_cb_t s_user_cb  = NULL;
static void       *s_user_ptr = NULL;

/* ── Timer callback (timer daemon task context) ──────────────────────────── */

static void debounce_timer_cb(TimerHandle_t timer)
{
    BtnState *state = (BtnState *)pvTimerGetTimerID(timer);
    if (!state) { return; }

    state->pending = false;

    /* Re-sample: GPIO should still be low for a genuine press. */
    uint32_t gpio_pin = (state->btn == BTN_A) ? BOARD_BTN_A_PIN
                                               : BOARD_BTN_B_PIN;
    bool still_pressed = !gpio_get(gpio_pin);   /* active-low */

    if (still_pressed && s_user_cb) {
        s_user_cb(state->btn, s_user_ptr);
        LOG_D("Button %s pressed", (state->btn == BTN_A) ? "A" : "B");
    }
}

/* ── GPIO IRQ handler ────────────────────────────────────────────────────── */

static void gpio_irq_handler(uint gpio, uint32_t events)
{
    if (!(events & GPIO_IRQ_EDGE_FALL)) {
        return;
    }

    BtnState *state = NULL;
    if (gpio == BOARD_BTN_A_PIN) {
        state = &s_btn_a;
    } else if (gpio == BOARD_BTN_B_PIN) {
        state = &s_btn_b;
    } else {
        return;
    }

    if (state->pending) {
        /* Already debouncing — ignore additional edges */
        return;
    }
    state->pending = true;

    /* Start (or restart) the one-shot debounce timer from ISR context. */
    BaseType_t higher_prio_woken = pdFALSE;
    xTimerResetFromISR(state->timer, &higher_prio_woken);
    portYIELD_FROM_ISR(higher_prio_woken);
}

/* ── ui_input_init ───────────────────────────────────────────────────────── */

err_t ui_input_init(button_cb_t cb, void *user)
{
    s_user_cb  = cb;
    s_user_ptr = user;

    /* Initialise button state structs */
    s_btn_a.btn     = BTN_A;
    s_btn_a.pending = false;
    s_btn_b.btn     = BTN_B;
    s_btn_b.pending = false;

    /* Create debounce timers (one-shot, auto-reload=false) */
    s_btn_a.timer = xTimerCreate("debounce_a",
                                 pdMS_TO_TICKS(DEBOUNCE_MS),
                                 pdFALSE,          /* one-shot */
                                 (void *)&s_btn_a,
                                 debounce_timer_cb);
    if (!s_btn_a.timer) {
        LOG_E("Failed to create debounce timer A");
        return ERR_NO_MEM;
    }

    s_btn_b.timer = xTimerCreate("debounce_b",
                                 pdMS_TO_TICKS(DEBOUNCE_MS),
                                 pdFALSE,          /* one-shot */
                                 (void *)&s_btn_b,
                                 debounce_timer_cb);
    if (!s_btn_b.timer) {
        LOG_E("Failed to create debounce timer B");
        return ERR_NO_MEM;
    }

    /* Configure GPIO pins: input, internal pull-up */
    gpio_init(BOARD_BTN_A_PIN);
    gpio_set_dir(BOARD_BTN_A_PIN, GPIO_IN);
    gpio_pull_up(BOARD_BTN_A_PIN);

    gpio_init(BOARD_BTN_B_PIN);
    gpio_set_dir(BOARD_BTN_B_PIN, GPIO_IN);
    gpio_pull_up(BOARD_BTN_B_PIN);

    /* Register IRQ handler for falling edge on both pins */
    gpio_set_irq_enabled_with_callback(BOARD_BTN_A_PIN,
                                       GPIO_IRQ_EDGE_FALL,
                                       true,
                                       gpio_irq_handler);
    gpio_set_irq_enabled_with_callback(BOARD_BTN_B_PIN,
                                       GPIO_IRQ_EDGE_FALL,
                                       true,
                                       gpio_irq_handler);

    LOG_I("Button input init OK (A=GPIO%u, B=GPIO%u, debounce=%u ms)",
          BOARD_BTN_A_PIN, BOARD_BTN_B_PIN, DEBOUNCE_MS);
    return ERR_OK;
}
