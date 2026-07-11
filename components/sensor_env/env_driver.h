#ifndef ENV_DRIVER_H
#define ENV_DRIVER_H

/* Abstract environmental-sensor driver interface.
 *
 * Two implementations live behind this vtable:
 *   - BMP280 (temp + pressure, NO humidity) — components/sensor_env/bmp280.c
 *   - AHT21  (temp + humidity, NO pressure) — components/sensor_env/aht21.c
 *
 * Selection is by config flag in /cfg/sensors.json's "env" field
 * ("bmp280" | "aht21"), defaulting to bmp280 for back-compat. See env_select.c.
 *
 * The unified EnvSample exposes `pressure_valid` and `humidity_valid` flags;
 * each driver leaves false the field its part can't measure, which
 * env_sample_encode (CLAUDE.md §9.2.3) renders as a literal `null` on the wire:
 * the AHT21 reports pressure_valid=false (`"pres_hpa": null`), the BMP280
 * reports humidity_valid=false (`"hum_pct": null`).  Mirror tiles that don't
 * consume a nulled field are unchanged. */

#include "err.h"
#include "hardware/i2c.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    float temp_c;            /* degrees Celsius (self-heat corrected, see below) */
    float humidity_pct;      /* relative humidity 0–100 %; only valid if humidity_valid */
    float pressure_hpa;      /* hPa; only valid if pressure_valid */
    bool  pressure_valid;
    bool  humidity_valid;
} EnvSample;

/* Board self-heating calibration: the env sensor sits on the 3V3 rail next to
 * the Pico/radar and reads consistently ~5 °C high.  Each env driver subtracts
 * this from temp_c at its vtable read(), so the correction applies to AHT21 and
 * BMP280 alike and to both the production firmware and the bring-up (both call
 * read()).  Tune per enclosure if the offset changes. */
#define ENV_TEMP_SELF_HEAT_OFFSET_C  5.0f

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

    /** Human-readable name for logging: "BMP280" or "AHT21". */
    const char *name;

    /** Opaque context pointer (driver-private state struct). */
    void *ctx;
} env_driver_t;

/* Driver constructors — each returns a pointer to a static env_driver_t. */
env_driver_t *env_bmp280_driver(void);
env_driver_t *env_aht21_driver(void);

/* Read /cfg/sensors.json's "env" key and return the corresponding driver.
 * Defaults to BMP280 if the field is absent. Returns NULL on a value the
 * firmware doesn't recognise (so the caller can fail loudly). */
env_driver_t *env_select_from_config(void);

#endif /* ENV_DRIVER_H */
