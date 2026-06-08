#define LOG_TAG "ENV"
#include "log.h"

#include "sensor_env.h"
#include "bme280.h"
#include "board_pico2wh.h"
#include "err.h"
#include "app_config.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "pico/time.h"

#include <stdint.h>
#include <stdbool.h>

/* ── Watchdog forward declaration ────────────────────────────────────────── */

/* Defined in app_main.c */
extern void wdt_task_alive(WdtTaskId id);

/* ── Module-level state ──────────────────────────────────────────────────── */

QueueHandle_t q_env = NULL;

static Bme280 s_bme;

/* ── env_task_init ───────────────────────────────────────────────────────── */

err_t env_task_init(void)
{
    q_env = xQueueCreate(Q_ENV_DEPTH, sizeof(EnvSample));
    if (q_env == NULL) {
        LOG_E("Failed to create q_env");
        return ERR_NO_MEM;
    }
    return ERR_OK;
}

/* ── env_task ────────────────────────────────────────────────────────────── */

void env_task(void *arg)
{
    (void)arg;

    /* Initialise BME280 using board constants (no hardcoded addresses). */
    err_t err = bme280_init(&s_bme, BOARD_I2C_INST, BOARD_BME280_ADDR);
    if (err != ERR_OK) {
        LOG_E("BME280 init failed: %d — env_task halting", err);
        /* If the sensor is absent we do not want to block other tasks.
         * Suspend rather than spin to avoid watchdog thrash. */
        vTaskSuspend(NULL);
        /* NOT REACHED */
    }

    uint32_t seq      = 0;
    uint32_t dropped  = 0;

    for (;;) {
        /* Notify watchdog we are alive. */
        wdt_task_alive(WDT_TASK_ENV);

        EnvSample sample;
        sample.seq   = seq++;
        sample.ts_us = (uint64_t)to_us_since_boot(get_absolute_time());

        err = bme280_read_sample(&s_bme, &sample.v);
        if (err == ERR_OK) {
            sample.q = 0;
        } else {
            LOG_W("BME280 read failed: %d", err);
            sample.q = 3;   /* invalid — caller must not use sample.v */
        }

        /* Non-blocking enqueue: if the queue is full, drop and count. */
        if (xQueueSendToBack(q_env, &sample, 0) != pdTRUE) {
            dropped++;
            if (dropped % 10 == 0) {
                LOG_W("q_env full — dropped %lu samples so far",
                      (unsigned long)dropped);
            }
        }

        /* 1 Hz cadence */
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
