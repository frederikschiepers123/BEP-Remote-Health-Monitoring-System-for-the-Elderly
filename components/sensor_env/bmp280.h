#ifndef BMP280_H
#define BMP280_H

/* BMP280 temperature and pressure sensor driver.
 *
 * The BMP280 is the temperature + pressure subset of the BME280 — it has NO
 * humidity sensor (chip ID 0x58, vs 0x60 for the BME280).  The board's env
 * footprint is populated with a BMP280, so humidity is unavailable on this
 * path: the env_driver_t adapter leaves EnvSample.humidity_valid = false,
 * which env_sample_encode renders as `"hum_pct": null` on the wire
 * (CLAUDE.md §9.2.2/§9.2.3) — the same mechanism the AHT21 uses for pressure.
 *
 * Correct read sequence (CLAUDE.md audit §A.1 — fixes stale-read bug):
 *   1. Write 0xF4 (ctrl_meas) with forced mode — triggers a single measurement.
 *   2. Wait for measurement to complete (~10 ms typical).
 *   3. Read raw ADC registers for pressure (0xF7-0xF9) and temp (0xFA-0xFC).
 *   4. Apply compensation formulas from the BMP280 datasheet.
 *
 * IMPORTANT: every call to bmp280_read_sample() triggers a fresh measurement.
 * There is no "previous value" fallback — errors return ERR_IO so callers
 * can set quality flag q=3 (invalid) in the sample payload.
 *
 * All I/O goes through pico-sdk hardware_i2c.  No direct register access
 * from outside this driver.
 */

#include "err.h"
#include "hardware/i2c.h"
#include <stdint.h>

/* ── Output sample ──────────────────────────────────────────────────────── */

typedef struct {
    float temp_c;          /* degrees Celsius */
    float pressure_hpa;    /* hPa (same as mbar) */
} Bmp280Sample;

/* ── Device state (holds calibration coefficients) ──────────────────────── */

typedef struct {
    i2c_inst_t *i2c;
    uint8_t     addr;       /* 0x76 or 0x77 */

    /* Temperature calibration */
    uint16_t dig_T1;
    int16_t  dig_T2;
    int16_t  dig_T3;

    /* Pressure calibration */
    uint16_t dig_P1;
    int16_t  dig_P2;
    int16_t  dig_P3;
    int16_t  dig_P4;
    int16_t  dig_P5;
    int16_t  dig_P6;
    int16_t  dig_P7;
    int16_t  dig_P8;
    int16_t  dig_P9;

    /* Fine temperature value shared between compensation functions. */
    int32_t  t_fine;
} Bmp280;

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * Initialise the BMP280 and read all calibration registers.
 *
 * Verifies chip ID (should be 0x58).  Leaves the sensor in sleep mode
 * (measurement triggered on each call to bmp280_read_sample).
 *
 * @param dev   Caller-allocated Bmp280 struct; will be filled by this call.
 * @param i2c   Pointer to the I2C instance (e.g. i2c0).
 * @param addr  I2C address (BOARD_BMP280_ADDR = 0x76).
 *
 * @return ERR_OK on success, ERR_IO on I2C failure, ERR_NOT_FOUND if chip ID
 *         does not match.
 */
err_t bmp280_init(Bmp280 *dev, i2c_inst_t *i2c, uint8_t addr);

/**
 * Trigger a measurement, wait for it to complete, and read the result.
 *
 * This function is self-contained:
 *   1. Writes to 0xF4 (forced mode) to trigger a single measurement.
 *   2. Delays 10 ms via vTaskDelay.
 *   3. Reads raw ADC data.
 *   4. Applies BMP280 compensation formulas.
 *
 * Calling this function without a prior call to bmp280_init() is undefined.
 *
 * @return ERR_OK on success, ERR_IO on I2C failure.
 */
err_t bmp280_read_sample(Bmp280 *dev, Bmp280Sample *out);

#endif /* BMP280_H */
