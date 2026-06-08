#ifndef SENSOR_AIR_H
#define SENSOR_AIR_H

/* Air-quality sensor task — ScioSense ENS160 (eCO2 + TVOC + UBA AQI).
 *
 * Publishes AirMsg to q_air at 1 Hz (CLAUDE.md §7.1).  The transport_task
 * drains q_air, stamps the §9.2.1 envelope, and forwards via MQTT.
 *
 * Each cycle the task feeds the most recent env temp/hum (env_last_reading())
 * into the ENS160 TEMP_IN / RH_IN compensation registers (§3.2) before
 * reading, so the gas-sensor maths is corrected for ambient conditions.
 *
 * Quality mapping (§9.2.2): the ENS160 needs a warm-up; readings during it
 * are flagged degraded, and an unusable STATUS=0x00 (chip not running) is
 * flagged invalid — the driver re-enters STANDARD mode to recover.
 *
 * I²C0 is shared (§7.2): every transaction takes the i2c_bus lock. */

#include "ens160.h"   /* Ens160Sample */
#include "err.h"

#include "FreeRTOS.h"
#include "queue.h"

#include <stdint.h>

/* ── Queue message ──────────────────────────────────────────────────────────
 * The driver fills the Ens160Sample v-body (incl. raw status); air_task maps
 * the status to the §9.2.1 quality flag.  ts_us/seq stamped by the transport. */

typedef struct {
    Ens160Sample v;         /* co2_ppm / tvoc_ppb / aqi / raw status */
    uint8_t      q;         /* 0=ok, 2=degraded (warm-up), 3=invalid */
} AirMsg;

/* ── Shared queue ────────────────────────────────────────────────────────── */

extern QueueHandle_t q_air;

/* ── API ─────────────────────────────────────────────────────────────────── */

/** Create q_air.  Call from app_main before the scheduler starts. */
err_t air_task_init(void);

/** FreeRTOS task entry point.  Never returns. */
void air_task(void *arg);

#endif /* SENSOR_AIR_H */
