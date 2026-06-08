/* Host unit tests for the AHT21/AHT20 decode logic.
 *
 * The driver reads 6 bytes (status + 5 data) and decodes two 20-bit fields.
 * It deliberately does NOT verify a CRC: the AHT20 silicon on most "AHT21"
 * combo breakouts NACKs the 7th byte, so a CRC check failed on every cycle
 * even with valid data (see aht21.c). These tests pin the decode math to the
 * datasheet example (rh=50 %, t=25 °C) and the BUSY-flag failure path.
 *
 * The pico-sdk / FreeRTOS / log symbols come from test/host/stubs; only the
 * I²C transaction surface is mocked here. */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include "cmocka.h"

#include "aht21.h"   /* pulls stub hardware/i2c.h (defines i2c_inst_t) */

/* ── Mocked I²C transaction sequence ──────────────────────────────────────────
 *   call 1: aht21_init status read  -> 0x18 (calibrated, not busy)
 *   call 2: aht21_read_sample 6-byte measurement read
 * s_status_override lets a test inject a BUSY status byte into call 2. */
static int     s_read_call = 0;
static uint8_t s_meas_status = 0x18;   /* raw[0] of the measurement read */

int i2c_write_blocking(i2c_inst_t *i2c, uint8_t addr,
                       const uint8_t *src, size_t len, bool nostop) {
    (void)i2c; (void)addr; (void)src; (void)nostop;
    return (int)len;
}

int i2c_read_blocking(i2c_inst_t *i2c, uint8_t addr,
                      uint8_t *dst, size_t len, bool nostop) {
    (void)i2c; (void)addr; (void)nostop;
    s_read_call++;
    if (s_read_call == 1) {                 /* init: status read */
        assert_int_equal(len, 1);
        dst[0] = 0x18;                      /* calibrated, not busy */
        return 1;
    }
    /* measurement read — driver asks for 6 bytes (no CRC byte) */
    assert_int_equal(len, 6);
    /* status + rh=50 % (0x80000) + t=25 °C (0x06<<16) */
    const uint8_t body[6] = { s_meas_status, 0x80, 0x00, 0x06, 0x00, 0x00 };
    memcpy(dst, body, 6);
    return 6;
}

static i2c_inst_t s_i2c;

static int setup(void **state) {
    (void)state; s_read_call = 0; s_meas_status = 0x18; return 0;
}

static void test_decode_25c_50rh(void **state) {
    (void)state;
    Aht21 dev;
    assert_int_equal(aht21_init(&dev, &s_i2c, 0x38), ERR_OK);
    Aht21Sample s;
    assert_int_equal(aht21_read_sample(&dev, &s), ERR_OK);
    assert_true(fabs(s.temp_c       - 25.0) < 0.05);
    assert_true(fabs(s.humidity_pct - 50.0) < 0.05);
}

static void test_busy_status_is_busy_error(void **state) {
    (void)state;
    s_meas_status = 0x98;                   /* BUSY bit (0x80) set */
    Aht21 dev;
    assert_int_equal(aht21_init(&dev, &s_i2c, 0x38), ERR_OK);
    Aht21Sample s;
    assert_int_equal(aht21_read_sample(&dev, &s), ERR_BUSY);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup(test_decode_25c_50rh,        setup),
        cmocka_unit_test_setup(test_busy_status_is_busy_error, setup),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
