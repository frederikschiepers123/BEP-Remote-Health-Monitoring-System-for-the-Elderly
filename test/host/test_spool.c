/* Host unit tests for components/spool (the NV outbound FIFO, ADR-0003).
 *
 * The spool's flash HAL is a RAM-backed fake (inside spool.c under HOST_TEST)
 * that enforces NOR program rules — programming a non-erased page trips an
 * assert — so any logic bug that would corrupt real flash fails a test here.
 * The fake image persists across spool_mount() calls, which is how the
 * power-loss / reboot cases are simulated.
 *
 * Geometry mirrors spool.c on a 4 MB part: 1 MB region / 256 B per slot = 4096
 * slots, 16 slots per 4 KB sector, capacity = 4096 - 16 = 4080 undelivered. */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <cmocka.h>

#include "spool.h"

/* Host-only hooks exported by spool.c (HOST_TEST build). */
void     spool_host_flash_reset(void);
uint8_t *spool_host_flash_ptr(void);

#define SLOT_SIZE   256u
#define SECTOR_SIZE 4096u
#define S_PER_SECT  (SECTOR_SIZE / SLOT_SIZE)   /* 16 */
#define N_SLOTS     4096u
#define CAP         (N_SLOTS - S_PER_SECT)       /* 4080 */

/* ── Fixtures ────────────────────────────────────────────────────────────── */

static int setup(void **state) {
    (void)state;
    spool_host_flash_reset();          /* wipe fake flash to 0xFF (erased) */
    assert_int_equal(spool_mount(), ERR_OK);
    return 0;
}

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void push_env(uint8_t q, uint64_t ts, int64_t wall, uint32_t seq, float t) {
    EnvSample v = { .temp_c = t, .humidity_pct = 50.0f,
                    .pressure_hpa = 1013.25f, .pressure_valid = true };
    SpoolRecord r;
    spool_make_env(&r, &v, q, ts, wall, seq);
    assert_int_equal(spool_push(&r), ERR_OK);
}

/* peek the head, assert its seq, then ack it. */
static void drain_one_expect(uint32_t expect_seq) {
    SpoolRecord r;
    uint64_t ws;
    assert_int_equal(spool_peek(&r, &ws), ERR_OK);
    assert_int_equal(r.seq, expect_seq);
    assert_int_equal(spool_ack(ws), ERR_OK);
}

/* ── Tests ───────────────────────────────────────────────────────────────── */

static void test_empty_mount(void **state) {
    (void)state;
    SpoolRecord r;
    uint64_t ws;
    assert_int_equal(spool_count(), 0);
    assert_int_equal(spool_dropped_total(), 0);
    assert_int_equal(spool_capacity(), CAP);
    assert_int_equal(spool_peek(&r, &ws), ERR_NOT_FOUND);
}

static void test_fifo_order_and_values(void **state) {
    (void)state;
    /* Push one of each kind with distinct payloads. */
    EnvSample    e = { .temp_c = 21.5f, .humidity_pct = 55.0f,
                       .pressure_hpa = 1010.0f, .pressure_valid = true };
    Ens160Sample a = { .co2_ppm = 600, .tvoc_ppb = 300, .aqi = 2, .status = 0 };
    RadarSample  d = { .breath_rpm = 16.5f, .heart_bpm = 72.0f,
                       .distance_mm = 2400, .presence = true, .q = 0 };
    LightSample  l = { .lux = 123.5f };

    SpoolRecord r;
    spool_make_env(&r, &e, 0, 1000, 1700000000000LL, 10); assert_int_equal(spool_push(&r), ERR_OK);
    spool_make_air(&r, &a, 2, 2000, -1, 11);               assert_int_equal(spool_push(&r), ERR_OK);
    spool_make_radar(&r, &d, 0, 3000, 1700000000003LL, 12);assert_int_equal(spool_push(&r), ERR_OK);
    spool_make_light(&r, &l, 0, 4000, 1700000000004LL, 13);assert_int_equal(spool_push(&r), ERR_OK);

    assert_int_equal(spool_count(), 4);

    uint64_t ws;
    /* env first (FIFO), full round-trip of envelope + body. */
    assert_int_equal(spool_peek(&r, &ws), ERR_OK);
    assert_int_equal(r.kind, SPOOL_KIND_ENV);
    assert_int_equal(r.seq, 10);
    assert_int_equal(r.q, 0);
    assert_true(r.ts_us == 1000);
    assert_true(r.wall_ms == 1700000000000LL);
    assert_true(r.body.env.temp_c == 21.5f);
    assert_true(r.body.env.pressure_valid);
    assert_int_equal(spool_ack(ws), ERR_OK);

    assert_int_equal(spool_peek(&r, &ws), ERR_OK);
    assert_int_equal(r.kind, SPOOL_KIND_AIR);
    assert_int_equal(r.seq, 11);
    assert_true(r.wall_ms == -1);
    assert_int_equal(r.body.air.co2_ppm, 600);
    assert_int_equal(r.body.air.aqi, 2);
    assert_int_equal(spool_ack(ws), ERR_OK);

    assert_int_equal(spool_peek(&r, &ws), ERR_OK);
    assert_int_equal(r.kind, SPOOL_KIND_RADAR);
    assert_int_equal(r.seq, 12);
    assert_true(r.body.radar.heart_bpm == 72.0f);
    assert_true(r.body.radar.presence);
    assert_int_equal(r.body.radar.distance_mm, 2400);
    assert_int_equal(spool_ack(ws), ERR_OK);

    assert_int_equal(spool_peek(&r, &ws), ERR_OK);
    assert_int_equal(r.kind, SPOOL_KIND_LIGHT);
    assert_int_equal(r.seq, 13);
    assert_true(r.body.light.lux == 123.5f);
    assert_int_equal(spool_ack(ws), ERR_OK);

    assert_int_equal(spool_count(), 0);
    assert_int_equal(spool_peek(&r, &ws), ERR_NOT_FOUND);
}

