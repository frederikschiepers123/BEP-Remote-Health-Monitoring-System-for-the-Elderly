#define LOG_TAG "RADAR"
#include "log.h"

#include "radar_driver.h"
#include "board_pico2wh.h"
#include "app_config.h"
#include "err.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "pico/time.h"

#include <stdint.h>

/* ── Watchdog forward declaration ────────────────────────────────────────── */

extern void wdt_task_alive(WdtTaskId id);

/* ── Module-level state ──────────────────────────────────────────────────── */

QueueHandle_t q_radar = NULL;

/* ── radar_task_init ─────────────────────────────────────────────────────── */

err_t radar_task_init(void)
{
    q_radar = xQueueCreate(Q_RADAR_DEPTH, sizeof(RadarSample));
    if (q_radar == NULL) {
        LOG_E("Failed to create q_radar");
        return ERR_NO_MEM;
    }
    return ERR_OK;
}

/* ── radar_task ──────────────────────────────────────────────────────────── */

void radar_task(void *arg)
{
    (void)arg;

    /* Select driver from config (blocking until config is readable). */
    radar_driver_t *drv = radar_select_from_config();
    if (drv == NULL) {
        LOG_E("radar_select_from_config() returned NULL — radar_task halting");
        vTaskSuspend(NULL);
    }

    /* Initialise the selected driver. */
    err_t err = drv->init(drv->ctx, BOARD_RADAR_UART_INST);
    if (err != ERR_OK) {
        LOG_E("Radar driver %s init failed: %d", drv->name, err);
        vTaskSuspend(NULL);
    }

    LOG_I("Radar task started, driver=%s", drv->name);

    uint32_t seq     = 0;
    uint32_t dropped = 0;

    for (;;) {
        wdt_task_alive(WDT_TASK_RADAR);

        RadarSample sample;
        /* read_sample blocks for up to 500 ms waiting for data */
        err = drv->read_sample(drv->ctx, &sample, 500U);
        if (err == ERR_TIMEOUT) {
            /* No frame within 500 ms — normal for slow-update radars */
            LOG_D("Radar read timeout (no frame in 500 ms)");
            continue;
        }
        if (err != ERR_OK) {
            LOG_W("Radar read error: %d", err);
            sample.presence    = false;
            sample.distance_mm = 0;
            sample.breath_rpm  = 0.0f;
            sample.heart_bpm   = 0.0f;
            sample.q           = 3;
        }

        /* Non-blocking enqueue */
        if (xQueueSendToBack(q_radar, &sample, 0) != pdTRUE) {
            dropped++;
            if (dropped % 10 == 0) {
                LOG_W("q_radar full — dropped %lu samples so far",
                      (unsigned long)dropped);
            }
        }

        (void)seq;
        seq++;
    }
}
