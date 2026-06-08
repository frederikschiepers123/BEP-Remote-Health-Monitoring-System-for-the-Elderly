#ifndef SH1106_H
#define SH1106_H

/* SH1106 128×64 OLED driver over I²C.
 *
 * TODO(spec): controller confirmed as SH1106 at 1.3" — verify by reading
 * the silkscreen on the actual part before first hardware bring-up.
 * If the controller is SSD1306, the page-addressing and column-offset
 * differ slightly; check sh1106_flush() page start column (2 vs 0).
 *
 * The SH1106 has an internal 132-column RAM but only 128 columns are
 * displayed.  Column addressing starts at col+2 (hardware offset).
 *
 * All drawing is done to an in-RAM framebuf (1024 bytes = 8 pages × 128 cols).
 * Call sh1106_flush() to push the framebuf to hardware.
 *
 * Font: 5×7 glyph bitmap, one glyph per column-byte.  Supported characters:
 *   ASCII 32–95 (space, digits 0-9, uppercase A-Z, punctuation).
 *   Lowercase is displayed as uppercase via -32 mapping.
 *   Unknown chars render as a solid block (0xFF).
 */

#include "err.h"
#include "hardware/i2c.h"

#include <stdint.h>
#include <stddef.h>

/* ── Device state ────────────────────────────────────────────────────────── */

typedef struct {
    i2c_inst_t *i2c;
    uint8_t     addr;
    uint8_t     framebuf[128 * 8];  /* 8 pages × 128 columns, 1 bit per pixel */
} Sh1106;

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * Initialise the SH1106.  Sends the full init command sequence and turns the
 * display on.  framebuf is cleared to zero.
 *
 * @return ERR_OK on success, ERR_IO if any I²C command fails.
 */
err_t sh1106_init(Sh1106 *dev, i2c_inst_t *i2c, uint8_t addr);

/**
 * Clear framebuf to zero.  Does NOT flush to hardware; call sh1106_flush().
 */
void sh1106_clear(Sh1106 *dev);

/**
 * Write the entire framebuf to the SH1106 hardware.
 * Sends 8 page writes of 128 bytes each.
 */
void sh1106_flush(Sh1106 *dev);

/**
 * Draw ASCII text string into framebuf at (col, page).
 *
 * @param col   Starting column (0–127).  Each character is 6 pixels wide
 *              (5 glyph + 1 space).
 * @param page  Target page row (0–7).  Each page is 8 pixels tall.
 * @param text  NUL-terminated string.  Clipped at column 128.
 */
void sh1106_draw_text(Sh1106 *dev, uint8_t col, uint8_t page, const char *text);

#endif /* SH1106_H */
