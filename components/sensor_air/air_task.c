#define LOG_TAG "AIR"
#include "log.h"

#include "sensor_air.h"
#include "ens160.h"
#include "sensor_env.h"     /* env_last_reading() for compensation */
#include "board_pico2wh.h"
#include "i2c_bus.h"
#include "err.h"
#include "app_config.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include <stdint.h>
#include <stdbool.h>

/* ── Watchdog forward declaration (defined in app_main.c) ─────────────────── */

extern void wdt_task_alive(WdtTaskId id);

/* ── Module-level state ──────────────────────────────────────────────────── */

QueueHandle_t q_air = NULL;

static Ens160 s_ens;

/* Map the ENS160 STATUS byte to the §9.2.1 quality flag. */
static uint8_t air_quality_from_status(uint8_t status)
{
    if (!ens160_is_operating(status)) {
        /* STATAS clear: no OPMODE running (the STATUS=0x00 all-zeros fault).
         * Data is unusable; the driver re-enters STANDARD to recover. */
        return 3;
    }
    switch (ens160_validity(status)) {
    case ENS160_VALIDITY_NORMAL:  return 0;
    case ENS160_VALIDITY_INVALID: return 3;
    default:                      return 2;   /* warm-up / initial-startup */
    }
}

/* ── air_task_init ───────────────────────────────────────────────────────── */

err_t air_task_init(void)
{
    q_air = xQueueCreate(Q_AIR_DEPTH, sizeof(AirMsg));
    if (q_air == NULL) {
        LOG_E("Failed to create q_air");
        return ERR_NO_MEM;
    }
    return ERR_OK;
}

/* ── air_task ────────────────────────────────────────────────────────────── */

void air_task(void *arg)
{
    (void)arg;

    i2c_bus_lock();
    err_t err = ens160_init(&s_ens, BOARD_I2C_INST, BOARD_ENS160_ADDR);
    i2c_bus_unlock();
    if (err != ERR_OK) {
        LOG_E("ENS160 init failed: %d — air_task halting", err);
        vTaskSuspend(NULL);
    }
    LOG_I("air task started (ENS160 @ 0x%02X)", BOARD_ENS160_ADDR);

    uint32_t dropped = 0;

    for (;;) {
        wdt_task_alive(WDT_TASK_AIR);

        /* Feed env temp/hum into the ENS160 compensation registers (§3.2). */
        float t_c, h_pct;
        if (env_last_reading(&t_c, &h_pct)) {
            i2c_bus_lock();
            (void)ens160_set_compensation(&s_ens, t_c, h_pct);
            i2c_bus_unlock();
        }

        AirMsg msg;
        i2c_bus_lock();
        err = ens160_read_sample(&s_ens, &msg.v);
        i2c_bus_unlock();

        if (err != ERR_OK) {
            LOG_W("ENS160 read failed: %d", err);
            msg.q = 3;
            msg.v.co2_ppm = 0; msg.v.tvoc_ppb = 0; msg.v.aqi = 0;
            msg.v.status = 0;
        } else {
            msg.q = air_quality_from_status(msg.v.status);
            if (msg.q == 3) {
                /* Unusable reading — null out the values so the receiver
                 * never sees a stale or zero AQI as if it were real. */
                msg.v.co2_ppm = 0; msg.v.tvoc_ppb = 0; msg.v.aqi = 0;
            }
            LOG_D("air AQI=%u CO2=%u TVOC=%u q=%u status=0x%02X%s",
                  msg.v.aqi, msg.v.co2_ppm, msg.v.tvoc_ppb, msg.q, msg.v.status,
                  ens160_is_operating(msg.v.status) ? "" : " [NOT RUNNING]");
        }

        if (xQueueSendToBack(q_air, &msg, 0) != pdTRUE) {
            if (++dropped % 10 == 0) {
                LOG_W("q_air full — dropped %lu samples so far",
                      (unsigned long)dropped);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));   /* 1 Hz (§7.1) */
    }
}
