/* Host unit tests for the 24 GHz HMMD frame parser (ADR-0007) — the second
 * radar behind the radar_driver_t v-table. Builds real frames:
 *
 *   [0x53][0x59][CTRL][CMD][LEN_H][LEN_L] DATA(LEN, big-endian length) [CKSUM][0x54][0x43]
 *   CKSUM = (sum of every byte from 0x53 through the last DATA byte) & 0xFF
 *
 * and drives them through the public read_sample via the stub UART byte feed
 * (test/host/stubs), exactly like test_radar_bha2.c. The (CTRL,CMD) codes here
 * match the named constants the driver decodes; if those are corrected at the
 * bench, both move together and these tests stay meaningful — they verify the
 * envelope (header/length/checksum/tail) and the field wiring, not magic codes.
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

/* (CTRL,CMD) report frames — mirror of radar_hmmd.c's constants. */
#define CTRL_PRESENCE 0x80
#define CMD_PRESENCE  0x01
#define CMD_DISTANCE  0x0A
#define CTRL_RESP     0x81
#define CMD_RESP_RATE 0x05
#define CTRL_HEART    0x85
#define CMD_HEART     0x03

/* ── frame builder ───────────────────────────────────────────────────────── */

/* Append one HMMD frame to buf; returns new length. */
static size_t put_frame(uint8_t *buf, size_t off, uint8_t ctrl, uint8_t cmd,
                        const uint8_t *data, uint16_t len) {
    uint8_t *f = buf + off;
    f[0] = 0x53; f[1] = 0x59;
    f[2] = ctrl; f[3] = cmd;
    f[4] = (uint8_t)(len >> 8); f[5] = (uint8_t)(len & 0xFF);
    memcpy(f + 6, data, len);
    uint32_t sum = 0;
    for (size_t i = 0; i < 6u + len; i++) sum += f[i];
    f[6 + len] = (uint8_t)(sum & 0xFF);
    f[7 + len] = 0x54; f[8 + len] = 0x43;
    return off + 9 + len;
}

static uart_inst_t s_uart;

static RadarSample drive(const uint8_t *frames, size_t len) {
    radar_driver_t *drv = radar_hmmd_driver();
    /* Static context + init() early-returns once initialised (correct for
     * production). close() first so each test starts from a clean latch. */
    drv->close(drv->ctx);
    drv->init(drv->ctx, &s_uart);
    host_time_reset();
    host_uart_load(frames, len);
    RadarSample s; memset(&s, 0, sizeof(s));
    drv->read_sample(drv->ctx, &s, 500);
    return s;
}

/* ── tests ───────────────────────────────────────────────────────────────── */

static void test_presence_breath_heart(void **state) {
    (void)state;
    uint8_t buf[64]; size_t n = 0;
    uint8_t d1[1] = {1};   n = put_frame(buf, n, CTRL_PRESENCE, CMD_PRESENCE, d1, 1);
    uint8_t d2[1] = {18};  n = put_frame(buf, n, CTRL_RESP,     CMD_RESP_RATE, d2, 1);
    uint8_t d3[1] = {70};  n = put_frame(buf, n, CTRL_HEART,    CMD_HEART,     d3, 1);

    RadarSample s = drive(buf, n);
    assert_true(s.presence);
    assert_true(fabsf(s.breath_rpm - 18.0f) < 0.01f);
    assert_true(fabsf(s.heart_bpm  - 70.0f) < 0.01f);
    assert_int_equal(s.q, 0);                 /* fresh vitals → ok */
    assert_false(s.resp_motion_amp_valid);    /* no phase stream on HMMD */
}

static void test_distance_cm_to_mm(void **state) {
    (void)state;
    uint8_t buf[64]; size_t n = 0;
    uint8_t pd[2] = {0x00, 0xF0};   /* BE16 = 240 cm */
    n = put_frame(buf, n, CTRL_PRESENCE, CMD_DISTANCE, pd, 2);

    RadarSample s = drive(buf, n);
    assert_int_equal(s.distance_mm, 2400);    /* 240 cm → 2400 mm */
}

static void test_presence_only_is_degraded(void **state) {
    (void)state;
    uint8_t buf[32]; size_t n = 0;
    uint8_t d1[1] = {1};
    n = put_frame(buf, n, CTRL_PRESENCE, CMD_PRESENCE, d1, 1);

    RadarSample s = drive(buf, n);
    assert_true(s.presence);
    assert_true(s.breath_rpm == 0.0f);
    assert_true(s.heart_bpm  == 0.0f);
    assert_int_equal(s.q, 2);                 /* present, no vitals → degraded */
}

static void test_bad_checksum_rejected(void **state) {
    (void)state;
    uint8_t buf[32]; size_t n = 0;
    uint8_t d2[1] = {18};
    n = put_frame(buf, n, CTRL_RESP, CMD_RESP_RATE, d2, 1);
    buf[n - 3] ^= 0xFF;                        /* corrupt CKSUM (before tail) */

    RadarSample s = drive(buf, n);
    assert_true(s.breath_rpm == 0.0f);        /* frame dropped, nothing fresh */
    assert_int_equal(s.q, 3);
}

static void test_bad_tail_rejected(void **state) {
    (void)state;
    uint8_t buf[32]; size_t n = 0;
    uint8_t d2[1] = {18};
    n = put_frame(buf, n, CTRL_RESP, CMD_RESP_RATE, d2, 1);
    buf[n - 1] ^= 0xFF;                        /* corrupt the second tail byte */

    RadarSample s = drive(buf, n);
    assert_true(s.breath_rpm == 0.0f);
    assert_int_equal(s.q, 3);
}

/* A 0x53 0x53 0x59 run must still sync (the sliding-window header hunt). */
static void test_resync_double_sof(void **state) {
    (void)state;
    uint8_t buf[40]; size_t n = 0;
    buf[n++] = 0x53;                          /* stray byte before a real frame */
    uint8_t d2[1] = {16};
    n = put_frame(buf, n, CTRL_RESP, CMD_RESP_RATE, d2, 1);

    RadarSample s = drive(buf, n);
    assert_true(fabsf(s.breath_rpm - 16.0f) < 0.01f);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_presence_breath_heart),
        cmocka_unit_test(test_distance_cm_to_mm),
        cmocka_unit_test(test_presence_only_is_degraded),
        cmocka_unit_test(test_bad_checksum_rejected),
        cmocka_unit_test(test_bad_tail_rejected),
        cmocka_unit_test(test_resync_double_sof),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
