/* Host unit tests for the ENS160 driver:
 *   - validity-flag extraction (status bits 3:2 → NORMAL/WARMUP/INITIAL/INVALID)
 *   - 6-byte STATUS burst decode (AQI + little-endian TVOC + eCO2)
 *   - init verifies PART_ID and that the chip entered STANDARD mode
 *   - compensation encoding: TEMP_IN = Kelvin × 64, RH_IN = %rH × 512, byte-
 *     exact (regression: ×256 overflowed uint16 for any ambient > −17 °C,
 *     feeding the chip −128 °C — the 2026-06-12 STATUS=0x03 root cause)
 *   - not-operating recovery is debounced: OPMODE rewrite only on the 2nd
 *     consecutive STATAS-clear read (one rewrite restarts the chip's warm-up)
 *
 * The mock tracks the register pointer set by read_regs' 1-byte write so it can
 * answer PART_ID / OPMODE / STATUS reads correctly, captures the last
 * multi-byte register write (compensation), and counts OPMODE writes.
 * pico-sdk / FreeRTOS / log symbols come from test/host/stubs. */

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
static uint8_t s_status    = 0x82;   /* STATAS set (running) + NEWDAT, validity NORMAL */
static uint8_t s_aqi       = 2;
static uint16_t s_tvoc     = 194;
static uint16_t s_co2      = 685;

/* Captured last multi-byte write: target register + payload. */
static uint8_t s_last_wreg            = 0xFF;
static uint8_t s_last_wdata[8]        = { 0 };
static size_t  s_last_wlen            = 0;
static int     s_opmode_write_count   = 0;   /* writes to OPMODE (0x10) */

int i2c_write_blocking(i2c_inst_t *i2c, uint8_t addr,
                       const uint8_t *src, size_t len, bool nostop) {
    (void)i2c; (void)addr; (void)nostop;
    if (len == 1) {                  /* register-pointer set (read_regs) */
        s_reg_ptr = src[0];
    } else if (len >= 2) {           /* register write: src[0]=reg, rest=data */
        s_last_wreg = src[0];
        s_last_wlen = len - 1;
        if (s_last_wlen > sizeof(s_last_wdata)) s_last_wlen = sizeof(s_last_wdata);
        memcpy(s_last_wdata, src + 1, s_last_wlen);
        if (src[0] == 0x10) s_opmode_write_count++;
    }
    /* OPMODE writes are accepted but do NOT change what the readback returns —
     * the test controls s_opmode so it can simulate a chip that never enters
     * STANDARD mode. */
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

/* ── operating / not-operating distinction (the STATUS=0x00 fault) ────────── */
static void test_is_operating(void **state) {
    (void)state;
    assert_true(ens160_is_operating(0x82));   /* STATAS set */
    assert_false(ens160_is_operating(0x00));  /* the fault: validity bits read NORMAL but chip is stopped */
    assert_false(ens160_is_operating(0x0A));  /* validity bits set but STATAS clear → still not running */
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
    assert_true(ens160_is_operating(s.status));
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

/* ── compensation encoding: byte-exact TEMP_IN (K×64) / RH_IN (%rH×512) ───── */
static void test_compensation_encoding(void **state) {
    (void)state;
    Ens160 dev = { .i2c = &s_i2c, .addr = 0x53, .statas_clear_count = 0 };

    /* 25.0 °C / 50 %rH: 298.15 K × 64 = 19081 = 0x4A89; 50 × 512 = 25600 = 0x6400 */
    assert_int_equal(ens160_set_compensation(&dev, 25.0f, 50.0f), ERR_OK);
    assert_int_equal(s_last_wreg, 0x13);       /* TEMP_IN, 4-byte burst */
    assert_int_equal(s_last_wlen, 4);
    assert_int_equal(s_last_wdata[0], 0x89);
    assert_int_equal(s_last_wdata[1], 0x4A);
    assert_int_equal(s_last_wdata[2], 0x00);
    assert_int_equal(s_last_wdata[3], 0x64);

    /* Regression: 19.0 °C / 78 %rH — the old ×256 encoding wrapped this to
     * −128.6 °C. 292.15 × 64 = 18697 = 0x4909; 78 × 512 = 39936 = 0x9C00. */
    assert_int_equal(ens160_set_compensation(&dev, 19.0f, 78.0f), ERR_OK);
    assert_int_equal(s_last_wdata[0], 0x09);
    assert_int_equal(s_last_wdata[1], 0x49);
    assert_int_equal(s_last_wdata[2], 0x00);
    assert_int_equal(s_last_wdata[3], 0x9C);

    /* Clamps: 200 °C → 85 °C (358.15 × 64 = 22921 = 0x5989);
     *         150 %  → 100 % (51200 = 0xC800). */
    assert_int_equal(ens160_set_compensation(&dev, 200.0f, 150.0f), ERR_OK);
    assert_int_equal(s_last_wdata[0], 0x89);
    assert_int_equal(s_last_wdata[1], 0x59);
    assert_int_equal(s_last_wdata[2], 0x00);
    assert_int_equal(s_last_wdata[3], 0xC8);
}

/* ── recovery debounce: OPMODE rewrite only on 2nd consecutive STATAS-clear ── */
static void test_recovery_debounced(void **state) {
    (void)state;
    Ens160 dev = { .i2c = &s_i2c, .addr = 0x53, .statas_clear_count = 0 };
    Ens160Sample s;

    s_status = 0x03;                  /* STATAS clear: chip not operating */
    s_opmode_write_count = 0;

    assert_int_equal(ens160_read_sample(&dev, &s), ERR_OK);
    assert_int_equal(s_opmode_write_count, 0);   /* 1st clear: no rewrite */

    assert_int_equal(ens160_read_sample(&dev, &s), ERR_OK);
    assert_int_equal(s_opmode_write_count, 1);   /* 2nd consecutive: rewrite */

    assert_int_equal(ens160_read_sample(&dev, &s), ERR_OK);
    assert_int_equal(s_opmode_write_count, 1);   /* counter reset: not every read */

    /* a good read resets the debounce: next single glitch doesn't rewrite */
    s_status = 0x82;
    assert_int_equal(ens160_read_sample(&dev, &s), ERR_OK);
    s_status = 0x03;
    assert_int_equal(ens160_read_sample(&dev, &s), ERR_OK);
    assert_int_equal(s_opmode_write_count, 1);

    s_status = 0x82;                  /* restore for any later test */
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_validity_bits),
        cmocka_unit_test(test_is_operating),
        cmocka_unit_test(test_init_and_decode),
        cmocka_unit_test(test_init_opmode_stuck_fails),
        cmocka_unit_test(test_compensation_encoding),
        cmocka_unit_test(test_recovery_debounced),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
