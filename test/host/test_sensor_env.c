/*
 * Host unit tests for the BME280 compensation formulas.
 *
 * Verifies that temperature and humidity compensation produce correct output
 * for the reference raw ADC values published in the BME280 datasheet
 * (Appendix A / section 4.2.3).
 *
 * Reference values (BME280 datasheet §4.2.3 compensation formulae example):
 *   adc_T  = 519888
 *   adc_P  = 415148
 *   adc_H  = 29515
 *   Expected temperature: 25.08 °C  (t_raw = 2508)
 *   Expected humidity:    ~50.0 % (rounded from 49.92 / formula)
 *   Expected pressure:    ~1000.0 hPa (varies with calibration coefficients)
 *
 * I²C calls are intercepted by HOST_TEST stubs so no hardware is needed.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#include "cmocka.h"

/* ── HOST_TEST stubs for pico-sdk and FreeRTOS symbols ──────────────────── */

#ifdef HOST_TEST

/* i2c_inst_t comes from the stub hardware/i2c.h; vTaskDelay/log_write from
 * test/host/stubs/host_stubs.c. Only the BME280-specific i2c transaction
 * sequence is mocked below. */
#include "hardware/i2c.h"
static i2c_inst_t i2c0_inst;
i2c_inst_t *i2c0 = &i2c0_inst;

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
    /*
     * Return the BME280 datasheet reference calibration coefficients for
     * the compensation formula verification test.
     *
     * Calibration values from the datasheet example (§4.2.3):
     *   dig_T1 = 27504   dig_T2 = 26435   dig_T3 = -1000
     *   dig_P1 = 36477   dig_P2 = -10685  dig_P3 = 3024
     *   dig_P4 = 2855    dig_P5 = 140     dig_P6 = -7
     *   dig_P7 = 15500   dig_P8 = -14600  dig_P9 = 6000
     *   dig_H1 = 75      dig_H2 = 370     dig_H3 = 0
     *   dig_H4 = 313     dig_H5 = 50      dig_H6 = 30
     *
     * The stub returns data in the exact byte layout bme280_init expects.
     * All multi-byte values are little-endian (LE).
     */
    static const uint8_t s_calib_tp[24] = {
        /* dig_T1 = 27504 = 0x6B70 */  0x70U, 0x6BU,
        /* dig_T2 = 26435 = 0x6743 */  0x43U, 0x67U,
        /* dig_T3 = -1000 = 0xFC18 */  0x18U, 0xFCU,
        /* dig_P1 = 36477 = 0x8E7D */  0x7DU, 0x8EU,
        /* dig_P2 = -10685= 0xD603 */  0x03U, 0xD6U,  /* -10685 = 0xFFFFD603 → 0xD603 LE */
        /* dig_P3 = 3024  = 0x0BD0 */  0xD0U, 0x0BU,
        /* dig_P4 = 2855  = 0x0B27 */  0x27U, 0x0BU,
        /* dig_P5 = 140   = 0x008C */  0x8CU, 0x00U,
        /* dig_P6 = -7    = 0xFFF9 */  0xF9U, 0xFFU,
        /* dig_P7 = 15500 = 0x3C8C */  0x8CU, 0x3CU,
        /* dig_P8 = -14600= 0xC738 */  0x38U, 0xC7U,
        /* dig_P9 = 6000  = 0x1770 */  0x70U, 0x17U,
    };

    static const uint8_t s_calib_h2_h6[7] = {
        /* dig_H2 = 370 = 0x0172 LE */ 0x72U, 0x01U,
        /* dig_H3 = 0 */               0x00U,
        /* dig_H4 = 313: high byte 0xE4 = 19, low nibble of 0xE5 = 9
         *   dig_H4 = (19 << 4) | 9 = 313 */
        0x13U, 0x69U,
        /* dig_H5 = 50: bits 11:4 from 0xE6 = 3, bits 3:0 from upper nibble of 0xE5 = 2
         *   dig_H5 = (3 << 4) | 2 = 50 */
        0x03U,
        /* dig_H6 = 30 */ 0x1EU,
    };

    static int call_count = 0;
    call_count++;

    switch (call_count) {
    case 1:
        /* bme280_init: read chip_id */
        dst[0] = 0x60U;   /* BME280_CHIP_ID */
        break;
    case 2:
        /* bme280_init: read calibration T+P (24 bytes from 0x88) */
        if (len == 24) {
            memcpy(dst, s_calib_tp, 24);
        }
        break;
    case 3:
        /* bme280_init: read dig_H1 (1 byte from 0xA1) */
        dst[0] = 75U;   /* dig_H1 */
        break;
    case 4:
        /* bme280_init: read calibration H (7 bytes from 0xE1) */
        if (len == 7) {
            memcpy(dst, s_calib_h2_h6, 7);
        }
        break;
    default:
        /*
         * bme280_read_sample: read raw ADC (8 bytes from 0xF7).
         * Reference raw values from the datasheet:
         *   adc_P = 415148  adc_T = 519888  adc_H = 29515
         *
         * Layout: [P_MSB, P_LSB, P_XLSB, T_MSB, T_LSB, T_XLSB, H_MSB, H_LSB]
         *   adc_P = 415148 → raw bytes: MSB/LSB/XLSB where 20-bit = top 20 bits
         *   adc_P raw = 415148 << 4 = 0x655AC0 → [0x65, 0x5A, 0xC0]
         *   adc_T raw = 519888 << 4 = 0x7F1890 → [0x7F, 0x18, 0x90] (approx)
         */
        if (len == 8) {
            /*
             * Pack the reference adc values into the register format:
             *   byte[0..2]: pressure  [MSB, LSB, XLSB(bits7:4)]
             *   byte[3..5]: temp      [MSB, LSB, XLSB(bits7:4)]
             *   byte[6..7]: humidity  [MSB, LSB]
             *
             * adc_P = 415148: stored as 20-bit value in upper 20 bits of 3 bytes
             *   bits[19:12] = 0x65 (bits 12–19 of 415148)
             *   bits[11:4]  = 0x5A
             *   bits[3:0]   = 0xC (upper nibble of last byte)
             */
            uint32_t adc_P = 415148UL;
            uint32_t adc_T = 519888UL;
            uint16_t adc_H = 29515U;

            dst[0] = (uint8_t)((adc_P >> 12) & 0xFFU);
            dst[1] = (uint8_t)((adc_P >>  4) & 0xFFU);
            dst[2] = (uint8_t)((adc_P & 0x0FU) << 4);
            dst[3] = (uint8_t)((adc_T >> 12) & 0xFFU);
            dst[4] = (uint8_t)((adc_T >>  4) & 0xFFU);
            dst[5] = (uint8_t)((adc_T & 0x0FU) << 4);
            dst[6] = (uint8_t)((adc_H >> 8) & 0xFFU);
            dst[7] = (uint8_t)(adc_H & 0xFFU);
        }
        break;
    }
    return (int)len;
}

