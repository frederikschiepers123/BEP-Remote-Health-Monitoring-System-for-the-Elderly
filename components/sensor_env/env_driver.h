#ifndef ENV_DRIVER_H
#define ENV_DRIVER_H

/* Abstract environmental-sensor driver interface.
 *
 * Two implementations live behind this vtable:
 *   - BME280 (temp + humidity + pressure)  — components/sensor_env/bme280.c
 *   - AHT21  (temp + humidity, NO pressure) — components/sensor_env/aht21.c
 *
 * Selection is by config flag in /cfg/sensors.json's "env" field
 * ("bme280" | "aht21"), defaulting to bme280 for back-compat. See env_select.c.
 *
 * The unified EnvSample exposes a `pressure_valid` flag; the AHT21 driver
 * leaves it false, which env_sample_encode (CLAUDE.md §9.2.3) renders as
 * `"pres_hpa": null` on the wire.  Mirror tiles that don't consume pres_hpa
 * are unchanged. */

#include "err.h"
#include "hardware/i2c.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    float temp_c;            /* degrees Celsius */
    float humidity_pct;      /* relative humidity 0–100 % */
    float pressure_hpa;      /* hPa; only valid if pressure_valid */
    bool  pressure_valid;
} EnvSample;

typedef struct {
    /**
     * Initialise the driver on a given I²C bus + address.
     * Returns ERR_OK on success.
     */
    err_t (*init)(void *ctx, i2c_inst_t *i2c, uint8_t addr);

    /**
     * Trigger a measurement, wait, decode, fill `out`. Self-contained.
     * Returns ERR_OK on success, ERR_IO on bus error, ERR_BUSY if the
     * sensor reports it didn't finish the conversion in time.
     */
    err_t (*read_sample)(void *ctx, EnvSample *out);

    /** Human-readable name for logging: "BME280" or "AHT21". */
    const char *name;

    /** Opaque context pointer (driver-private state struct). */
    void *ctx;
} env_driver_t;

/* Driver constructors — each returns a pointer to a static env_driver_t. */
env_driver_t *env_bme280_driver(void);
env_driver_t *env_aht21_driver(void);

/* Read /cfg/sensors.json's "env" key and return the corresponding driver.
 * Defaults to BME280 if the field is absent. Returns NULL on a value the
 * firmware doesn't recognise (so the caller can fail loudly). */
env_driver_t *env_select_from_config(void);

#endif /* ENV_DRIVER_H */
