#define LOG_TAG "ENS160"
#include "log.h"

#include "ens160.h"
#include "err.h"

#include "hardware/i2c.h"

#ifndef HOST_TEST
#  include "FreeRTOS.h"
#  include "task.h"
#endif

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ── Register map (ENS160 datasheet, rev 1.4 §6) ────────────────────────── */
#define ENS160_REG_PART_ID      0x00U   /* 2 bytes LE, expected 0x0160       */
#define ENS160_REG_OPMODE       0x10U
#define ENS160_REG_CONFIG       0x11U
#define ENS160_REG_COMMAND      0x12U
#define ENS160_REG_TEMP_IN      0x13U   /* 2 bytes LE, Q8.8 Kelvin           */
#define ENS160_REG_RH_IN        0x15U   /* 2 bytes LE, Q8.8 %rH              */
#define ENS160_REG_STATUS       0x20U
#define ENS160_REG_AQI          0x21U   /* UBA AQI 1..5                      */
#define ENS160_REG_TVOC         0x22U   /* 2 bytes LE, ppb                   */
#define ENS160_REG_ECO2         0x24U   /* 2 bytes LE, ppm                   */

#define ENS160_OPMODE_DEEP_SLEEP    0x00U
#define ENS160_OPMODE_IDLE          0x01U
#define ENS160_OPMODE_STANDARD      0x02U
#define ENS160_OPMODE_RESET         0xF0U

#define ENS160_PART_ID_EXPECTED     0x0160U

/* ── Low-level I²C helpers ──────────────────────────────────────────────── */

static err_t write_reg(Ens160 *dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    int ret = i2c_write_blocking(dev->i2c, dev->addr, buf, 2, false);
    if (ret < 0) {
        LOG_E("I2C write reg 0x%02X failed: %d", reg, ret);
        return ERR_IO;
    }
    return ERR_OK;
}

static err_t write_regs(Ens160 *dev, uint8_t reg, const uint8_t *src, size_t len)
{
    /* Largest burst the driver issues is 4 bytes (temp + hum compensation). */
    uint8_t buf[1 + 4];
    if (len > sizeof(buf) - 1U) {
        return ERR_INVALID_ARG;
    }
    buf[0] = reg;
    memcpy(&buf[1], src, len);
    int ret = i2c_write_blocking(dev->i2c, dev->addr, buf, 1 + len, false);
    if (ret < 0) {
        LOG_E("I2C write regs 0x%02X len %u failed: %d", reg, (unsigned)len, ret);
        return ERR_IO;
    }
    return ERR_OK;
}

static err_t read_regs(Ens160 *dev, uint8_t reg, uint8_t *dst, size_t len)
{
    int ret = i2c_write_blocking(dev->i2c, dev->addr, &reg, 1, true);
    if (ret < 0) {
        LOG_E("I2C write addr failed: %d", ret);
        return ERR_IO;
    }
    ret = i2c_read_blocking(dev->i2c, dev->addr, dst, len, false);
    if (ret < 0) {
        LOG_E("I2C read reg 0x%02X len %u failed: %d", reg, (unsigned)len, ret);
        return ERR_IO;
    }
    return ERR_OK;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

err_t ens160_init(Ens160 *dev, i2c_inst_t *i2c, uint8_t addr)
{
    if (!dev || !i2c) {
        return ERR_INVALID_ARG;
    }
    memset(dev, 0, sizeof(*dev));
    dev->i2c  = i2c;
    dev->addr = addr;

    /* Soft reset. */
    err_t err = write_reg(dev, ENS160_REG_OPMODE, ENS160_OPMODE_RESET);
    if (err != ERR_OK) { return err; }
#ifndef HOST_TEST
    vTaskDelay(pdMS_TO_TICKS(10));
#endif

    /* Verify part ID. */
    uint8_t pid_bytes[2] = { 0 };
    err = read_regs(dev, ENS160_REG_PART_ID, pid_bytes, 2);
    if (err != ERR_OK) { return err; }
    uint16_t pid = (uint16_t)pid_bytes[0] |
                  ((uint16_t)pid_bytes[1] << 8);
    if (pid != ENS160_PART_ID_EXPECTED) {
        LOG_E("part ID mismatch: got 0x%04X expected 0x%04X",
              pid, ENS160_PART_ID_EXPECTED);
        return ERR_NOT_FOUND;
    }

    /* Enter standard sensing mode. */
    err = write_reg(dev, ENS160_REG_OPMODE, ENS160_OPMODE_STANDARD);
    if (err != ERR_OK) { return err; }
#ifndef HOST_TEST
    vTaskDelay(pdMS_TO_TICKS(10));
#endif

    LOG_I("ENS160 init OK at 0x%02X (warmup ~3 min)", addr);
    return ERR_OK;
}

err_t ens160_set_compensation(Ens160 *dev, float temp_c, float hum_pct)
{
    if (!dev) {
        return ERR_INVALID_ARG;
    }
    /* TEMP_IN: Kelvin in Q8.8 (datasheet §6.2.4). */
    float temp_k = temp_c + 273.15f;
    if (temp_k < 0.0f) temp_k = 0.0f;
    if (temp_k > 327.67f) temp_k = 327.67f;
    uint16_t temp_q88 = (uint16_t)(temp_k * 256.0f);

    /* RH_IN: %rH in Q8.8. */
    if (hum_pct < 0.0f) hum_pct = 0.0f;
    if (hum_pct > 100.0f) hum_pct = 100.0f;
    uint16_t rh_q88 = (uint16_t)(hum_pct * 256.0f);

    uint8_t buf[4] = {
        (uint8_t)(temp_q88 & 0xFFU), (uint8_t)(temp_q88 >> 8),
        (uint8_t)(rh_q88   & 0xFFU), (uint8_t)(rh_q88   >> 8),
    };
    return write_regs(dev, ENS160_REG_TEMP_IN, buf, 4);
}

err_t ens160_read_sample(Ens160 *dev, Ens160Sample *out)
{
    if (!dev || !out) {
        return ERR_INVALID_ARG;
    }

    /* Single 6-byte burst from 0x20: STATUS, AQI, TVOC_L, TVOC_H, ECO2_L, ECO2_H */
    uint8_t buf[6] = { 0 };
    err_t err = read_regs(dev, ENS160_REG_STATUS, buf, sizeof(buf));
    if (err != ERR_OK) { return err; }

    out->status   = buf[0];
    out->aqi      = buf[1];
    out->tvoc_ppb = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
    out->co2_ppm  = (uint16_t)buf[4] | ((uint16_t)buf[5] << 8);

    LOG_D("ENS160 sample: status=0x%02X AQI=%u TVOC=%u ppb CO2=%u ppm",
          out->status, out->aqi, out->tvoc_ppb, out->co2_ppm);

    return ERR_OK;
}
