#define LOG_TAG "BMP280"
#include "log.h"

#include "bmp280.h"
#include "err.h"
#include "sensor_cal.h"

#include "hardware/i2c.h"

/* FreeRTOS is used for vTaskDelay only; on host-test builds it is stubbed. */
#ifndef HOST_TEST
#  include "FreeRTOS.h"
#  include "task.h"
#endif

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ── BMP280 register addresses ──────────────────────────────────────────── */
#define BMP280_REG_CHIP_ID      0xD0U
#define BMP280_REG_RESET        0xE0U
#define BMP280_REG_STATUS       0xF3U
#define BMP280_REG_CTRL_MEAS    0xF4U
#define BMP280_REG_CONFIG       0xF5U
#define BMP280_REG_PRESS_MSB    0xF7U
#define BMP280_REG_TEMP_MSB     0xFAU

/* Calibration base register (T1 .. P9 in 0x88..0x9F). The BMP280 has no
 * humidity calibration block — that is the only register-map difference from
 * the BME280. */
#define BMP280_REG_CAL_T1       0x88U

#define BMP280_CHIP_ID          0x58U   /* BMP280 (BME280 would read 0x60) */

/* Forced mode: temp×1, pressure×1 (osrs = 001), mode = forced. */
#define BMP280_CTRL_MEAS_FORCED 0x25U   /* temp×1, pressure×1, forced = 0b00100101 */

/* ── Low-level I2C helpers ──────────────────────────────────────────────── */

static err_t write_reg(Bmp280 *dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    int ret = i2c_write_blocking(dev->i2c, dev->addr, buf, 2, false);
    if (ret < 0) {
        LOG_E("I2C write reg 0x%02X failed: %d", reg, ret);
        return ERR_IO;
    }
    return ERR_OK;
}

static err_t read_regs(Bmp280 *dev, uint8_t reg, uint8_t *buf, size_t len)
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

static err_t read_reg(Bmp280 *dev, uint8_t reg, uint8_t *out)
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

/* ── bmp280_init ─────────────────────────────────────────────────────────── */

err_t bmp280_init(Bmp280 *dev, i2c_inst_t *i2c, uint8_t addr)
{
    if (!dev || !i2c) {
        return ERR_INVALID_ARG;
    }
    memset(dev, 0, sizeof(Bmp280));
    dev->i2c  = i2c;
    dev->addr = addr;

    /* Verify chip ID */
    uint8_t chip_id = 0;
    err_t err = read_reg(dev, BMP280_REG_CHIP_ID, &chip_id);
    if (err != ERR_OK) {
        return err;
    }
    if (chip_id != BMP280_CHIP_ID) {
        LOG_E("BMP280 chip_id mismatch: got 0x%02X expected 0x%02X",
              chip_id, BMP280_CHIP_ID);
        return ERR_NOT_FOUND;
    }

    /* Soft reset */
    err = write_reg(dev, BMP280_REG_RESET, 0xB6U);
    if (err != ERR_OK) { return err; }

    /* Wait for NVM copy to complete (status bit 0) */
#ifndef HOST_TEST
    vTaskDelay(pdMS_TO_TICKS(10));
#endif

    /* Read temperature + pressure calibration registers (0x88..0x9F = 24 bytes) */
    uint8_t cal_tp[24];
    err = read_regs(dev, BMP280_REG_CAL_T1, cal_tp, sizeof(cal_tp));
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

    LOG_I("BMP280 init OK at 0x%02X", addr);
    return ERR_OK;
}

/* ── Compensation formulas (integer math — BMP280 datasheet §3.11.3) ──────── */

/* Returns temperature in DegC, resolution 0.01 DegC.
 * Output value of "5123" equals 51.23 DegC.
 * Sets t_fine for use by pressure compensation. */
static int32_t compensate_temp(Bmp280 *dev, int32_t adc_T)
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
static uint32_t compensate_pressure(Bmp280 *dev, int32_t adc_P)
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

/* ── bmp280_read_sample ──────────────────────────────────────────────────── */