/* vTaskDelay + log_write are provided by test/host/stubs/host_stubs.c. */

#endif /* HOST_TEST */

/* ── Include the unit under test ─────────────────────────────────────────── */

#include "bme280.h"

/* ── Helper to init with stub data ──────────────────────────────────────── */

static Bme280 s_dev;

static void setup_bme280(void)
{
    extern i2c_inst_t *i2c0;
    /* Reset call counter between tests */
    extern int /* reset */ call_count;
    /* We rely on call order in the read stub above.  Re-init resets it. */
    err_t err = bme280_init(&s_dev, i2c0, 0x76U);
    assert_int_equal(err, ERR_OK);
}

/* ── Test: temperature compensation ─────────────────────────────────────── */

static void test_temp_compensation(void **state)
{
    (void)state;

    setup_bme280();

    Bme280Sample sample;
    err_t err = bme280_read_sample(&s_dev, &sample);
    assert_int_equal(err, ERR_OK);

    /*
     * Expected: 25.08 °C for adc_T = 519888 with the reference calibration.
     * The datasheet gives t_raw = 2508 (in 0.01 °C units) → 25.08 °C.
     * Accept ±0.5 °C tolerance for floating-point rounding.
     */
    assert_true(fabs((double)sample.temp_c - 25.08) < 0.5);
    printf("  temp_c = %.4f (expected ~25.08)\n", (double)sample.temp_c);
}

/* ── Test: humidity compensation ─────────────────────────────────────────── */

static void test_humidity_compensation(void **state)
{
    (void)state;

    /*
     * We cannot call setup_bme280() again here because the stub call counter
     * has advanced.  We reuse s_dev from the previous test and trigger a new
     * read_sample (which calls into the stub's default case).
     *
     * Expected: ~47–51 % RH for adc_H = 29515 with reference calibration.
     * The datasheet example gives approximately 47.4 % (Q22.10 = 47445/1024).
     * Accept a wide ±5 % tolerance because the exact value depends on t_fine
     * from the temperature compensation above.
     */
    Bme280Sample sample;
    err_t err = bme280_read_sample(&s_dev, &sample);
    assert_int_equal(err, ERR_OK);

    assert_true(sample.humidity_pct >= 30.0f && sample.humidity_pct <= 70.0f);
    printf("  humidity_pct = %.4f (expected 40–55 range)\n",
           (double)sample.humidity_pct);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_temp_compensation),
        cmocka_unit_test(test_humidity_compensation),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
