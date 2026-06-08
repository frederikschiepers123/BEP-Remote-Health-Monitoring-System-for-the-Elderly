#define LOG_TAG "IR_CAM"
#include "log.h"

#include "sensor_ir_camera.h"
#include "board_pico2wh.h"
#include "app_config.h"
#include "err.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "pico/time.h"

#include <stdint.h>

/*
 * TODO(spec): implement when IR camera part is confirmed — CLAUDE.md §16 Q1.
 *
 * Until the part is confirmed this stub:
 *   1. Creates q_ir_meta (required by transport_task).
 *   2. Runs at ~10 Hz.
 *   3. Publishes IrFrameMeta with q=3 (invalid) so downstream consumers can
 *      distinguish "stub running" from "task not started".
 *   4. Does NOT initialise SPI or assert the CS pin, so no bus contention
 *      occurs on an unknown device.
 *
 * When the part is confirmed, replace this file with a real driver that:
 *   - Calls spi_init on BOARD_IR_SPI_INST at BOARD_IR_SPI_FREQ_HZ.
 *   - Asserts BOARD_IR_CS_PIN, transfers a frame, deasserts CS.
 *   - Fills width, height, and q=0 in IrFrameMeta.
 *   - Writes the binary frame to a ring buffer for transport_task.
 */

/* ── Watchdog forward declaration ────────────────────────────────────────── */

extern void wdt_task_alive(WdtTaskId id);

/* ── Module-level state ──────────────────────────────────────────────────── */

QueueHandle_t q_ir_meta = NULL;

/* ── ir_task_init ────────────────────────────────────────────────────────── */

err_t ir_task_init(void)
{
    q_ir_meta = xQueueCreate(Q_IR_META_DEPTH, sizeof(IrFrameMeta));
    if (q_ir_meta == NULL) {
        LOG_E("Failed to create q_ir_meta");
        return ERR_NO_MEM;
    }
    return ERR_OK;
}

/* ── ir_task ─────────────────────────────────────────────────────────────── */

void ir_task(void *arg)
{
    (void)arg;

    LOG_W("IR camera driver is a stub — part not yet confirmed (CLAUDE.md §16 Q1)");

    uint32_t seq     = 0;
    uint32_t dropped = 0;

    for (;;) {
        wdt_task_alive(WDT_TASK_IR);

        IrFrameMeta meta;
        meta.ts_us  = (uint64_t)to_us_since_boot(get_absolute_time());
        meta.seq    = seq++;
        meta.width  = 0;
        meta.height = 0;
        meta.q      = 3;   /* invalid — part not confirmed */

        if (xQueueSendToBack(q_ir_meta, &meta, 0) != pdTRUE) {
            dropped++;
            if (dropped % 10 == 0) {
                LOG_W("q_ir_meta full — dropped %lu frames",
                      (unsigned long)dropped);
            }
        }

        /* ~10 Hz */
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