err_t bmp280_read_sample(Bmp280 *dev, Bmp280Sample *out)
{
    if (!dev || !out) {
        return ERR_INVALID_ARG;
    }

    /*
     * Trigger a forced-mode measurement (temp×1, pressure×1).
     * This guarantees a fresh reading every call — no stale-read possible.
     */
    err_t err = write_reg(dev, BMP280_REG_CTRL_MEAS, BMP280_CTRL_MEAS_FORCED);
    if (err != ERR_OK) { return err; }

    /* Wait for measurement to complete (~10 ms for ×1 oversampling). */
#ifndef HOST_TEST
    vTaskDelay(pdMS_TO_TICKS(10));
#endif

    /*
     * Read 6 contiguous bytes from 0xF7:
     *   0xF7, 0xF8, 0xF9 — pressure (20-bit)
     *   0xFA, 0xFB, 0xFC — temperature (20-bit)
     * (The BME280's humidity bytes at 0xFD/0xFE do not exist on the BMP280.)
     */
    uint8_t raw[6];
    err = read_regs(dev, BMP280_REG_PRESS_MSB, raw, sizeof(raw));
    if (err != ERR_OK) { return err; }

    /* Decode raw ADC values (20-bit for temp/pressure) */
    int32_t adc_P = (int32_t)(((uint32_t)raw[0] << 12) |
                               ((uint32_t)raw[1] <<  4) |
                               ((uint32_t)raw[2] >>  4));
    int32_t adc_T = (int32_t)(((uint32_t)raw[3] << 12) |
                               ((uint32_t)raw[4] <<  4) |
                               ((uint32_t)raw[5] >>  4));

    /* Apply compensation formulas. */

    /* Temperature first (sets t_fine used by pressure). */
    int32_t temp_raw = compensate_temp(dev, adc_T);
    out->temp_c = (float)temp_raw / 100.0f + CAL_ENV_TEMP_DELTA_C;

    /* Pressure: compensate_pressure returns Q24.8 (unit: Pa*256). */
    uint32_t press_q8 = compensate_pressure(dev, adc_P);
    out->pressure_hpa = (float)press_q8 / (256.0f * 100.0f)   /* Pa → hPa */
                        + CAL_ENV_PRES_DELTA_HPA;

    LOG_D("BMP280 sample: T=%.2f C, P=%.2f hPa",
          (double)out->temp_c,
          (double)out->pressure_hpa);

    return ERR_OK;
}

/* ── env_driver_t v-table adapter ──────────────────────────────────────────
 * Wraps the Bmp280 API in the shared env_driver_t interface so callers can
 * select between BMP280 and AHT21 at /cfg/sensors.json provisioning time
 * without rebuilding firmware.  The BMP280 has no humidity sensor, so the
 * adapter reports humidity_valid = false (→ `"hum_pct": null` on the wire,
 * §9.2.2) — symmetric with the AHT21 adapter's pressure_valid = false. */
#include "env_driver.h"

static Bmp280 s_bmp280_ctx;

static err_t bmp280_drv_init(void *ctx, i2c_inst_t *i2c, uint8_t addr) {
    return bmp280_init((Bmp280 *)ctx, i2c, addr);
}

static err_t bmp280_drv_read(void *ctx, EnvSample *out) {
    Bmp280Sample s;
    err_t e = bmp280_read_sample((Bmp280 *)ctx, &s);
    if (e != ERR_OK) return e;
    out->temp_c         = s.temp_c - ENV_TEMP_SELF_HEAT_OFFSET_C;
    out->humidity_pct   = 0.0f;     /* no humidity sensor on the BMP280 */
    out->humidity_valid = false;
    out->pressure_hpa   = s.pressure_hpa;
    out->pressure_valid = true;
    return ERR_OK;
}

static env_driver_t s_bmp280_driver = {
    .init        = bmp280_drv_init,
    .read_sample = bmp280_drv_read,
    .name        = "BMP280",
    .ctx         = &s_bmp280_ctx,
};

env_driver_t *env_bmp280_driver(void) { return &s_bmp280_driver; }
