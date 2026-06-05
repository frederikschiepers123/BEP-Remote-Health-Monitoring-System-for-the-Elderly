#ifndef AHT21_H
#define AHT21_H

/* ASAIR AHT21 temperature + humidity sensor driver.
 *
 * Used on the ENS160+AHT21 combo breakout. The AHT21 has NO pressure
 * sensor — env_sample_encode emits `"pres_hpa": null` per CLAUDE.md §9.2.3
 * when the AHT21 path is active.
 *
 * Protocol (I²C, fixed 7-bit addr 0x38):
 *   1. Status read (0x71). If status & 0x18 != 0x18, calibrate via
 *      write 0xBE 0x08 0x00, wait 10 ms.
 *   2. Trigger:  write 0xAC 0x33 0x00, wait 80 ms (datasheet typical).
 *   3. Read 7 bytes:
 *        [status][hum_h][hum_m][hum_l/temp_h][temp_m][temp_l][crc8]
 *        hum_raw  = (bytes[1] << 12) | (bytes[2] << 4) | (bytes[3] >> 4)
 *        temp_raw = ((bytes[3] & 0x0F) << 16) | (bytes[4] << 8) | bytes[5]
 *        rh_pct = (hum_raw  / 2^20) * 100
 *        t_c    = (temp_raw / 2^20) * 200 - 50
 *        CRC8 (poly 0x31, init 0xFF) over bytes 0..5. */

#include "err.h"
#include "hardware/i2c.h"
#include <stdint.h>

typedef struct {
    float temp_c;
    float humidity_pct;
} Aht21Sample;

typedef struct {
    i2c_inst_t *i2c;
    uint8_t     addr;       /* 0x38 (fixed) */
} Aht21;

err_t aht21_init(Aht21 *dev, i2c_inst_t *i2c, uint8_t addr);
err_t aht21_read_sample(Aht21 *dev, Aht21Sample *out);

#endif /* AHT21_H */
