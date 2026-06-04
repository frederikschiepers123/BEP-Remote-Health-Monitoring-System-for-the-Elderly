#define LOG_TAG "SH1122"
#include "log.h"

#include "sh1122.h"
#include "err.h"

#include "hardware/i2c.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* SH1122 I²C control byte. 0x00 = command stream, 0x40 = data stream
 * (high bit "Co" = 0 means stream continues to end of transfer). */
#define SH1122_CMD_BYTE    0x00U
#define SH1122_DATA_BYTE   0x40U

static const uint8_t s_font5x7[64][5] = {
    /* 32 ' ' */ { 0x00, 0x00, 0x00, 0x00, 0x00 },
    /* 33 '!' */ { 0x00, 0x00, 0x5F, 0x00, 0x00 },
    /* 34 '"' */ { 0x00, 0x07, 0x00, 0x07, 0x00 },
    /* 35 '#' */ { 0x14, 0x7F, 0x14, 0x7F, 0x14 },
    /* 36 '$' */ { 0x24, 0x2A, 0x7F, 0x2A, 0x12 },
    /* 37 '%' */ { 0x23, 0x13, 0x08, 0x64, 0x62 },
    /* 38 '&' */ { 0x36, 0x49, 0x55, 0x22, 0x50 },
    /* 39 '\''*/ { 0x00, 0x05, 0x03, 0x00, 0x00 },
    /* 40 '(' */ { 0x00, 0x1C, 0x22, 0x41, 0x00 },
    /* 41 ')' */ { 0x00, 0x41, 0x22, 0x1C, 0x00 },
    /* 42 '*' */ { 0x14, 0x08, 0x3E, 0x08, 0x14 },
    /* 43 '+' */ { 0x08, 0x08, 0x3E, 0x08, 0x08 },
    /* 44 ',' */ { 0x00, 0x50, 0x30, 0x00, 0x00 },
    /* 45 '-' */ { 0x08, 0x08, 0x08, 0x08, 0x08 },
    /* 46 '.' */ { 0x00, 0x60, 0x60, 0x00, 0x00 },
    /* 47 '/' */ { 0x20, 0x10, 0x08, 0x04, 0x02 },
    /* 48 '0' */ { 0x3E, 0x51, 0x49, 0x45, 0x3E },
    /* 49 '1' */ { 0x00, 0x42, 0x7F, 0x40, 0x00 },
    /* 50 '2' */ { 0x42, 0x61, 0x51, 0x49, 0x46 },
    /* 51 '3' */ { 0x21, 0x41, 0x45, 0x4B, 0x31 },
    /* 52 '4' */ { 0x18, 0x14, 0x12, 0x7F, 0x10 },
    /* 53 '5' */ { 0x27, 0x45, 0x45, 0x45, 0x39 },
    /* 54 '6' */ { 0x3C, 0x4A, 0x49, 0x49, 0x30 },
    /* 55 '7' */ { 0x01, 0x71, 0x09, 0x05, 0x03 },
    /* 56 '8' */ { 0x36, 0x49, 0x49, 0x49, 0x36 },
    /* 57 '9' */ { 0x06, 0x49, 0x49, 0x29, 0x1E },
    /* 58 ':' */ { 0x00, 0x36, 0x36, 0x00, 0x00 },
    /* 59 ';' */ { 0x00, 0x56, 0x36, 0x00, 0x00 },
    /* 60 '<' */ { 0x08, 0x14, 0x22, 0x41, 0x00 },
    /* 61 '=' */ { 0x14, 0x14, 0x14, 0x14, 0x14 },
    /* 62 '>' */ { 0x00, 0x41, 0x22, 0x14, 0x08 },
    /* 63 '?' */ { 0x02, 0x01, 0x51, 0x09, 0x06 },
    /* 64 '@' */ { 0x32, 0x49, 0x79, 0x41, 0x3E },
    /* 65 'A' */ { 0x7E, 0x11, 0x11, 0x11, 0x7E },
    /* 66 'B' */ { 0x7F, 0x49, 0x49, 0x49, 0x36 },
    /* 67 'C' */ { 0x3E, 0x41, 0x41, 0x41, 0x22 },
    /* 68 'D' */ { 0x7F, 0x41, 0x41, 0x22, 0x1C },
    /* 69 'E' */ { 0x7F, 0x49, 0x49, 0x49, 0x41 },
    /* 70 'F' */ { 0x7F, 0x09, 0x09, 0x09, 0x01 },
    /* 71 'G' */ { 0x3E, 0x41, 0x49, 0x49, 0x7A },
    /* 72 'H' */ { 0x7F, 0x08, 0x08, 0x08, 0x7F },
    /* 73 'I' */ { 0x00, 0x41, 0x7F, 0x41, 0x00 },
    /* 74 'J' */ { 0x20, 0x40, 0x41, 0x3F, 0x01 },
    /* 75 'K' */ { 0x7F, 0x08, 0x14, 0x22, 0x41 },
    /* 76 'L' */ { 0x7F, 0x40, 0x40, 0x40, 0x40 },
    /* 77 'M' */ { 0x7F, 0x02, 0x0C, 0x02, 0x7F },
    /* 78 'N' */ { 0x7F, 0x04, 0x08, 0x10, 0x7F },
    /* 79 'O' */ { 0x3E, 0x41, 0x41, 0x41, 0x3E },
    /* 80 'P' */ { 0x7F, 0x09, 0x09, 0x09, 0x06 },
    /* 81 'Q' */ { 0x3E, 0x41, 0x51, 0x21, 0x5E },
    /* 82 'R' */ { 0x7F, 0x09, 0x19, 0x29, 0x46 },
    /* 83 'S' */ { 0x46, 0x49, 0x49, 0x49, 0x31 },
    /* 84 'T' */ { 0x01, 0x01, 0x7F, 0x01, 0x01 },
    /* 85 'U' */ { 0x3F, 0x40, 0x40, 0x40, 0x3F },
    /* 86 'V' */ { 0x1F, 0x20, 0x40, 0x20, 0x1F },
    /* 87 'W' */ { 0x3F, 0x40, 0x38, 0x40, 0x3F },
    /* 88 'X' */ { 0x63, 0x14, 0x08, 0x14, 0x63 },
    /* 89 'Y' */ { 0x07, 0x08, 0x70, 0x08, 0x07 },
    /* 90 'Z' */ { 0x61, 0x51, 0x49, 0x45, 0x43 },
    /* 91 '[' */ { 0x00, 0x7F, 0x41, 0x41, 0x00 },
    /* 92 '\\'*/ { 0x02, 0x04, 0x08, 0x10, 0x20 },
    /* 93 ']' */ { 0x00, 0x41, 0x41, 0x7F, 0x00 },
    /* 94 '^' */ { 0x04, 0x02, 0x01, 0x02, 0x04 },
    /* 95 '_' */ { 0x40, 0x40, 0x40, 0x40, 0x40 },
};

