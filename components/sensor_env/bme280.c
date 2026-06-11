#define LOG_TAG "BME280"
#include "log.h"

#include "bme280.h"
#include "err.h"

#include "hardware/i2c.h"

/* FreeRTOS is used for vTaskDelay only; on host-test builds it is stubbed. */
#ifndef HOST_TEST
#  include "FreeRTOS.h"
#  include "task.h"
#endif

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ── BME280 register addresses ──────────────────────────────────────────── */
#define BME280_REG_CHIP_ID      0xD0U
#define BME280_REG_RESET        0xE0U
#define BME280_REG_CTRL_HUM     0xF2U
#define BME280_REG_STATUS       0xF3U
#define BME280_REG_CTRL_MEAS    0xF4U
#define BME280_REG_CONFIG       0xF5U
#define BME280_REG_PRESS_MSB    0xF7U
#define BME280_REG_TEMP_MSB     0xFAU
#define BME280_REG_HUM_MSB      0xFDU

/* Calibration base registers */
#define BME280_REG_CAL_T1       0x88U   /* T1 .. P9 in 0x88..0x9F */
#define BME280_REG_CAL_H1       0xA1U
#define BME280_REG_CAL_H2_LSB   0xE1U   /* H2..H6 in 0xE1..0xE7 */

#define BME280_CHIP_ID          0x60U

/* Forced mode: temp×1, pressure×1, humid×1 (osrs = 001) */
#define BME280_CTRL_HUM_OS1     0x01U   /* humidity oversampling ×1 */
#define BME280_CTRL_MEAS_FORCED 0x25U   /* temp×1, pressure×1, forced = 0b00100101 */

/* ── Low-level I2C helpers ──────────────────────────────────────────────── */

static err_t write_reg(Bme280 *dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    int ret = i2c_write_blocking(dev->i2c, dev->addr, buf, 2, false);
    if (ret < 0) {
        LOG_E("I2C write reg 0x%02X failed: %d", reg, ret);
        return ERR_IO;
    }
    return ERR_OK;
}

static err_t read_regs(Bme280 *dev, uint8_t reg, uint8_t *buf, size_t len)
{
    int ret = i2c_write_blocking(dev->i2c, dev->addr, &reg, 1, true);
    if (ret < 0) {
        LOG_E("I2C write addr failed: %d", ret);
        return ERR_IO;
    }
    ret = i2c_read_blocking(dev->i2c, dev->addr, buf, len, false);
    if (ret < 0) {
        LOG_E("I2C read reg 0x%02X len %u failed: %d", reg, (unsigned)len, ret);
        return ERR_IO;
    }
    return ERR_OK;
}

static err_t read_reg(Bme280 *dev, uint8_t reg, uint8_t *out)
{
    return read_regs(dev, reg, out, 1);
}

/* ── Calibration parsing helpers ────────────────────────────────────────── */

static uint16_t u16le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int16_t s16le(const uint8_t *p)
{
    return (int16_t)u16le(p);
}

/* ── bme280_init ─────────────────────────────────────────────────────────── */

err_t bme280_init(Bme280 *dev, i2c_inst_t *i2c, uint8_t addr)
{
    if (!dev || !i2c) {
        return ERR_INVALID_ARG;
    }
    memset(dev, 0, sizeof(Bme280));
    dev->i2c  = i2c;
    dev->addr = addr;

    /* Verify chip ID */
    uint8_t chip_id = 0;
    err_t err = read_reg(dev, BME280_REG_CHIP_ID, &chip_id);
    if (err != ERR_OK) {
        return err;
    }
    if (chip_id != BME280_CHIP_ID) {
        LOG_E("BME280 chip_id mismatch: got 0x%02X expected 0x%02X",
              chip_id, BME280_CHIP_ID);
        return ERR_NOT_FOUND;
    }

    /* Soft reset */
    err = write_reg(dev, BME280_REG_RESET, 0xB6U);
    if (err != ERR_OK) { return err; }

    /* Wait for NVM copy to complete (status bit 0) */
#ifndef HOST_TEST
    vTaskDelay(pdMS_TO_TICKS(10));
#endif

    /* Read temperature + pressure calibration registers (0x88..0x9F = 24 bytes) */
    uint8_t cal_tp[24];
    err = read_regs(dev, BME280_REG_CAL_T1, cal_tp, sizeof(cal_tp));
    if (err != ERR_OK) { return err; }

    dev->dig_T1 = u16le(&cal_tp[0]);
    dev->dig_T2 = s16le(&cal_tp[2]);
    dev->dig_T3 = s16le(&cal_tp[4]);

    dev->dig_P1 = u16le(&cal_tp[6]);
    dev->dig_P2 = s16le(&cal_tp[8]);
    dev->dig_P3 = s16le(&cal_tp[10]);
    dev->dig_P4 = s16le(&cal_tp[12]);
    dev->dig_P5 = s16le(&cal_tp[14]);
    dev->dig_P6 = s16le(&cal_tp[16]);
    dev->dig_P7 = s16le(&cal_tp[18]);
    dev->dig_P8 = s16le(&cal_tp[20]);
    dev->dig_P9 = s16le(&cal_tp[22]);

    /* Read dig_H1 (single byte at 0xA1) */
    err = read_reg(dev, BME280_REG_CAL_H1, &dev->dig_H1);
    if (err != ERR_OK) { return err; }

    /* Read humidity calibration 0xE1..0xE7 (7 bytes) */
    uint8_t cal_h[7];
    err = read_regs(dev, BME280_REG_CAL_H2_LSB, cal_h, sizeof(cal_h));
    if (err != ERR_OK) { return err; }

    /* dig_H2: 0xE1 (LSB), 0xE2 (MSB) */
    dev->dig_H2 = s16le(&cal_h[0]);

    /* dig_H3: 0xE3 */
    dev->dig_H3 = cal_h[2];

    /*
     * dig_H4: bits 11:4 from 0xE4, bits 3:0 from lower nibble of 0xE5
     * dig_H5: bits 11:4 from 0xE6, bits 3:0 from upper nibble of 0xE5
     */
    dev->dig_H4 = (int16_t)(((int16_t)cal_h[3] << 4) |
                             ((int16_t)(cal_h[4] & 0x0FU)));
    dev->dig_H5 = (int16_t)(((int16_t)cal_h[5] << 4) |
                             ((int16_t)(cal_h[4] >> 4)));

    /* dig_H6: 0xE7 (signed) */
    dev->dig_H6 = (int8_t)cal_h[6];

    LOG_I("BME280 init OK at 0x%02X", addr);
    return ERR_OK;
}

