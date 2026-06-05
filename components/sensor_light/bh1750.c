#define LOG_TAG "BH1750"
#ifndef HOST_TEST
#include "log.h"
#endif

#include "bh1750.h"
#include "err.h"

#ifndef HOST_TEST
#include "FreeRTOS.h"
#include "task.h"
#endif

#include <string.h>

#define BH1750_CMD_POWER_ON     0x01
#define BH1750_CMD_RESET        0x07
#define BH1750_CMD_CONT_H       0x10   /* continuous 1-lux mode, ~120 ms / sample */

static err_t send_cmd(Bh1750 *dev, uint8_t cmd) {
    int n = i2c_write_blocking(dev->i2c, dev->addr, &cmd, 1, false);
    return (n == 1) ? ERR_OK : ERR_IO;
}

err_t bh1750_init(Bh1750 *dev, i2c_inst_t *i2c, uint8_t addr) {
    memset(dev, 0, sizeof(*dev));
    dev->i2c  = i2c;
    dev->addr = addr;

    err_t e = send_cmd(dev, BH1750_CMD_POWER_ON);
    if (e != ERR_OK) {
        LOG_E("power-on write failed (rc=%d) — wiring/addr?", (int)e);
        return ERR_NOT_FOUND;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    e = send_cmd(dev, BH1750_CMD_CONT_H);
    if (e != ERR_OK) {
        LOG_E("mode write failed (rc=%d)", (int)e);
        return ERR_IO;
    }
    /* Wait one full measurement window before first read. */
    vTaskDelay(pdMS_TO_TICKS(180));

    LOG_I("init OK at 0x%02x (continuous H-resolution)", addr);
    return ERR_OK;
}

err_t bh1750_read_sample(Bh1750 *dev, Bh1750Sample *out) {
    uint8_t raw[2];
    int n = i2c_read_blocking(dev->i2c, dev->addr, raw, sizeof(raw), false);
    if (n != (int)sizeof(raw)) return ERR_IO;

    uint16_t counts = (uint16_t)((uint16_t)raw[0] << 8 | (uint16_t)raw[1]);
    out->lux = (float)counts / 1.2f;
    return ERR_OK;
}
