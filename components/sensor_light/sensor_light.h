#ifndef SENSOR_LIGHT_H
#define SENSOR_LIGHT_H

/* Ambient-light sensor task.
 *
 * The concrete sensor (BH1750 over I²C on the advanced module, or a GL5516 LDR
 * on ADC0 on the generic module) is selected at runtime from /cfg/sensors.json
 * via the light_driver_t vtable (light_select.c); light_task is unaware of
 * which variant is populated.  See ADR-0001.
 *
 * Publishes LightMsg to q_light at 0.2 Hz (CLAUDE.md §7.1 — ambient lux barely
 * moves within a second, and the mirror tile needs no fast refresh).  The
 * transport_task drains q_light, stamps the §9.2.1 envelope, and forwards
 * via MQTT.
 *
 * The BH1750 path touches I²C0 (shared bus): every read takes the i2c_bus
 * lock.  The GL5516 path reads ADC0 only, but takes the lock anyway for a
 * uniform call shape — ADC reads are sub-microsecond. */

#include "light_driver.h"   /* LightSample { float lux; } */
#include "err.h"

#include "FreeRTOS.h"
#include "queue.h"

#include <stdint.h>

/* ── Queue message ──────────────────────────────────────────────────────────
 * The driver fills the LightSample v-body; light_task adds the quality flag.
 * ts_us/seq are stamped by the transport at publish time (§9.2.1). */

typedef struct {
    LightSample v;          /* driver sample: lux */
    uint8_t     q;          /* 0=ok, 2=degraded (saturated), 3=invalid */
} LightMsg;

/* ── Shared queue ────────────────────────────────────────────────────────── */

extern QueueHandle_t q_light;

/* ── API ─────────────────────────────────────────────────────────────────── */

/** Create q_light.  Call from app_main before the scheduler starts. */
err_t light_task_init(void);

/** FreeRTOS task entry point.  Never returns. */
void light_task(void *arg);

#endif /* SENSOR_LIGHT_H */