/* ── Low-level I²C helpers ──────────────────────────────────────────────── */

static err_t send_cmd(Sh1122 *dev, uint8_t cmd)
{
    uint8_t buf[2] = { SH1122_CMD_BYTE, cmd };
    int rc = i2c_write_blocking(dev->i2c, dev->addr, buf, 2, false);
    if (rc < 0) {
        LOG_E("I2C cmd 0x%02X failed: %d", cmd, rc);
        return ERR_IO;
    }
    return ERR_OK;
}

static err_t send_cmd2(Sh1122 *dev, uint8_t a, uint8_t b)
{
    err_t e = send_cmd(dev, a);
    if (e != ERR_OK) return e;
    return send_cmd(dev, b);
}

/* Send a 128-byte row of pixel data. */
static err_t send_data_row(Sh1122 *dev, const uint8_t *row)
{
    uint8_t buf[1 + (SH1122_WIDTH / 2U)];
    buf[0] = SH1122_DATA_BYTE;
    memcpy(&buf[1], row, SH1122_WIDTH / 2U);
    int rc = i2c_write_blocking(dev->i2c, dev->addr, buf, sizeof(buf), false);
    if (rc < 0) {
        LOG_E("I2C data row failed: %d", rc);
        return ERR_IO;
    }
    return ERR_OK;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

err_t sh1122_init(Sh1122 *dev, i2c_inst_t *i2c, uint8_t addr)
{
    if (!dev || !i2c) return ERR_INVALID_ARG;
    memset(dev, 0, sizeof(*dev));
    dev->i2c  = i2c;
    dev->addr = addr;

    /* Standard SH1122 256×64 power-up sequence.
     * Values are conservative defaults — most 2.08" panels (e.g. Adafruit-style
     * yellow/green OLEDs) accept these without tuning. If the contrast looks
     * dim, raise the 0x81 byte. */
    err_t e;
    e = send_cmd (dev, 0xAEU);            if (e) return e;  /* display off */
    e = send_cmd2(dev, 0xB0U, 0x00U);     if (e) return e;  /* row address = 0 */
    e = send_cmd (dev, 0x00U);            if (e) return e;  /* col low nibble = 0 */
    e = send_cmd (dev, 0x10U);            if (e) return e;  /* col high nibble = 0 */
    e = send_cmd (dev, 0xC0U);            if (e) return e;  /* COM scan direction normal */
    e = send_cmd (dev, 0xA0U);            if (e) return e;  /* segment remap normal */
    e = send_cmd2(dev, 0xA8U, 0x3FU);     if (e) return e;  /* multiplex = 63 (64 rows) */
    e = send_cmd2(dev, 0xD3U, 0x00U);     if (e) return e;  /* display offset = 0 */
    e = send_cmd (dev, 0x40U);            if (e) return e;  /* display start line = 0 */
    e = send_cmd2(dev, 0xD5U, 0x50U);     if (e) return e;  /* clock divide / oscillator */
    e = send_cmd2(dev, 0xD9U, 0x22U);     if (e) return e;  /* pre-charge period */
    e = send_cmd2(dev, 0xDBU, 0x35U);     if (e) return e;  /* VCOMH deselect */
    e = send_cmd2(dev, 0xDCU, 0x35U);     if (e) return e;  /* pre-charge voltage */
    e = send_cmd2(dev, 0x81U, 0x7FU);     if (e) return e;  /* contrast = 127/255 */
    e = send_cmd (dev, 0x30U);            if (e) return e;  /* discharge level */
    e = send_cmd (dev, 0xA4U);            if (e) return e;  /* RAM content (not all-on) */
    e = send_cmd (dev, 0xA6U);            if (e) return e;  /* normal (not inverse) */
    e = send_cmd (dev, 0xAFU);            if (e) return e;  /* display on */

    sh1122_clear(dev);
    e = sh1122_flush(dev);
    if (e != ERR_OK) return e;

    LOG_I("SH1122 init OK at 0x%02X (256x64, 4bpp)", addr);
    return ERR_OK;
}

void sh1122_clear(Sh1122 *dev)
{
    if (!dev) return;
    memset(dev->framebuf, 0, sizeof(dev->framebuf));
}

err_t sh1122_flush(Sh1122 *dev)
{
    if (!dev) return ERR_INVALID_ARG;
    for (uint8_t row = 0; row < SH1122_HEIGHT; row++) {
        /* Set row pointer + column = 0 for this row. */
        err_t e = send_cmd2(dev, 0xB0U, row);
        if (e != ERR_OK) return e;
        e = send_cmd(dev, 0x00U);   /* col low nibble */
        if (e != ERR_OK) return e;
        e = send_cmd(dev, 0x10U);   /* col high nibble */
        if (e != ERR_OK) return e;

        e = send_data_row(dev, &dev->framebuf[(size_t)row * (SH1122_WIDTH / 2U)]);
        if (e != ERR_OK) return e;
    }
    return ERR_OK;
}

void sh1122_set_pixel(Sh1122 *dev, uint16_t x, uint16_t y, uint8_t shade)
{
    if (!dev) return;
    if (x >= SH1122_WIDTH || y >= SH1122_HEIGHT) return;
    if (shade > 0x0FU) shade = 0x0FU;

    size_t idx = (size_t)y * (SH1122_WIDTH / 2U) + (x / 2U);
    if (x & 1U) {
        dev->framebuf[idx] = (uint8_t)((dev->framebuf[idx] & 0xF0U) | shade);
    } else {
        dev->framebuf[idx] = (uint8_t)((dev->framebuf[idx] & 0x0FU) | (shade << 4));
    }
}

/* Internal: draw a single 5×7 glyph at (col, row). */
static void draw_glyph(Sh1122 *dev, uint16_t col, uint16_t row,
                       uint8_t scale, char c)
{
    if (scale == 0U) scale = 1U;
    /* Map lowercase to uppercase, fall back to '?' (0x3F) for out-of-range. */
    int idx;
    if (c >= 'a' && c <= 'z') idx = (int)(c - 32) - 32;
    else                       idx = (int)c - 32;
    if (idx < 0 || idx >= 64) idx = '?' - 32;

    const uint8_t *glyph = s_font5x7[idx];
    for (uint8_t gx = 0; gx < 5; gx++) {
        uint8_t bits = glyph[gx];
        for (uint8_t gy = 0; gy < 7; gy++) {
            if (bits & (1U << gy)) {
                /* Each glyph pixel becomes a scale×scale block. */
                for (uint8_t sx = 0; sx < scale; sx++) {
                    for (uint8_t sy = 0; sy < scale; sy++) {
                        uint16_t px = (uint16_t)(col + (uint16_t)gx * scale + sx);
                        uint16_t py = (uint16_t)(row + (uint16_t)gy * scale + sy);
                        sh1122_set_pixel(dev, px, py, 0x0FU);
                    }
                }
            }
        }
    }
}

void sh1122_draw_text(Sh1122 *dev, uint16_t col, uint16_t row,
                      uint8_t scale, const char *text)
{
    if (!dev || !text) return;
    if (scale == 0U) scale = 1U;
    uint16_t advance = (uint16_t)(6U * scale);   /* 5 px glyph + 1 px space */
    for (uint16_t x = col; *text; text++) {
        if (x + advance > SH1122_WIDTH) break;
        draw_glyph(dev, x, row, scale, *text);
        x = (uint16_t)(x + advance);
    }
}
