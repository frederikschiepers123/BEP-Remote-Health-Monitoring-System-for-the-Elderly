/* Host unit tests for the ENS160 driver:
 *   - validity-flag extraction (status bits 3:2 → NORMAL/WARMUP/INITIAL/INVALID)
 *   - 6-byte STATUS burst decode (AQI + little-endian TVOC + eCO2)
 *   - init verifies PART_ID and that the chip entered STANDARD mode
 *
 * The mock tracks the register pointer set by read_regs' 1-byte write so it can
 * answer PART_ID / OPMODE / STATUS reads correctly. pico-sdk / FreeRTOS / log
 * symbols come from test/host/stubs. */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "cmocka.h"

#include "ens160.h"   /* pulls stub hardware/i2c.h */

/* Mocked register file. */
static uint8_t s_reg_ptr   = 0;
static uint8_t s_opmode    = 0x02;   /* what an OPMODE readback returns */
static uint8_t s_status    = 0x02;
static uint8_t s_aqi       = 2;
static uint16_t s_tvoc     = 194;
static uint16_t s_co2      = 685;

int i2c_write_blocking(i2c_inst_t *i2c, uint8_t addr,
                       const uint8_t *src, size_t len, bool nostop) {
    (void)i2c; (void)addr; (void)nostop;
    if (len == 1) {                  /* register-pointer set (read_regs) */
        s_reg_ptr = src[0];
    }
    /* OPMODE writes (len==2, reg 0x10) are accepted but do NOT change what the
     * readback returns — the test controls s_opmode so it can simulate a chip
     * that never enters STANDARD mode. */
    return (int)len;
}

int i2c_read_blocking(i2c_inst_t *i2c, uint8_t addr,
                      uint8_t *dst, size_t len, bool nostop) {
    (void)i2c; (void)addr; (void)nostop;
    switch (s_reg_ptr) {
    case 0x00:                        /* PART_ID, 2 bytes LE = 0x0160 */
        assert_int_equal(len, 2); dst[0] = 0x60; dst[1] = 0x01; break;
    case 0x10:                        /* OPMODE readback */
        assert_int_equal(len, 1); dst[0] = s_opmode; break;
    case 0x20:                        /* STATUS burst: status,aqi,tvoc_l,tvoc_h,co2_l,co2_h */
        assert_int_equal(len, 6);
        dst[0] = s_status; dst[1] = s_aqi;
        dst[2] = (uint8_t)(s_tvoc & 0xFF); dst[3] = (uint8_t)(s_tvoc >> 8);
        dst[4] = (uint8_t)(s_co2  & 0xFF); dst[5] = (uint8_t)(s_co2  >> 8);
        break;
    default:
        memset(dst, 0, len); break;
    }
    return (int)len;
}

static i2c_inst_t s_i2c;

/* ── validity extraction (pure, no i2c) ──────────────────────────────────── */
static void test_validity_bits(void **state) {
    (void)state;
    assert_int_equal(ens160_validity(0x00), ENS160_VALIDITY_NORMAL);
    assert_int_equal(ens160_validity(0x04), ENS160_VALIDITY_WARMUP);          /* bits 3:2 = 01 */
    assert_int_equal(ens160_validity(0x08), ENS160_VALIDITY_INITIAL_STARTUP); /* bits 3:2 = 10 */
    assert_int_equal(ens160_validity(0x0C), ENS160_VALIDITY_INVALID);         /* bits 3:2 = 11 */
    /* other bits must not bleed in */
    assert_int_equal(ens160_validity(0x83), ENS160_VALIDITY_NORMAL);
}

/* ── init + sample decode ────────────────────────────────────────────────── */
static void test_init_and_decode(void **state) {
    (void)state;
    Ens160 dev;
    assert_int_equal(ens160_init(&dev, &s_i2c, 0x53), ERR_OK);

    Ens160Sample s;
    assert_int_equal(ens160_read_sample(&dev, &s), ERR_OK);
    assert_int_equal(s.aqi, 2);
    assert_int_equal(s.tvoc_ppb, 194);
    assert_int_equal(s.co2_ppm, 685);
    assert_int_equal(ens160_validity(s.status), ENS160_VALIDITY_NORMAL);
}

/* ── init fails loudly if the chip never enters STANDARD mode ─────────────── */
static void test_init_opmode_stuck_fails(void **state) {
    (void)state;
    s_opmode = 0x00;                  /* readback never reaches STANDARD */
    Ens160 dev;
    assert_int_equal(ens160_init(&dev, &s_i2c, 0x53), ERR_IO);
    s_opmode = 0x02;                  /* restore for any later test */
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_validity_bits),
        cmocka_unit_test(test_init_and_decode),
        cmocka_unit_test(test_init_opmode_stuck_fails),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
