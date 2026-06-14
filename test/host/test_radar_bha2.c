/* Host unit tests for the MR60BHA2 frame parser — the most logic-dense driver,
 * and where a framing/checksum bug actually bit us on hardware (the original
 * Andar-framing assumption). Builds real frames:
 *
 *   [0x01][SEQ_H][SEQ_L][LEN_H][LEN_L][TYPE_H][TYPE_L][HDR_CKSUM]
 *   payload(LEN, big-endian length)  [DATA_CKSUM]
 *   HDR_CKSUM  = ~XOR(header[0..6])
 *   DATA_CKSUM = ~XOR(payload)
 *
 * and drives them through the public read_sample via the stub UART byte feed
 * (test/host/stubs). Frame types: 0x0A14 breath, 0x0A15 heart, 0x0A16 distance.
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

#include "host_stubs.h"
#include "radar_driver.h"

/* ── frame builders ──────────────────────────────────────────────────────── */
static uint8_t inv_xor(const uint8_t *p, size_t n) {
    uint8_t x = 0; for (size_t i = 0; i < n; i++) x ^= p[i]; return (uint8_t)~x;
}

/* Append a frame of `type` carrying `payload` to buf; returns new length. */
static size_t put_frame(uint8_t *buf, size_t off, uint16_t seq, uint16_t type,
                        const uint8_t *payload, uint16_t len) {
    uint8_t *f = buf + off;
    f[0] = 0x01;
    f[1] = (uint8_t)(seq >> 8);  f[2] = (uint8_t)(seq & 0xFF);
    f[3] = (uint8_t)(len >> 8);  f[4] = (uint8_t)(len & 0xFF);
    f[5] = (uint8_t)(type >> 8); f[6] = (uint8_t)(type & 0xFF);
    f[7] = inv_xor(f, 7);
    memcpy(f + 8, payload, len);
    f[8 + len] = inv_xor(payload, len);
    return off + 8 + len + 1;
}

static void le_float(uint8_t *p, float v) { memcpy(p, &v, 4); }
static void le_u32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}

static uart_inst_t s_uart;

static RadarSample drive(const uint8_t *frames, size_t len) {
    radar_driver_t *drv = radar_bha2_driver();
    /* The driver context is static and init() early-returns once initialised
     * (correct for production — init runs once). close() first so each test
     * starts from a clean latched state. */
    drv->close(drv->ctx);
    drv->init(drv->ctx, &s_uart);
    host_time_reset();
    host_uart_load(frames, len);
    RadarSample s; memset(&s, 0, sizeof(s));
    drv->read_sample(drv->ctx, &s, 500);
    return s;
}

/* Drive `n` 0x0A13 phase frames `step_ms` apart, breath phase taken from
 * vals[], and return the final sample's amplitude fields. The clock is jumped
 * forward each round (then auto-advance re-enabled so read_sample still hits
 * its deadline) so the samples span real time — the driver's amplitude window
 * needs a multi-second span (ADR-0006). */
static RadarSample drive_phase(const float *vals, int n, uint32_t step_ms) {
    radar_driver_t *drv = radar_bha2_driver();
    drv->close(drv->ctx);
    drv->init(drv->ctx, &s_uart);
    host_time_reset();
    RadarSample s; memset(&s, 0, sizeof(s));
    for (int i = 0; i < n; i++) {
        host_time_set_ms((uint32_t)i * step_ms);  /* jump clock forward (pins) */
        host_time_auto(1);                         /* resume +1ms/query        */
        uint8_t buf[32];
        uint8_t pf[12];
        le_float(pf + 0, 1.0f);        /* total phase (unused) */
        le_float(pf + 4, vals[i]);     /* breath phase         */
        le_float(pf + 8, 2.0f);        /* heart phase (unused) */
        size_t m = put_frame(buf, 0, (uint16_t)(i + 1), 0x0A13, pf, 12);
        host_uart_load(buf, m);
        drv->read_sample(drv->ctx, &s, 500);
    }
    return s;
}

