#define LOG_TAG "SH1106"
#include "log.h"

#include "sh1106.h"
#include "err.h"

#include "hardware/i2c.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ── I²C control byte for commands vs data ──────────────────────────────── */
/* SH1106 I²C write: [addr][0x00 = cmd stream | 0x40 = data stream][bytes...] */
#define SH1106_CMD_BYTE   0x00U
#define SH1106_DATA_BYTE  0x40U

/* SH1106 column start offset (internal RAM is 132 cols, display shows 128).
 * The visible window starts at column 2. */
#define SH1106_COL_OFFSET 2U

/* ── 5×7 bitmap font, ASCII 32–95 ───────────────────────────────────────── */
/*
 * Each entry is 5 bytes (columns), MSB = top pixel, LSB = bottom of 7 rows.
 * Glyph is 5 px wide × 7 px tall stored in the lower 7 bits of each byte.
 * Rendering writes glyph[0..4] into 5 consecutive framebuf columns then
 * one blank column for spacing → 6 pixels per character.
 *
 * Coverage: ASCII 32 (space) through 95 (~).
 * Total: 64 glyphs × 5 bytes = 320 bytes.
 */
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

/* ── I²C helpers ────────────────────────────────────────────────────────── */

static err_t send_cmd(Sh1106 *dev, uint8_t cmd)
{
    uint8_t buf[2] = { SH1106_CMD_BYTE, cmd };
    int ret = i2c_write_blocking(dev->i2c, dev->addr, buf, 2, false);
    return (ret < 0) ? ERR_IO : ERR_OK;
}

static err_t send_cmd2(Sh1106 *dev, uint8_t cmd, uint8_t arg)
{
    uint8_t buf[3] = { SH1106_CMD_BYTE, cmd, arg };
    int ret = i2c_write_blocking(dev->i2c, dev->addr, buf, 3, false);
    return (ret < 0) ? ERR_IO : ERR_OK;
}

/* ── sh1106_init ─────────────────────────────────────────────────────────── */

err_t sh1106_init(Sh1106 *dev, i2c_inst_t *i2c, uint8_t addr)
{
    if (!dev || !i2c) { return ERR_INVALID_ARG; }
    dev->i2c  = i2c;
    dev->addr = addr;
    memset(dev->framebuf, 0, sizeof(dev->framebuf));

    /*
     * SH1106 initialisation sequence.
     * Commands follow the SH1106 Application Note / datasheet.
     */
    err_t err;

    err = send_cmd(dev, 0xAEU);            /* Display off */
    if (err != ERR_OK) { return err; }

    err = send_cmd2(dev, 0x20U, 0x10U);    /* Memory addressing mode: page */
    if (err != ERR_OK) { return err; }

    err = send_cmd(dev, 0xB0U);            /* Page start address: page 0 */
    if (err != ERR_OK) { return err; }

    err = send_cmd(dev, 0xC8U);            /* COM scan direction: remapped */
    if (err != ERR_OK) { return err; }

    err = send_cmd(dev, 0x00U);            /* Low column address nibble: 0 */
    if (err != ERR_OK) { return err; }

    err = send_cmd(dev, 0x10U);            /* High column address nibble: 0 */
    if (err != ERR_OK) { return err; }

    err = send_cmd(dev, 0x40U);            /* Display start line: 0 */
    if (err != ERR_OK) { return err; }

    err = send_cmd2(dev, 0x81U, 0x7FU);   /* Contrast: 127 */
    if (err != ERR_OK) { return err; }

    err = send_cmd(dev, 0xA1U);            /* Segment remap: column 127→SEG0 */
    if (err != ERR_OK) { return err; }

    err = send_cmd(dev, 0xA6U);            /* Normal display (not inverted) */
    if (err != ERR_OK) { return err; }

    err = send_cmd2(dev, 0xA8U, 0x3FU);   /* Multiplex ratio: 64 */
    if (err != ERR_OK) { return err; }

    err = send_cmd(dev, 0xA4U);            /* Output follows RAM content */
    if (err != ERR_OK) { return err; }

    err = send_cmd2(dev, 0xD3U, 0x00U);   /* Display offset: 0 */
    if (err != ERR_OK) { return err; }

    err = send_cmd2(dev, 0xD5U, 0x80U);   /* Clock divider / oscillator frequency */
    if (err != ERR_OK) { return err; }

    err = send_cmd2(dev, 0xD9U, 0xF1U);   /* Pre-charge period */
    if (err != ERR_OK) { return err; }

    err = send_cmd2(dev, 0xDAU, 0x12U);   /* COM pins hardware configuration */
    if (err != ERR_OK) { return err; }

    err = send_cmd2(dev, 0xDBU, 0x20U);   /* VCOMH deselect level */
    if (err != ERR_OK) { return err; }

    err = send_cmd2(dev, 0x8DU, 0x14U);   /* Charge pump: enable */
    if (err != ERR_OK) { return err; }

    err = send_cmd(dev, 0xAFU);            /* Display on */
    if (err != ERR_OK) { return err; }

    LOG_I("SH1106 init OK at 0x%02X", addr);
    return ERR_OK;
}

