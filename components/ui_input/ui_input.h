#ifndef UI_INPUT_H
#define UI_INPUT_H

/* Button input driver — GPIO interrupt-driven with FreeRTOS debounce.
 *
 * Hardware:
 *   BOARD_BTN_A_PIN (GPIO20) — active-low, internal pull-up.
 *   BOARD_BTN_B_PIN (GPIO21) — active-low, internal pull-up.
 *
 * Debounce:
 *   A falling-edge interrupt starts a one-shot FreeRTOS software timer
 *   (50 ms).  Additional edges during this window are ignored.  On timer
 *   expiry, the GPIO state is re-sampled; if still asserted (low), the
 *   callback is invoked.  This avoids false triggers from contact bounce.
 *
 * Thread safety:
 *   The callback is invoked from the FreeRTOS timer daemon task context.
 *   It must not block.
 *
 * Lock order: none — ui_input has no internal mutex.
 */

#include "err.h"
#include <stdint.h>

/* ── Button enumeration ──────────────────────────────────────────────────── */

typedef enum {
    BTN_A = 0,
    BTN_B = 1,
} Button;

/* ── Callback type ───────────────────────────────────────────────────────── */

/**
 * Called from the timer daemon task after a debounced button press.
 *
 * @param btn   Which button was pressed.
 * @param user  The user pointer passed to ui_input_init().
 */
typedef void (*button_cb_t)(Button btn, void *user);

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * Initialise GPIO pins and register the interrupt callback.
 *
 * Must be called after the FreeRTOS scheduler has started (or at least after
 * xTimerCreate infrastructure is available) because it allocates software
 * timers.
 *
 * @param cb   Callback to invoke on a debounced button press.  Must not block.
 * @param user Opaque pointer forwarded to the callback.
 *
 * @return ERR_OK on success, ERR_NO_MEM if timer creation fails.
 */
err_t ui_input_init(button_cb_t cb, void *user);

#endif /* UI_INPUT_H */