/* ── tests ───────────────────────────────────────────────────────────────── */
static void test_breath_and_heart(void **state) {
    (void)state;
    uint8_t buf[64]; size_t n = 0;
    uint8_t pf[4];
    le_float(pf, 16.5f); n = put_frame(buf, n, 1, 0x0A14, pf, 4);   /* breath 16.5 */
    le_float(pf, 72.0f); n = put_frame(buf, n, 2, 0x0A15, pf, 4);   /* heart  72.0 */

    RadarSample s = drive(buf, n);
    assert_true(fabsf(s.breath_rpm - 16.5f) < 0.01f);
    assert_true(fabsf(s.heart_bpm  - 72.0f) < 0.01f);
    assert_int_equal(s.q, 0);          /* fresh metrics → ok */
    assert_true(s.presence);           /* frames arrived → presence */
}

static void test_distance_cm_to_mm(void **state) {
    (void)state;
    uint8_t buf[64]; size_t n = 0;
    uint8_t pd[8];
    le_u32(pd, 1);            /* flag = 1 (valid target) */
    le_float(pd + 4, 24.0f);  /* 24.0 cm */
    n = put_frame(buf, n, 1, 0x0A16, pd, 8);

    RadarSample s = drive(buf, n);
    assert_int_equal(s.distance_mm, 240);   /* 24.0 cm → 240 mm */
}

static void test_bad_data_checksum_rejected(void **state) {
    (void)state;
    uint8_t buf[64]; size_t n = 0;
    uint8_t pf[4]; le_float(pf, 16.5f);
    n = put_frame(buf, n, 1, 0x0A14, pf, 4);
    buf[n - 1] ^= 0xFF;        /* corrupt the data checksum */

    RadarSample s = drive(buf, n);
    /* frame rejected → no fresh breath → driver reports invalid */
    assert_true(s.breath_rpm == 0.0f);
    assert_int_equal(s.q, 3);
}

static void test_bad_header_checksum_rejected(void **state) {
    (void)state;
    uint8_t buf[64]; size_t n = 0;
    uint8_t pf[4]; le_float(pf, 16.5f);
    n = put_frame(buf, n, 1, 0x0A14, pf, 4);
    buf[7] ^= 0xFF;            /* corrupt the header checksum */

    RadarSample s = drive(buf, n);
    assert_true(s.breath_rpm == 0.0f);
    assert_int_equal(s.q, 3);
}

/* Phase-based hold detection (ADR-0006): an oscillating breath phase yields a
 * large peak-to-peak amplitude reported as valid. */
static void test_phase_amplitude_breathing(void **state) {
    (void)state;
    const float vals[8] = {0.0f, 3.0f, 0.0f, 3.0f, 0.0f, 3.0f, 0.0f, 3.0f};
    RadarSample s = drive_phase(vals, 8, 700);
    assert_true(s.resp_motion_amp_valid);
    assert_true(fabsf(s.resp_motion_amp - 3.0f) < 0.01f);   /* p2p = 3 */
}

/* A flat breath phase (no chest motion) yields ~0 amplitude — still valid; this
 * is the signal the filter thresholds into a breath-hold. */
static void test_phase_amplitude_flat(void **state) {
    (void)state;
    const float vals[8] = {5.0f, 5.0f, 5.0f, 5.0f, 5.0f, 5.0f, 5.0f, 5.0f};
    RadarSample s = drive_phase(vals, 8, 700);
    assert_true(s.resp_motion_amp_valid);
    assert_true(s.resp_motion_amp < 0.01f);                 /* p2p ~ 0 */
}

/* Too few phase samples ⇒ amplitude not trusted (no false flat). */
static void test_phase_amplitude_too_few_invalid(void **state) {
    (void)state;
    const float vals[3] = {0.0f, 3.0f, 0.0f};
    RadarSample s = drive_phase(vals, 3, 700);
    assert_false(s.resp_motion_amp_valid);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_breath_and_heart),
        cmocka_unit_test(test_distance_cm_to_mm),
        cmocka_unit_test(test_bad_data_checksum_rejected),
        cmocka_unit_test(test_bad_header_checksum_rejected),
        cmocka_unit_test(test_phase_amplitude_breathing),
        cmocka_unit_test(test_phase_amplitude_flat),
        cmocka_unit_test(test_phase_amplitude_too_few_invalid),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
