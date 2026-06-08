/* Host unit tests for the BH1750 driver — verifies the 16-bit-raw → lux
 * conversion (lux = raw / 1.2) using canonical examples.
 *
 *   raw = 0x0000 →  0.0 lx
 *   raw = 0x0001 →  ~0.833 lx
 *   raw = 0xBE57 →  ~40 657.5 lx
 *   raw = 0xFFFF →  ~54 612.5 lx
 *
 * pico-sdk / FreeRTOS / log symbols come from test/host/stubs; only the 2-byte
 * I²C result read is mocked here. */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include "cmocka.h"

#include "bh1750.h"   /* pulls stub hardware/i2c.h */

static uint16_t s_pending_raw = 0;

int i2c_write_blocking(i2c_inst_t *i2c, uint8_t addr,
                       const uint8_t *src, size_t len, bool nostop) {
    (void)i2c; (void)addr; (void)src; (void)nostop;
    return (int)len;
}

int i2c_read_blocking(i2c_inst_t *i2c, uint8_t addr,
                      uint8_t *dst, size_t len, bool nostop) {
    (void)i2c; (void)addr; (void)nostop;
    assert_int_equal(len, 2);            /* H-res result is a 16-bit big-endian count */
    dst[0] = (uint8_t)(s_pending_raw >> 8);
    dst[1] = (uint8_t)(s_pending_raw & 0xFF);
    return 2;
}

static i2c_inst_t s_i2c;

static void check_lux(uint16_t raw, float expected_lx) {
    s_pending_raw = raw;
    Bh1750 dev;
    assert_int_equal(bh1750_init(&dev, &s_i2c, 0x23), ERR_OK);
    Bh1750Sample s;
    assert_int_equal(bh1750_read_sample(&dev, &s), ERR_OK);
    assert_true(fabsf(s.lux - expected_lx) < 0.01f);
}

static void test_zero(void **state) { (void)state; check_lux(0x0000,     0.0f); }
static void test_step(void **state) { (void)state; check_lux(0x0001,     0.8333333f); }
static void test_high(void **state) { (void)state; check_lux(0x6978, 22500.0f); }  /* 27000 / 1.2 */
static void test_max (void **state) { (void)state; check_lux(0xFFFF, 54612.5f); }  /* 65535 / 1.2 */

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_zero),
        cmocka_unit_test(test_step),
        cmocka_unit_test(test_high),
        cmocka_unit_test(test_max),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
