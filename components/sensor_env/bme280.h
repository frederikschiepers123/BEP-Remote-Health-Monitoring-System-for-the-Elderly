#ifndef BME280_H
#define BME280_H

/* BME280 temperature, humidity, and pressure sensor driver.
 *
 * Correct read sequence (CLAUDE.md audit §A.1 — fixes stale-read bug):
 *   1. Write 0xF2 (humidity ctrl) to configure oversampling.
 *   2. Write 0xF4 (ctrl_meas) with forced mode — triggers a single measurement.
 *   3. Wait for measurement to complete (~10 ms typical).
 *   4. Read raw ADC registers for temp (0xFA-0xFC), pressure (0xF7-0xF9),
 *      and humidity (0xFD-0xFE).
 *   5. Apply compensation formulas from the BME280 datasheet.
 *
 * IMPORTANT: every call to bme280_read_sample() triggers a fresh measurement.
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
    float humidity_pct;    /* relative humidity 0–100 % */
    float pressure_hpa;    /* hPa (same as mbar) */
} Bme280Sample;

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

    /* Humidity calibration */
    uint8_t  dig_H1;
    int16_t  dig_H2;
    uint8_t  dig_H3;
    int16_t  dig_H4;
    int16_t  dig_H5;
    int8_t   dig_H6;

    /* Fine temperature value shared between compensation functions. */
    int32_t  t_fine;
} Bme280;

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * Initialise the BME280 and read all calibration registers.
 *
 * Verifies chip ID (should be 0x60).  Leaves the sensor in sleep mode
 * (measurement triggered on each call to bme280_read_sample).
 *
 * @param dev   Caller-allocated Bme280 struct; will be filled by this call.
 * @param i2c   Pointer to the I2C instance (e.g. i2c0).
 * @param addr  I2C address (BOARD_BME280_ADDR = 0x76).
 *
 * @return ERR_OK on success, ERR_IO on I2C failure, ERR_NOT_FOUND if chip ID
 *         does not match.
 */
err_t bme280_init(Bme280 *dev, i2c_inst_t *i2c, uint8_t addr);

/**
 * Trigger a measurement, wait for it to complete, and read the result.
 *
 * This function is self-contained:
 *   1. Writes to 0xF2 (humidity ctrl) and 0xF4 (forced mode) to trigger.
 *   2. Delays 10 ms via vTaskDelay.
 *   3. Reads raw ADC data.
 *   4. Applies BME280 compensation formulas.
 *
 * Calling this function without a prior call to bme280_init() is undefined.
 *
 * @return ERR_OK on success, ERR_IO on I2C failure.
 */
err_t bme280_read_sample(Bme280 *dev, Bme280Sample *out);

#endif /* BME280_H */
