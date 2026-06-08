#define LOG_TAG "ENV"
#include "log.h"

#include "sensor_env.h"
#include "env_driver.h"
#include "board_pico2wh.h"
#include "i2c_bus.h"
#include "err.h"
#include "app_config.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "pico/time.h"

#include <stdint.h>
#include <stdbool.h>

/* ── Watchdog forward declaration (defined in app_main.c) ─────────────────── */

extern void wdt_task_alive(WdtTaskId id);

/* ── Module-level state ──────────────────────────────────────────────────── */

QueueHandle_t q_env = NULL;

static env_driver_t *s_drv = NULL;

/* Last good temp/hum, published for ENS160 compensation via env_last_reading().
 * Single-writer (env_task) / single-reader (air_task); volatile, no lock. */
static volatile float s_last_temp_c = 0.0f;
static volatile float s_last_hum_pct = 0.0f;
static volatile bool  s_last_valid = false;

/* ── env_task_init ───────────────────────────────────────────────────────── */

err_t env_task_init(void)
{
    q_env = xQueueCreate(Q_ENV_DEPTH, sizeof(EnvMsg));
    if (q_env == NULL) {
        LOG_E("Failed to create q_env");
        return ERR_NO_MEM;
    }
    return ERR_OK;
}

bool env_last_reading(float *temp_c, float *hum_pct)
{
    if (!s_last_valid) {
        return false;
    }
    if (temp_c)  { *temp_c  = s_last_temp_c; }
    if (hum_pct) { *hum_pct = s_last_hum_pct; }
    return true;
}

/* ── env_task ────────────────────────────────────────────────────────────── */

void env_task(void *arg)
{
    (void)arg;

    /* Select AHT21/BME280 from /cfg/sensors.json (defaults to BME280). */
    s_drv = env_select_from_config();
    if (s_drv == NULL) {
        LOG_E("env_select_from_config() returned NULL — env_task halting");
        vTaskSuspend(NULL);
    }

    /* Each driver knows its own fixed/strapped address. */
    uint8_t addr = (s_drv == env_aht21_driver())
                       ? BOARD_AHT21_ADDR : BOARD_BME280_ADDR;

    i2c_bus_lock();
    err_t err = s_drv->init(s_drv->ctx, BOARD_I2C_INST, addr);
    i2c_bus_unlock();
    if (err != ERR_OK) {
        LOG_E("env driver %s init failed: %d — env_task halting",
              s_drv->name, err);
        vTaskSuspend(NULL);
    }
    LOG_I("env task started, driver=%s addr=0x%02X", s_drv->name, addr);

    uint32_t dropped = 0;

    for (;;) {
        wdt_task_alive(WDT_TASK_ENV);

        EnvMsg msg;
        i2c_bus_lock();
        err = s_drv->read_sample(s_drv->ctx, &msg.v);
        i2c_bus_unlock();

        if (err == ERR_OK) {
            msg.q = 0;
            s_last_temp_c  = msg.v.temp_c;
            s_last_hum_pct = msg.v.humidity_pct;
            s_last_valid   = true;
            LOG_D("env %s T=%.2fC H=%.2f%%%s", s_drv->name,
                  (double)msg.v.temp_c, (double)msg.v.humidity_pct,
                  msg.v.pressure_valid ? "" : " (no pres)");
        } else {
            LOG_W("env %s read failed: %d", s_drv->name, err);
            msg.q = 3;   /* invalid — receiver must not use v */
            msg.v.temp_c = msg.v.humidity_pct = msg.v.pressure_hpa = 0.0f;
            msg.v.pressure_valid = false;
        }

        if (xQueueSendToBack(q_env, &msg, 0) != pdTRUE) {
            if (++dropped % 10 == 0) {
                LOG_W("q_env full — dropped %lu samples so far",
                      (unsigned long)dropped);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));   /* 1 Hz (§7.1) */
    }
}