/* ── sh1106_clear ────────────────────────────────────────────────────────── */

void sh1106_clear(Sh1106 *dev)
{
    if (!dev) { return; }
    memset(dev->framebuf, 0, sizeof(dev->framebuf));
}

/* ── sh1106_flush ────────────────────────────────────────────────────────── */

void sh1106_flush(Sh1106 *dev)
{
    if (!dev) { return; }

    /*
     * SH1106 page-addressing mode.
     * For each of the 8 pages:
     *   1. Set page address (0xB0 + page).
     *   2. Set lower column address nibble (column offset low).
     *   3. Set higher column address nibble (column offset high).
     *   4. Write 128 data bytes prefixed with 0x40 (Co=0, D/C#=1).
     *
     * The SH1106 column offset is 2 (hardware RAM offset from the display edge).
     */
    uint8_t data_buf[129];  /* 0x40 control byte + 128 data bytes */
    data_buf[0] = SH1106_DATA_BYTE;

    for (uint8_t page = 0; page < 8; page++) {
        /* Set page */
        (void)send_cmd(dev, (uint8_t)(0xB0U | page));
        /* Set column start low nibble (offset = 2 → low = 2, high = 0) */
        (void)send_cmd(dev, (uint8_t)(0x00U | (SH1106_COL_OFFSET & 0x0FU)));
        /* Set column start high nibble */
        (void)send_cmd(dev, (uint8_t)(0x10U | ((SH1106_COL_OFFSET >> 4) & 0x0FU)));

        /* Copy framebuf page into transmit buffer */
        memcpy(&data_buf[1], &dev->framebuf[page * 128], 128);

        (void)i2c_write_blocking(dev->i2c, dev->addr, data_buf,
                                 sizeof(data_buf), false);
    }
}

/* ── sh1106_draw_text ────────────────────────────────────────────────────── */

void sh1106_draw_text(Sh1106 *dev, uint8_t col, uint8_t page, const char *text)
{
    if (!dev || !text || page >= 8) { return; }

    uint8_t x = col;

    for (const char *p = text; *p != '\0'; p++) {
        /* Each character occupies 6 framebuf columns (5 glyph + 1 space). */
        if (x + 6 > 128) { break; }

        /* Map character to font index.  Lowercase → uppercase via -32. */
        char ch = *p;
        if (ch >= 'a' && ch <= 'z') {
            ch = (char)(ch - 32);
        }

        uint8_t idx;
        if (ch >= 32 && ch <= 95) {
            idx = (uint8_t)(ch - 32);
        } else {
            /* Unknown char: solid block */
            for (uint8_t col_byte = 0; col_byte < 5; col_byte++) {
                dev->framebuf[page * 128 + x + col_byte] = 0xFFU;
            }
            dev->framebuf[page * 128 + x + 5] = 0x00U;
            x = (uint8_t)(x + 6);
            continue;
        }

        for (uint8_t col_byte = 0; col_byte < 5; col_byte++) {
            dev->framebuf[page * 128 + x + col_byte] = s_font5x7[idx][col_byte];
        }
        /* Spacing column */
        dev->framebuf[page * 128 + x + 5] = 0x00U;
        x = (uint8_t)(x + 6);
    }
}
