/* Host unit tests for the Waveshare HMMD report-mode frame parser (ADR-0007) —
 * the second radar behind the radar_driver_t v-table. Protocol per the previous
 * BAP group's reference driver (CLAUDE.md §18,
 * bestanden_vorige_BAP/.../lib/hmmd_mpy.py):
 *
 *   [F4 F3 F2 F1] [LEN(2, little-endian)] PAYLOAD(LEN) [F8 F7 F6 F5]
 *   payload[0]     presence (0/1)
 *   payload[1..2]  distance_raw (little-endian)
 *   payload[3..]   16 × 2-byte gate energies (little-endian)
 *   (no checksum — header + tail + length delimit the frame)
 *
 * Frames are driven through the public read_sample via the stub UART byte feed
 * (test/host/stubs), exactly like test_radar_bha2.c.
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

/* Build one HMMD report frame: presence + distance + 16 gate energies. */
static size_t put_frame(uint8_t *buf, size_t off, uint8_t presence,
                        uint16_t distance_raw, uint16_t energy) {
    uint8_t *f = buf + off;
    f[0] = 0xF4; f[1] = 0xF3; f[2] = 0xF2; f[3] = 0xF1;
    uint16_t len = 1 + 2 + 16 * 2;          /* 35-byte payload */
    f[4] = (uint8_t)(len & 0xFF); f[5] = (uint8_t)(len >> 8);   /* LE length */
    uint8_t *p = f + 6;
    p[0] = presence;
    p[1] = (uint8_t)(distance_raw & 0xFF); p[2] = (uint8_t)(distance_raw >> 8);
    for (int i = 0; i < 16; i++) {
        p[3 + 2 * i]     = (uint8_t)(energy & 0xFF);
        p[3 + 2 * i + 1] = (uint8_t)(energy >> 8);
    }
    uint8_t *tail = p + len;
    tail[0] = 0xF8; tail[1] = 0xF7; tail[2] = 0xF6; tail[3] = 0xF5;
    return off + 4 + 2 + len + 4;
}

static uart_inst_t s_uart;

static RadarSample drive(const uint8_t *frames, size_t len) {
    radar_driver_t *drv = radar_hmmd_driver();
    /* Static context + init() early-returns once initialised. close() first so
     * each test starts from a clean latch. */
    drv->close(drv->ctx);
    drv->init(drv->ctx, &s_uart);
    host_time_reset();
    host_uart_load(frames, len);
    RadarSample s; memset(&s, 0, sizeof(s));
    drv->read_sample(drv->ctx, &s, 500);
    return s;
}

/* ── tests ───────────────────────────────────────────────────────────────── */

static void test_presence_and_distance(void **state) {
    (void)state;
    uint8_t buf[64];
    size_t n = put_frame(buf, 0, 1, 240, 30000);   /* present, 240 cm, energy */

    RadarSample s = drive(buf, n);
    assert_true(s.presence);
    assert_int_equal(s.distance_mm, 2400);          /* 240 cm → 2400 mm */
    assert_int_equal(s.q, 0);
    /* HMMD has no vitals — always null / inert. */
    assert_true(s.breath_rpm == 0.0f);
    assert_true(s.heart_bpm  == 0.0f);
    assert_false(s.resp_motion_amp_valid);
}

static void test_no_target_clears_distance(void **state) {
    (void)state;
    uint8_t buf[64];
    size_t n = put_frame(buf, 0, 0, 999, 100);      /* presence=0 */

    RadarSample s = drive(buf, n);
    assert_false(s.presence);
    assert_int_equal(s.distance_mm, 0);             /* no target → no distance */
    assert_int_equal(s.q, 0);                        /* a valid frame is still ok */
}

static void test_bad_tail_rejected(void **state) {
    (void)state;
    uint8_t buf[64];
    size_t n = put_frame(buf, 0, 1, 100, 50);
    buf[n - 1] ^= 0xFF;                              /* corrupt last tail byte */

    RadarSample s = drive(buf, n);
    assert_false(s.presence);                        /* frame dropped */
    assert_int_equal(s.q, 3);
}

static void test_resync_after_garbage(void **state) {
    (void)state;
    uint8_t buf[80];
    /* Leading noise, including a lone header byte, before a real frame. */
    buf[0] = 0x00; buf[1] = 0xF4; buf[2] = 0x11; buf[3] = 0xF4;
    size_t n = put_frame(buf, 4, 1, 150, 20);

    RadarSample s = drive(buf, n);
    assert_true(s.presence);
    assert_int_equal(s.distance_mm, 1500);          /* 150 cm → 1500 mm */
}

static void test_two_frames_latest_wins(void **state) {
    (void)state;
    uint8_t buf[160];
    size_t n = put_frame(buf, 0, 1, 100, 10);
    n = put_frame(buf, n, 1, 320, 10);              /* newer: 320 cm */

    RadarSample s = drive(buf, n);
    assert_true(s.presence);
    assert_int_equal(s.distance_mm, 3200);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_presence_and_distance),
        cmocka_unit_test(test_no_target_clears_distance),
        cmocka_unit_test(test_bad_tail_rejected),
        cmocka_unit_test(test_resync_after_garbage),
        cmocka_unit_test(test_two_frames_latest_wins),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
