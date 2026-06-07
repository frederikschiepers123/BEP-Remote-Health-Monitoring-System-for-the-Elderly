#define LOG_TAG "AHT21"
#ifndef HOST_TEST
#include "log.h"
#endif

#include "aht21.h"
#include "err.h"

#ifndef HOST_TEST
#include "FreeRTOS.h"
#include "task.h"
#endif

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define AHT21_CMD_STATUS    0x71
#define AHT21_CMD_INIT      0xBE
#define AHT21_CMD_MEASURE   0xAC

#define AHT21_STATUS_BUSY        0x80
#define AHT21_STATUS_CAL_BITS    0x18   /* 0x18 = calibrated */

/* CRC-8 over the first 6 bytes of the response; polynomial 0x31, init 0xFF
 * (AHT2x datasheet). Matches the implementation in ASAIR's reference code. */
static uint8_t crc8(const uint8_t *data, size_t len) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static err_t i2c_write(Aht21 *dev, const uint8_t *buf, size_t len) {
    int n = i2c_write_blocking(dev->i2c, dev->addr, buf, len, false);
    return (n == (int)len) ? ERR_OK : ERR_IO;
}

static err_t i2c_read(Aht21 *dev, uint8_t *buf, size_t len) {
    int n = i2c_read_blocking(dev->i2c, dev->addr, buf, len, false);
    return (n == (int)len) ? ERR_OK : ERR_IO;
}

static err_t read_status(Aht21 *dev, uint8_t *status) {
    uint8_t cmd = AHT21_CMD_STATUS;
    err_t e = i2c_write(dev, &cmd, 1);
    if (e != ERR_OK) return e;
    return i2c_read(dev, status, 1);
}

err_t aht21_init(Aht21 *dev, i2c_inst_t *i2c, uint8_t addr) {
    memset(dev, 0, sizeof(*dev));
    dev->i2c  = i2c;
    dev->addr = addr;

    /* Datasheet: wait ≥40 ms after power-up before talking to the sensor. */
    vTaskDelay(pdMS_TO_TICKS(50));

    uint8_t status = 0;
    err_t e = read_status(dev, &status);
    if (e != ERR_OK) {
        LOG_E("status read failed (rc=%d) — wiring?", (int)e);
        return ERR_NOT_FOUND;
    }

    if ((status & AHT21_STATUS_CAL_BITS) != AHT21_STATUS_CAL_BITS) {
        /* Send the soft-init / calibrate-enable sequence. */
        const uint8_t init_seq[3] = { AHT21_CMD_INIT, 0x08, 0x00 };
        e = i2c_write(dev, init_seq, sizeof(init_seq));
        if (e != ERR_OK) {
            LOG_E("init write failed (rc=%d)", (int)e);
            return ERR_IO;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        if (read_status(dev, &status) != ERR_OK
            || (status & AHT21_STATUS_CAL_BITS) != AHT21_STATUS_CAL_BITS) {
            LOG_E("calibration bits did not come up (status=0x%02x)", status);
            return ERR_FAIL;
        }
    }

    LOG_I("init OK at 0x%02x (status=0x%02x, calibrated)", addr, status);
    return ERR_OK;
}

err_t aht21_read_sample(Aht21 *dev, Aht21Sample *out) {
    const uint8_t trigger[3] = { AHT21_CMD_MEASURE, 0x33, 0x00 };
    err_t e = i2c_write(dev, trigger, sizeof(trigger));
    if (e != ERR_OK) return e;

    /* Fixed 300 ms wait. The datasheet says typical 75 ms / max 100 ms but
     * AHT20 silicon mounted on "AHT21" combo boards needs more. We tried
     * polling the BUSY bit to wait the minimum needed, but a status read
     * issued between the trigger and the burst read can leave the chip's
     * data buffer half-written — observed as humidity stuck at one
     * previous value (e.g. 89.05%) over many samples while temperature
     * keeps updating. 300 ms is well past any AHTxx max conversion time
     * and the publish loop only runs at 1 Hz, so the latency is invisible. */
    vTaskDelay(pdMS_TO_TICKS(300));

    /* Read status + 5 data bytes only. The datasheet specifies a 7-byte
     * read with CRC at byte 6, but most ENS160+AHT21 combo breakouts in
     * the wild actually mount an AHT20 — same protocol up to byte 5, then
     * stops driving SDA so byte 6 reads back as the floating high pull-up
     * (0xff) and CRC verify fails on every cycle. Skipping CRC trades a
     * theoretical undetected single-bit flip for actually-working reads;
     * the 5 data bytes encode well-formed 20-bit fields whose decoded
     * temp + humidity are sanity-checked at sample time by the consumer. */
    uint8_t raw[6];
    e = i2c_read(dev, raw, 6);
    if (e != ERR_OK) return e;

    if (raw[0] & AHT21_STATUS_BUSY) {
        LOG_W("BUSY came back after burst read (status=0x%02x)", raw[0]);
        return ERR_BUSY;
    }

    /* hum_raw is 20 bits: bytes[1] [bytes[2]] [bytes[3] high nibble]
     * temp_raw is 20 bits: [bytes[3] low nibble] bytes[4] bytes[5] */
    uint32_t hum_raw  = ((uint32_t)raw[1] << 12)
                      | ((uint32_t)raw[2] << 4)
                      | ((uint32_t)raw[3] >> 4);
    uint32_t temp_raw = ((uint32_t)(raw[3] & 0x0F) << 16)
                      | ((uint32_t)raw[4] << 8)
                      | (uint32_t)raw[5];

    out->humidity_pct = ((float)hum_raw  / 1048576.0f) * 100.0f;
    out->temp_c       = ((float)temp_raw / 1048576.0f) * 200.0f - 50.0f;
    return ERR_OK;
}

/* ── env_driver_t v-table adapter ────────────────────────────────────────── */
#include "env_driver.h"

static Aht21 s_aht21_ctx;

static err_t aht21_drv_init(void *ctx, i2c_inst_t *i2c, uint8_t addr) {
    return aht21_init((Aht21 *)ctx, i2c, addr);
}

static err_t aht21_drv_read(void *ctx, EnvSample *out) {
    Aht21Sample s;
    err_t e = aht21_read_sample((Aht21 *)ctx, &s);
    if (e != ERR_OK) return e;
    out->temp_c         = s.temp_c;
    out->humidity_pct   = s.humidity_pct;
    out->pressure_hpa   = 0.0f;
    out->pressure_valid = false;
    return ERR_OK;
}

static env_driver_t s_aht21_driver = {
    .init        = aht21_drv_init,
    .read_sample = aht21_drv_read,
    .name        = "AHT21",
    .ctx         = &s_aht21_ctx,
};

env_driver_t *env_aht21_driver(void) { return &s_aht21_driver; }
