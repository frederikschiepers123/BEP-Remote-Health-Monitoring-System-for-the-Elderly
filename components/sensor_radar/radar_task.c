#define LOG_TAG "RADAR"
#include "log.h"

#include "radar_driver.h"
#include "radar_filter.h"
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

    /* MCU-side plausibility filter (ADR-0005): debounce presence, gate +
     * smooth distance/vitals.  Applied on the generic RadarSample, above the
     * driver v-table, so it is driver-agnostic. */
    static RadarFilter filt;
    radar_filter_init(&filt);

    uint32_t seq     = 0;
    uint32_t dropped = 0;

    for (;;) {
        wdt_task_alive(WDT_TASK_RADAR);

        RadarSample raw;
        /* read_sample blocks for up to 500 ms waiting for data */
        err = drv->read_sample(drv->ctx, &raw, 500U);
        if (err != ERR_OK) {
            if (err == ERR_TIMEOUT) {
                /* No frame in 500 ms — still clock the filter below so its
                 * absence / input-timeout windows keep counting (ADR-0005).
                 * Skipping here would freeze the filter at its last state
                 * (e.g. presence=true with held vitals) across a radar
                 * outage, and let it resume that state on recovery. */
                LOG_D("Radar read timeout (no frame in 500 ms)");
            } else {
                LOG_W("Radar read error: %d", err);
            }
            raw.presence    = false;
            raw.distance_mm = 0;
            raw.breath_rpm  = 0.0f;
            raw.heart_bpm   = 0.0f;
            raw.q           = 3;
        }

        RadarSample sample;
        radar_filter_apply(&filt, &raw,
                           to_ms_since_boot(get_absolute_time()), &sample);

        /* Robust vitals estimate — fires repeatedly while present, once both
         * per-vital windows refill (ADR-0005).  Dev-console only — the §9.1
         * topic set is closed, so putting this on the wire would be an ADR +
         * Radxa sign-off. */
        RadarVitalsEstimate est;
        if (radar_filter_take_estimate(&filt, &est)) {
            LOG_I("Final stable estimate: heart %.1f BPM (±%.1f, n=%d), "
                  "breath %.1f RPM (±%.1f, n=%d)",
                  (double)est.heart_bpm, (double)est.heart_spread, est.heart_n,
                  (double)est.breath_rpm, (double)est.breath_spread,
                  est.breath_n);
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
