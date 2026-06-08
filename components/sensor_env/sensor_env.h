#ifndef SENSOR_ENV_H
#define SENSOR_ENV_H

/* Environmental sensor task — temperature + humidity (+ pressure on BME280).
 *
 * The concrete sensor (AHT21 or BME280) is selected at runtime from
 * /cfg/sensors.json via the env_driver_t vtable (env_select.c); env_task is
 * unaware of which part is populated.  AHT21 leaves pressure_valid=false,
 * which the transport encodes as `"pres_hpa": null` (§9.2.2).
 *
 * Publishes EnvMsg to q_env at 1 Hz.  The transport_task drains q_env, stamps
 * the §9.2.1 envelope (ts_us/seq), and forwards the sample via MQTT.
 *
 * Queue: q_env, depth Q_ENV_DEPTH (from app_config.h).
 * Task entry: env_task (created by app_main.c).
 *
 * I²C0 is shared (CLAUDE.md §3.2/§7.2): every read takes the i2c_bus lock. */

#include "env_driver.h"   /* EnvSample (temp/hum/pres/pres_valid) */
#include "err.h"

#include "FreeRTOS.h"
#include "queue.h"

#include <stdint.h>
#include <stdbool.h>

/* ── Queue message ───────────────────────────────────────────────────────────
 * The driver fills the EnvSample v-body; env_task adds the quality flag.
 * ts_us/seq are stamped by the transport at publish time (§9.2.1). */

typedef struct {
    EnvSample v;            /* driver sample: temp/hum/pres + pressure_valid */
    uint8_t   q;            /* 0=ok, 3=invalid (driver ERR_*) */
} EnvMsg;

/* ── Shared queue (produced here, consumed by transport_task) ────────────── */

extern QueueHandle_t q_env;

/* ── API ─────────────────────────────────────────────────────────────────── */

/** Create q_env.  Call from app_main before the scheduler starts. */
err_t env_task_init(void);

/** FreeRTOS task entry point.  Never returns.  Pass NULL as arg. */
void env_task(void *arg);

/**
 * Most-recent valid temperature/humidity, for ENS160 compensation
 * (CLAUDE.md §3.2).  air_task calls this each cycle and writes the values to
 * the ENS160 TEMP_IN/RH_IN registers.
 *
 * @return true and fills *temp_c / *hum_pct if env_task has produced at least
 *         one good reading; false otherwise (caller skips compensation).
 *
 * Lock-free single-writer (env_task) / single-reader: a torn read yields at
 * worst a one-cycle-stale value, irrelevant for gas-sensor compensation.
 */
bool env_last_reading(float *temp_c, float *hum_pct);

#endif /* SENSOR_ENV_H */
