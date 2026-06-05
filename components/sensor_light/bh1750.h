#ifndef BH1750_H
#define BH1750_H

/* Rohm BH1750FVI — digital ambient-light sensor over I²C.
 *
 * CLAUDE.md §3.2 originally specified a GL5516 LDR on ADC. The shipping
 * hardware uses the BH1750 instead: same purpose, but calibrated lux output
 * over I²C (16-bit raw value / 1.2 → lux per the datasheet) and a 5-decade
 * dynamic range, vs. the LDR's voltage-divider + ADC compensation. The
 * §9.2.2 light topic payload is unchanged: {"lux": ...}.
 *
 * Protocol (I²C, 7-bit addr 0x23 default, 0x5C if ADDR pin pulled high):
 *   1. Power on:                       write 0x01
 *   2. Continuous H-resolution mode:   write 0x10
 *      (1 lux resolution, ~120 ms per measurement, auto-cycles)
 *   3. Read 2 bytes:
 *        raw_u16 = (byte[0] << 8) | byte[1]
 *        lux     = raw_u16 / 1.2
 */

#include "err.h"
#include "hardware/i2c.h"
#include <stdint.h>

typedef struct {
    float lux;
} Bh1750Sample;

typedef struct {
    i2c_inst_t *i2c;
    uint8_t     addr;       /* 0x23 default */
} Bh1750;

/* Initialise: power-on + put into continuous H-resolution mode. Waits for
 * the first measurement to be available before returning. */
err_t bh1750_init(Bh1750 *dev, i2c_inst_t *i2c, uint8_t addr);

/* Read the most recent measurement (the chip auto-cycles in continuous
 * mode; no trigger needed). */
err_t bh1750_read_sample(Bh1750 *dev, Bh1750Sample *out);

#endif /* BH1750_H */
