/*
 * Host unit tests for the AHT21 driver — exercises the CRC8 verification and
 * the 20-bit raw → physical temp/humidity decode path with no hardware.
 *
 * Test vectors are constructed analytically:
 *   rh_target = 50.0 %  →  hum_raw  = 0x80000  (rh = 2^20 / 2)
 *   t_target  = 25.0 °C → temp_raw = 0x60000  (t = (75 / 200) * 2^20)
 *
 * Packed per the AHT21 protocol (see components/sensor_env/aht21.h):
 *   [status][hum_h][hum_m][hum_l/temp_h][temp_m][temp_l][crc8]
 *     0x18  0x80  0x00     0x06         0x00    0x00   <computed>
 *
 * status = 0x18 means calibrated + not busy, so aht21_init() takes the
 * fast path (no soft-init write).
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

/* ── HOST_TEST stubs for pico-sdk + FreeRTOS + log ───────────────────────── */
#ifdef HOST_TEST

typedef struct i2c_inst { int dummy; } i2c_inst_t;
static i2c_inst_t i2c0_inst;
i2c_inst_t *i2c0 = &i2c0_inst;

/* The test orchestrates the i2c_read sequence via this small state machine:
 *   call 1: status read (init → calibrated check)
 *   call 2: 7-byte measurement read
 * If the test sets WANT_BAD_CRC, the CRC byte is corrupted so we can verify
 * aht21_read_sample fails closed. */
static int s_read_call = 0;
static bool s_want_bad_crc = false;

/* CRC-8 (poly 0x31, init 0xFF) — duplicate the driver's local helper here so
 * the test computes the expected byte without exposing the static function. */
static uint8_t test_crc8(const uint8_t *data, size_t len) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

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
    s_read_call++;

    if (s_read_call == 1) {
        /* Status read: 0x18 = calibrated, not busy. Driver skips soft-init. */
        assert_int_equal(len, 1);
        dst[0] = 0x18;
        return 1;
    }
    if (s_read_call == 2) {
        /* 7-byte measurement read. Payload encodes rh=50 % and t=25 °C. */
        assert_int_equal(len, 7);
        const uint8_t body[6] = { 0x18, 0x80, 0x00, 0x06, 0x00, 0x00 };
        memcpy(dst, body, 6);
        dst[6] = test_crc8(body, 6);
        if (s_want_bad_crc) dst[6] ^= 0xAA;
        return 7;
    }
    return 0;
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

#include "aht21.h"

/* ── Tests ───────────────────────────────────────────────────────────────── */

static void test_aht21_decode_25c_50rh(void **state) {
    (void)state;
    extern i2c_inst_t *i2c0;
    s_read_call = 0;
    s_want_bad_crc = false;

    Aht21 dev;
    assert_int_equal(aht21_init(&dev, i2c0, 0x38), ERR_OK);

    Aht21Sample s;
    assert_int_equal(aht21_read_sample(&dev, &s), ERR_OK);

    /* Allow 0.05 deg / 0.05 % tolerance — the 20-bit quantization is
     * coarser than that but the synthetic input lands exactly on a code. */
    assert_true(fabs(s.temp_c       - 25.0) < 0.05);
    assert_true(fabs(s.humidity_pct - 50.0) < 0.05);
}

static void test_aht21_crc_failure_is_io_error(void **state) {
    (void)state;
    extern i2c_inst_t *i2c0;
    s_read_call = 0;
    s_want_bad_crc = true;

    Aht21 dev;
    assert_int_equal(aht21_init(&dev, i2c0, 0x38), ERR_OK);

    Aht21Sample s;
    /* Read should refuse to silently propagate corrupted data. */
    assert_int_equal(aht21_read_sample(&dev, &s), ERR_IO);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_aht21_decode_25c_50rh),
        cmocka_unit_test(test_aht21_crc_failure_is_io_error),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
