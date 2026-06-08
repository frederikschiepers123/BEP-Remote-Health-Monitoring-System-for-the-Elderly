#ifndef SENSOR_IR_CAMERA_H
#define SENSOR_IR_CAMERA_H

/* IR camera task — SPI frame capture and metadata publication.
 *
 * TODO(spec): IR camera part TBD — see CLAUDE.md §16 open question 1.
 * This is a placeholder implementation until the part is confirmed.
 * The driver is entirely part-specific; note that most thermal sensors
 * (MLX90640, AMG8833) use I²C not SPI, so the bus hint may indicate
 * a different type of device.  Resolve before bring-up step 12.
 *
 * Current behaviour:
 *   - Creates q_ir_meta queue.
 *   - Runs a loop at ~10 Hz.
 *   - Publishes IrFrameMeta with q=3 (invalid) until the part is known.
 *   - Does NOT attempt SPI transactions on an unconfirmed part.
 */

#include "err.h"

#include "FreeRTOS.h"
#include "queue.h"

#include <stdint.h>

/* Maximum IR frame payload in bytes (conservative upper bound). */
#define IR_FRAME_MAX_BYTES  2048U

/* ── Frame metadata ──────────────────────────────────────────────────────── */

typedef struct {
    uint64_t ts_us;     /* monotonic microseconds since boot */
    uint32_t seq;       /* per-topic monotonic counter */
    uint16_t width;     /* frame width in pixels (0 until part confirmed) */
    uint16_t height;    /* frame height in pixels (0 until part confirmed) */
    uint8_t  q;         /* 0=ok, 3=invalid */
} IrFrameMeta;

/* ── Shared queue (meta only; full frames go via ring buffer — future) ────── */

extern QueueHandle_t q_ir_meta;

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * Initialise the IR camera component.  Creates q_ir_meta.
 * Must be called before the scheduler starts.
 */
err_t ir_task_init(void);

/** FreeRTOS task entry point.  Never returns. */
void ir_task(void *arg);

#endif /* SENSOR_IR_CAMERA_H */