/* Stale-ack / duplicate PUBACK: ack of a non-tail write_seq is rejected. */
static void test_ack_out_of_order_rejected(void **state) {
    (void)state;
    push_env(0, 1, -1, 100, 20.0f);
    push_env(0, 2, -1, 101, 21.0f);

    SpoolRecord r;
    uint64_t ws_head;
    assert_int_equal(spool_peek(&r, &ws_head), ERR_OK);
    /* Acking head+1 (not the tail) must be rejected and change nothing. */
    assert_int_equal(spool_ack(ws_head + 1), ERR_INVALID_ARG);
    assert_int_equal(spool_count(), 2);
    /* The correct (tail) ack succeeds. */
    assert_int_equal(spool_ack(ws_head), ERR_OK);
    assert_int_equal(spool_count(), 1);
}

/* Continuously push/ack far past the physical ring size: exercises slot
 * wrap-around and eager sector reclaim while preserving strict FIFO. */
static void test_wraparound_steady_state(void **state) {
    (void)state;
    const uint32_t total = 5000;   /* > N_SLOTS, so slots wrap */
    for (uint32_t i = 0; i < total; i++) {
        push_env(0, i + 1u, -1, i, (float)i);
        assert_int_equal(spool_count(), 1);
        drain_one_expect(i);       /* FIFO: out in the same order */
        assert_int_equal(spool_count(), 0);
    }
    assert_int_equal(spool_dropped_total(), 0);   /* never overflowed */
}

/* Filling past capacity without acking drops the OLDEST records (logged),
 * keeps the newest, and never silently loses count integrity. */
static void test_overflow_drops_oldest(void **state) {
    (void)state;
    const uint32_t over = 200;
    const uint32_t total = CAP + over;
    for (uint32_t i = 0; i < total; i++) {
        push_env(0, i + 1u, -1, i, (float)i);
    }
    /* Undelivered count is capped at capacity (one sector reserved). */
    assert_true(spool_count() <= CAP);
    assert_true(spool_dropped_total() >= over);

    /* The surviving records are the most recent ones, still in FIFO order and
     * contiguous up to the last pushed seq. */
    SpoolRecord r;
    uint64_t ws;
    assert_int_equal(spool_peek(&r, &ws), ERR_OK);
    uint32_t first_surviving = r.seq;
    assert_true(first_surviving >= over);          /* oldest `over` were dropped */

    uint32_t expect = first_surviving;
    uint32_t drained = 0;
    while (spool_peek(&r, &ws) == ERR_OK) {
        assert_int_equal(r.seq, expect);           /* strict ascending order */
        assert_int_equal(spool_ack(ws), ERR_OK);
        expect++;
        drained++;
    }
    assert_int_equal(r.seq, total - 1u);           /* newest survived */
    assert_true(drained <= CAP);
}

/* A record whose magic/CRC is damaged in place is skipped by peek so a single
 * bad page can never wedge the FIFO; surrounding records still drain in order. */
static void test_corrupt_record_skipped(void **state) {
    (void)state;
    push_env(0, 1, -1, 0, 10.0f);   /* ws 0, slot 0 */
    push_env(0, 2, -1, 1, 11.0f);   /* ws 1, slot 1 */
    push_env(0, 3, -1, 2, 12.0f);   /* ws 2, slot 2 */

    /* Corrupt the middle record's magic byte in place (clearing a bit is a
     * legal flash op; it invalidates the record). */
    uint8_t *f = spool_host_flash_ptr();
    f[1u * SLOT_SIZE + 0u] &= 0x00u;

    drain_one_expect(0);            /* record 0 ok */
    drain_one_expect(2);            /* record 1 skipped, record 2 next */
    SpoolRecord r; uint64_t ws;
    assert_int_equal(spool_peek(&r, &ws), ERR_NOT_FOUND);
}

