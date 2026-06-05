/*
 * Host unit tests for the BH1750 driver — verifies the 16-bit-raw → lux
 * conversion (lux = raw / 1.2) using the canonical datasheet examples.
 *
 *   raw = 0x0000 →  0.0 lx
 *   raw = 0x0001 →  ~0.833 lx       (1-lx step in H-resolution mode is ~1.2)
 *   raw = 0xBE57 →  ~40 657.5 lx    (high range)
 *   raw = 0xFFFF →  ~54 612.5 lx    (max code)
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#include "cmocka.h"

#ifdef HOST_TEST

typedef struct i2c_inst { int dummy; } i2c_inst_t;
static i2c_inst_t i2c0_inst;
i2c_inst_t *i2c0 = &i2c0_inst;

static uint16_t s_pending_raw = 0;

int i2c_write_blocking(i2c_inst_t *i2c, uint8_t addr,
                       const uint8_t *src, size_t len, bool nostop)
{
    (void)i2c; (void)addr; (void)src; (void)len; (void)nostop;
    return (int)len;
}

int i2c_read_blocking(i2c_inst_t *i2c, uint8_t addr,
                      uint8_t *dst, size_t len, bool nostop)
{
    (void)i2c; (void)addr; (void)nostop;
    assert_int_equal(len, 2);
    dst[0] = (uint8_t)(s_pending_raw >> 8);
    dst[1] = (uint8_t)(s_pending_raw & 0xFF);
    return 2;
}

void vTaskDelay(uint32_t ticks) { (void)ticks; }
#define pdMS_TO_TICKS(ms) (ms)

void log_write(int level, const char *tag, const char *fmt, ...)
{
    (void)level;
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[%s] ", tag); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}

#endif /* HOST_TEST */

#include "bh1750.h"

static void check_lux(uint16_t raw, float expected_lx) {
    extern i2c_inst_t *i2c0;
    s_pending_raw = raw;

    Bh1750 dev;
    assert_int_equal(bh1750_init(&dev, i2c0, 0x23), ERR_OK);

    Bh1750Sample s;
    assert_int_equal(bh1750_read_sample(&dev, &s), ERR_OK);

    /* ±0.01 lux tolerance is plenty for the / 1.2 division. */
    assert_true(fabsf(s.lux - expected_lx) < 0.01f);
}

static void test_bh1750_zero (void **state)       { (void)state; check_lux(0x0000,     0.0f); }
static void test_bh1750_step (void **state)       { (void)state; check_lux(0x0001,     0.8333333f); }
static void test_bh1750_high (void **state)       { (void)state; check_lux(0xBE57, 40657.5f); }
static void test_bh1750_max  (void **state)       { (void)state; check_lux(0xFFFF, 54612.5f); }

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_bh1750_zero),
        cmocka_unit_test(test_bh1750_step),
        cmocka_unit_test(test_bh1750_high),
        cmocka_unit_test(test_bh1750_max),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
