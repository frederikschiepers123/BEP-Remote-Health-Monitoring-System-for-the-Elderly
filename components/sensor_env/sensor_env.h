#ifndef SENSOR_ENV_H
#define SENSOR_ENV_H

/* Environmental sensor task — BME280 temperature, humidity, pressure.
 *
 * Publishes EnvSample to q_env at 1 Hz.  The transport_task drains q_env
 * and forwards samples via MQTT.
 *
 * Queue: q_env, depth Q_ENV_DEPTH (from app_config.h).
 * Task entry: env_task (created by app_main.c).
 */

#include "bme280.h"
#include "err.h"

#include "FreeRTOS.h"
#include "queue.h"

#include <stdint.h>

/* ── Sample type ─────────────────────────────────────────────────────────── */

typedef struct {
    uint64_t    ts_us;          /* monotonic microseconds since boot */
    uint32_t    seq;            /* per-topic monotonic counter */
    Bme280Sample v;             /* actual sensor values */
    uint8_t     q;              /* 0=ok, 3=invalid (ERR_IO from driver) */
} EnvSample;

/* ── Shared queue (produced here, consumed by transport_task) ────────────── */

extern QueueHandle_t q_env;

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * Initialise the env component.  Creates q_env.
 * Must be called from app_main before the scheduler starts.
 *
 * @return ERR_OK on success, ERR_NO_MEM if queue creation fails.
 */
err_t env_task_init(void);

/**
 * FreeRTOS task entry point.  Never returns.
 * Pass NULL as arg; it is unused.
 */
void env_task(void *arg);

#endif /* SENSOR_ENV_H */