/* A torn write at the newest slot is excluded at mount: the highest *valid*
 * record defines the head, and the device keeps running. */
static void test_remount_excludes_torn_newest(void **state) {
    (void)state;
    for (uint32_t i = 0; i < 5; i++) { push_env(0, i + 1u, -1, i, (float)i); }

    /* Damage the newest record (ws 4, slot 4). */
    uint8_t *f = spool_host_flash_ptr();
    f[4u * SLOT_SIZE + 0u] &= 0x00u;

    assert_int_equal(spool_mount(), ERR_OK);   /* "reboot" */

    /* ws 0..3 recovered in order; the torn newest (ws 4) is excluded. */
    SpoolRecord r; uint64_t ws;
    bool seen[4] = { false, false, false, false };
    uint32_t got = 0;
    while (spool_peek(&r, &ws) == ERR_OK) {
        assert_true(r.seq < 4);                /* torn ws 4 never surfaces */
        seen[r.seq] = true;
        assert_int_equal(spool_ack(ws), ERR_OK);
        got++;
    }
    assert_int_equal(got, 4);
    for (uint32_t i = 0; i < 4; i++) { assert_true(seen[i]); }

    /* A torn head must not block new appends, and they drain cleanly. */
    push_env(0, 99, -1, 500, 1.0f);
    drain_one_expect(500);
    assert_int_equal(spool_peek(&r, &ws), ERR_NOT_FOUND);
}

/* Power loss: undelivered records survive a reboot; already-delivered records
 * may re-send (bounded, deduped downstream) but none are lost. */
static void test_power_loss_preserves_undelivered(void **state) {
    (void)state;
    for (uint32_t i = 0; i < 10; i++) { push_env(0, i + 1u, -1, i, (float)i); }
    /* Deliver the first 4 (no sector boundary crossed → all 10 still on flash). */
    for (uint32_t i = 0; i < 4; i++) { drain_one_expect(i); }
    assert_int_equal(spool_count(), 6);

    assert_int_equal(spool_mount(), ERR_OK);   /* reboot — flash persists */

    /* No undelivered record (4..9) lost; re-sends of 0..3 are acceptable. */
    SpoolRecord r; uint64_t ws;
    assert_int_equal(spool_peek(&r, &ws), ERR_OK);
    uint32_t first = r.seq;
    assert_true(first <= 4);                    /* bounded re-send of delivered */
    uint32_t expect = first;
    while (spool_peek(&r, &ws) == ERR_OK) {
        assert_int_equal(r.seq, expect);
        assert_int_equal(spool_ack(ws), ERR_OK);
        expect++;
    }
    assert_int_equal(expect, 10);               /* drained through seq 9 */
    assert_int_equal(spool_dropped_total(), 0);
}

/* After enough acks to cross sector boundaries, the reclaimed sectors are NOT
 * re-sent on reboot — bounding duplicate delivery to roughly one sector. */
static void test_eager_reclaim_bounds_resend(void **state) {
    (void)state;
    const uint32_t n = 40;          /* spans >2 sectors (16 slots each) */
    for (uint32_t i = 0; i < n; i++) { push_env(0, i + 1u, -1, i, (float)i); }
    for (uint32_t i = 0; i < n; i++) { drain_one_expect(i); }   /* deliver all */
    assert_int_equal(spool_count(), 0);

    assert_int_equal(spool_mount(), ERR_OK);    /* reboot */

    /* Sectors 0 and 1 (ws 0..31) were erased on ack; only the last partial
     * sector lingers, so the re-sent set is well under a full ring. */
    uint32_t lingering = spool_count();
    assert_true(lingering <= 2u * S_PER_SECT);
    SpoolRecord r; uint64_t ws;
    if (lingering > 0) {
        assert_int_equal(spool_peek(&r, &ws), ERR_OK);
        assert_true(r.seq >= 32u);              /* earlier ones were reclaimed */
    }
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup(test_empty_mount, setup),
        cmocka_unit_test_setup(test_fifo_order_and_values, setup),
        cmocka_unit_test_setup(test_ack_out_of_order_rejected, setup),
        cmocka_unit_test_setup(test_wraparound_steady_state, setup),
        cmocka_unit_test_setup(test_overflow_drops_oldest, setup),
        cmocka_unit_test_setup(test_corrupt_record_skipped, setup),
        cmocka_unit_test_setup(test_remount_excludes_torn_newest, setup),
        cmocka_unit_test_setup(test_power_loss_preserves_undelivered, setup),
        cmocka_unit_test_setup(test_eager_reclaim_bounds_resend, setup),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