/* ── Compensation formulas (integer math — BME280 datasheet §4.2.3) ──────── */

/* Returns temperature in DegC, resolution 0.01 DegC.
 * Output value of "5123" equals 51.23 DegC.
 * Sets t_fine for use by pressure and humidity compensation. */
static int32_t compensate_temp(Bme280 *dev, int32_t adc_T)
{
    int32_t var1, var2, T;
    var1 = ((((adc_T >> 3) - ((int32_t)dev->dig_T1 << 1))) *
             ((int32_t)dev->dig_T2)) >> 11;
    var2 = (((((adc_T >> 4) - ((int32_t)dev->dig_T1)) *
              ((adc_T >> 4) - ((int32_t)dev->dig_T1))) >> 12) *
             ((int32_t)dev->dig_T3)) >> 14;
    dev->t_fine = var1 + var2;
    T = (dev->t_fine * 5 + 128) >> 8;
    return T;
}

/* Returns pressure in Pa as Q24.8 (24-bit integer part, 8-bit fraction).
 * Output value of "24674867" equals 24674867/256 = 96386.2 Pa. */
static uint32_t compensate_pressure(Bme280 *dev, int32_t adc_P)
{
    int64_t var1, var2, p;
    var1 = ((int64_t)dev->t_fine) - 128000LL;
    var2 = var1 * var1 * (int64_t)dev->dig_P6;
    var2 = var2 + ((var1 * (int64_t)dev->dig_P5) << 17);
    var2 = var2 + (((int64_t)dev->dig_P4) << 35);
    var1 = ((var1 * var1 * (int64_t)dev->dig_P3) >> 8) +
           ((var1 * (int64_t)dev->dig_P2) << 12);
    var1 = (((((int64_t)1) << 47) + var1)) *
           ((int64_t)dev->dig_P1) >> 33;
    if (var1 == 0) {
        return 0;   /* avoid divide by zero */
    }
    p = 1048576LL - (int64_t)adc_P;
    p = (((p << 31) - var2) * 3125LL) / var1;
    var1 = (((int64_t)dev->dig_P9) * (p >> 13) * (p >> 13)) >> 25;
    var2 = (((int64_t)dev->dig_P8) * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (((int64_t)dev->dig_P7) << 4);
    return (uint32_t)p;
}

/* Returns humidity in %rH as Q22.10 (22-bit integer, 10-bit fraction).
 * Output value of "47445" equals 47445/1024 = 46.333 %rH. */
static uint32_t compensate_humidity(Bme280 *dev, int32_t adc_H)
{
    int32_t v_x1_u32r;
    v_x1_u32r = (dev->t_fine - ((int32_t)76800));
    v_x1_u32r = (((((adc_H << 14) - (((int32_t)dev->dig_H4) << 20) -
                    (((int32_t)dev->dig_H5) * v_x1_u32r)) +
                   ((int32_t)16384)) >> 15) *
                 (((((((v_x1_u32r * ((int32_t)dev->dig_H6)) >> 10) *
                      (((v_x1_u32r * ((int32_t)dev->dig_H3)) >> 11) +
                       ((int32_t)32768))) >> 10) +
                    ((int32_t)2097152)) *
                   ((int32_t)dev->dig_H2) + 8192) >> 14));
    v_x1_u32r = (v_x1_u32r -
                 (((((v_x1_u32r >> 15) * (v_x1_u32r >> 15)) >> 7) *
                   ((int32_t)dev->dig_H1)) >> 4));
    if (v_x1_u32r < 0) { v_x1_u32r = 0; }
    if (v_x1_u32r > 419430400) { v_x1_u32r = 419430400; }
    return (uint32_t)(v_x1_u32r >> 12);
}

/* ── bme280_read_sample ──────────────────────────────────────────────────── */

err_t bme280_read_sample(Bme280 *dev, Bme280Sample *out)
{
    if (!dev || !out) {
        return ERR_INVALID_ARG;
    }

    /*
     * Step 1: Configure humidity oversampling ×1 (must be set before ctrl_meas).
     * Step 2: Trigger forced-mode measurement (temp×1, pressure×1).
     * This guarantees a fresh reading every call — no stale-read possible.
     */
    err_t err = write_reg(dev, BME280_REG_CTRL_HUM, BME280_CTRL_HUM_OS1);
    if (err != ERR_OK) { return err; }

    err = write_reg(dev, BME280_REG_CTRL_MEAS, BME280_CTRL_MEAS_FORCED);
    if (err != ERR_OK) { return err; }

    /* Step 3: Wait for measurement to complete (~10 ms for ×1 oversampling). */
#ifndef HOST_TEST
    vTaskDelay(pdMS_TO_TICKS(10));
#endif

    /*
     * Step 4: Read 8 contiguous bytes from 0xF7:
     *   0xF7, 0xF8, 0xF9 — pressure (20-bit)
     *   0xFA, 0xFB, 0xFC — temperature (20-bit)
     *   0xFD, 0xFE       — humidity (16-bit)
     */
    uint8_t raw[8];
    err = read_regs(dev, BME280_REG_PRESS_MSB, raw, sizeof(raw));
    if (err != ERR_OK) { return err; }

    /* Decode raw ADC values (20-bit for temp/pressure, 16-bit for humidity) */
    int32_t adc_P = (int32_t)(((uint32_t)raw[0] << 12) |
                               ((uint32_t)raw[1] <<  4) |
                               ((uint32_t)raw[2] >>  4));
    int32_t adc_T = (int32_t)(((uint32_t)raw[3] << 12) |
                               ((uint32_t)raw[4] <<  4) |
                               ((uint32_t)raw[5] >>  4));
    int32_t adc_H = (int32_t)(((uint32_t)raw[6] << 8) |
                               (uint32_t)raw[7]);

    /* Step 5: Apply compensation formulas. */

    /* Temperature first (sets t_fine used by pressure and humidity). */
    int32_t temp_raw = compensate_temp(dev, adc_T);
    out->temp_c = (float)temp_raw / 100.0f;

    /* Pressure: compensate_pressure returns Q24.8 (unit: Pa*256). */
    uint32_t press_q8 = compensate_pressure(dev, adc_P);
    out->pressure_hpa = (float)press_q8 / (256.0f * 100.0f);  /* Pa → hPa */

    /* Humidity: compensate_humidity returns Q22.10 (%rH * 1024). */
    uint32_t hum_q10 = compensate_humidity(dev, adc_H);
    out->humidity_pct = (float)hum_q10 / 1024.0f;

    LOG_D("BME280 sample: T=%.2f C, H=%.2f %%, P=%.2f hPa",
          (double)out->temp_c,
          (double)out->humidity_pct,
          (double)out->pressure_hpa);

    return ERR_OK;
}

/* ── env_driver_t v-table adapter ──────────────────────────────────────────
 * Wraps the Bme280 API in the shared env_driver_t interface so callers can
 * select between BME280 and AHT21 at /cfg/sensors.json provisioning time
 * without rebuilding firmware. */
#include "env_driver.h"

static Bme280 s_bme280_ctx;

static err_t bme280_drv_init(void *ctx, i2c_inst_t *i2c, uint8_t addr) {
    return bme280_init((Bme280 *)ctx, i2c, addr);
}

static err_t bme280_drv_read(void *ctx, EnvSample *out) {
    Bme280Sample s;
    err_t e = bme280_read_sample((Bme280 *)ctx, &s);
    if (e != ERR_OK) return e;
    out->temp_c         = s.temp_c - ENV_TEMP_SELF_HEAT_OFFSET_C;
    out->humidity_pct   = s.humidity_pct;
    out->pressure_hpa   = s.pressure_hpa;
    out->pressure_valid = true;
    return ERR_OK;
}

static env_driver_t s_bme280_driver = {
    .init        = bme280_drv_init,
    .read_sample = bme280_drv_read,
    .name        = "BME280",
    .ctx         = &s_bme280_ctx,
};

env_driver_t *env_bme280_driver(void) { return &s_bme280_driver; }
