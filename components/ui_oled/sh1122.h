#ifndef SH1122_H
#define SH1122_H

/* SH1122 256×64 4-bit grayscale OLED driver over I²C.
 *
 * Memory layout: 1 byte holds two horizontally-adjacent pixels packed at 4 bpp
 * — pixel at column 2C goes into the upper nibble of byte at column-address C,
 * pixel at column 2C+1 into the lower nibble. Framebuffer is therefore
 * 128 column-bytes × 64 rows = 8192 bytes.
 *
 * Grayscale: we use full-on (nibble = 0xF) and off (nibble = 0x0) for v1 —
 * dimming/gradients are easy to add later by passing a 0..15 intensity into
 * sh1122_set_pixel().
 *
 * The driver wraps a portable 5×7 font with optional 2× scaling so text is
 * legible on the 256-wide display. Font coverage: ASCII 32–95 (no lowercase
 * — lowercase falls back to uppercase via -32 mapping).
 */

#include "err.h"
#include "hardware/i2c.h"

#include <stdbool.h>
#include <stdint.h>

#define SH1122_WIDTH    256U
#define SH1122_HEIGHT   64U
#define SH1122_FB_BYTES ((SH1122_WIDTH / 2U) * SH1122_HEIGHT)   /* 8192 */

typedef struct {
    i2c_inst_t *i2c;
    uint8_t     addr;
    uint8_t     framebuf[SH1122_FB_BYTES];
} Sh1122;

/* Initialise the display: run the SH1122 power-up command sequence, clear
 * framebuf, push it to hardware, then turn display on. */
err_t sh1122_init(Sh1122 *dev, i2c_inst_t *i2c, uint8_t addr);

/* Zero the framebuf. Does not push to hardware. */
void sh1122_clear(Sh1122 *dev);

/* Write the entire framebuf to display RAM. */
err_t sh1122_flush(Sh1122 *dev);

/* Set one pixel. shade is 0 (off) to 15 (full brightness). */
void sh1122_set_pixel(Sh1122 *dev, uint16_t x, uint16_t y, uint8_t shade);

/* Draw a NUL-terminated string at (col, row) in framebuf using the 5×7 font.
 * scale: 1 = 5×7 px per glyph (6 col stride), 2 = 10×14 px per glyph (12 col).
 * Clipped at the right/bottom edges. */
void sh1122_draw_text(Sh1122 *dev, uint16_t col, uint16_t row,
                      uint8_t scale, const char *text);

#endif /* SH1122_H */
