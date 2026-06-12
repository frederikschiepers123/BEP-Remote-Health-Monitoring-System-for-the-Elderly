/* Host unit tests for the MCU-side radar plausibility filter (ADR-0005).
 *
 * Pure-logic module: we drive radar_filter_apply() with synthetic RadarSample
 * sequences at the real 1 Hz cadence and assert the gate/confirm/reset
 * behaviour ported from the supervisor's reference filter:
 *   - presence debounce: 10 s to confirm, 8 s to drop
 *   - distance gate 350–1500 mm, 6 s confirm, >200 mm jump restarts
 *   - vitals only after presence AND distance stable; 10 s confirm;
 *     implausible values (heart outside 45–125) never lock
 *   - outputs carry the -RADAR_HEART_CAL_OFFSET_BPM /
 *     -RADAR_BREATH_CAL_OFFSET_RPM calibrations
 *   - q: 0 stable-or-empty, 2 validating, 3 invalid-with-nothing-held
 *   - final robust estimate: per-vital decoupled windows, one-shot per
 *     presence episode (re-armed only by absence), calibrated, spread floored
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
#include "radar_filter.h"

/* ── helpers ─────────────────────────────────────────────────────────────── */

static RadarSample mk(bool pres, uint32_t dist_mm, float heart, float breath,
                      uint8_t q)
{
    RadarSample s;
    s.presence    = pres;
    s.distance_mm = dist_mm;
    s.heart_bpm   = heart;
    s.breath_rpm  = breath;
    s.q           = q;
    return s;
}

/* Feed `raw` n times at 1 Hz starting at *t; advances *t; returns last out. */
static RadarSample feed(RadarFilter *f, const RadarSample *raw, int n,
                        uint32_t *t)
{
    RadarSample out;
    memset(&out, 0, sizeof(out));
    for (int i = 0; i < n; i++) {
        radar_filter_apply(f, raw, *t, &out);
        *t += 1000u;
    }
    return out;
}

static bool feq(float a, float b) { return fabsf(a - b) < 0.05f; }

/* ── tests ───────────────────────────────────────────────────────────────── */

/* Presence needs 10 s of evidence; before that q=2 (validating). */
static void test_presence_debounce(void **state)
{
    (void)state;
    RadarFilter f;
    radar_filter_init(&f);
    uint32_t t = 1000;

    RadarSample person = mk(true, 600, 0.0f, 0.0f, 2);

    /* 10 samples = evidence from t..t+9000 < 10 s confirm — not yet present */
    RadarSample out = feed(&f, &person, 10, &t);
    assert_false(out.presence);
    assert_int_equal(out.q, 2);            /* warming up, not "empty" */

    /* the 11th sample (t0+10000) crosses the confirm window */
    out = feed(&f, &person, 1, &t);
    assert_true(out.presence);

    /* absence: 8 s of no evidence drops presence and yields q=0 (empty) */
    RadarSample empty = mk(false, 0, 0.0f, 0.0f, 0);
    out = feed(&f, &empty, 7, &t);         /* 7 s of silence — still present */
    assert_true(out.presence);
    out = feed(&f, &empty, 1, &t);         /* 8 s — dropped */
    assert_false(out.presence);
    assert_int_equal(out.q, 0);
}

/* Distance stabilises after 6 s in-gate; out-of-gate never; jump restarts. */
static void test_distance_gate_confirm_jump(void **state)
{
    (void)state;
    RadarFilter f;
    radar_filter_init(&f);
    uint32_t t = 1000;

    RadarSample s = mk(true, 800, 0.0f, 0.0f, 2);
    RadarSample out = feed(&f, &s, 6, &t);     /* 0..5000 ms < 6 s confirm */
    assert_int_equal(out.distance_mm, 0);      /* still validating */

    out = feed(&f, &s, 1, &t);                 /* t0+6000 — confirmed */
    assert_int_equal(out.distance_mm, 800);

    /* >200 mm jump restarts validation */
    RadarSample jumped = mk(true, 1200, 0.0f, 0.0f, 2);
    out = feed(&f, &jumped, 1, &t);
    assert_int_equal(out.distance_mm, 0);

    /* out-of-gate (>1500 mm) never stabilises */
    radar_filter_init(&f);
    t = 1000;
    RadarSample far = mk(true, 2500, 0.0f, 0.0f, 2);
    out = feed(&f, &far, 20, &t);
    assert_int_equal(out.distance_mm, 0);
}

/* Vitals lock only after presence (10 s) AND distance (6 s) are stable,
 * then their own 10 s confirm — and the heart output is offset-calibrated. */
static void test_vitals_lock_and_heart_offset(void **state)
{
    (void)state;
    RadarFilter f;
    radar_filter_init(&f);
    uint32_t t = 1000;

    RadarSample s = mk(true, 600, 100.0f, 15.0f, 0);

    /* t0..t0+9000: gates still confirming — no vitals */
    RadarSample out = feed(&f, &s, 10, &t);
    assert_true(feq(out.heart_bpm, 0.0f));
    assert_true(feq(out.breath_rpm, 0.0f));
    assert_int_equal(out.q, 2);

    /* t0+10000: gates open; vitals start their own confirm window */
    out = feed(&f, &s, 10, &t);                /* ..t0+19000: confirming */
    assert_true(feq(out.heart_bpm, 0.0f));
    assert_int_equal(out.q, 2);                /* present, vitals not locked */

    out = feed(&f, &s, 1, &t);                 /* t0+20000: vitals stable */
    assert_true(feq(out.heart_bpm, 100.0f - RADAR_HEART_CAL_OFFSET_BPM));
    assert_true(feq(out.breath_rpm, 15.0f - RADAR_BREATH_CAL_OFFSET_RPM));
    assert_int_equal(out.q, 0);
    assert_true(out.presence);
    assert_int_equal(out.distance_mm, 600);
}

/* Implausible heart rate (outside 45–125 BPM) must never lock. */
static void test_implausible_heart_rejected(void **state)
{
    (void)state;
    RadarFilter f;
    radar_filter_init(&f);
    uint32_t t = 1000;

    RadarSample s = mk(true, 600, 160.0f, 15.0f, 0);
    RadarSample out = feed(&f, &s, 30, &t);    /* way past all windows */

    assert_true(feq(out.heart_bpm, 0.0f));     /* rejected forever */
    assert_true(feq(out.breath_rpm,            /* breath (valid) locked fine */
                    15.0f - RADAR_BREATH_CAL_OFFSET_RPM));
    assert_int_equal(out.q, 0);                /* one stable vital ⇒ ok */
}

/* A vital jump drops the filter back to re-confirming, but the display HOLDS
 * the last confident value (≤ RADAR_VITAL_HOLD_MS) so the mirror tile doesn't
 * null out.  If the vital keeps jumping (never re-confirms) the held value
 * finally expires. */
static void test_vital_hold_through_jump(void **state)
{
    (void)state;
    RadarFilter f;
    radar_filter_init(&f);
    uint32_t t = 1000;

    RadarSample s = mk(true, 600, 100.0f, 15.0f, 0);
    RadarSample out = feed(&f, &s, 21, &t);    /* heart locked */
    float held = 100.0f - RADAR_HEART_CAL_OFFSET_BPM;
    assert_true(feq(out.heart_bpm, held));

    /* keep jumping heart >8 BPM every sample so it never re-confirms */
    for (int i = 0; i < 12; i++) {             /* ~12 s: HELD, not null */
        RadarSample j = mk(true, 600, (i % 2) ? 115.0f : 60.0f, 15.0f, 0);
        radar_filter_apply(&f, &j, t, &out);
        t += 1000u;
    }
    assert_true(feq(out.heart_bpm, held));     /* held through re-confirm */
    assert_int_equal(out.q, 0);                /* breath still fresh-stable */

    /* past RADAR_VITAL_HOLD_MS since the last real reading → held expires */
    for (int i = 0; i < 12; i++) {
        RadarSample j = mk(true, 600, (i % 2) ? 115.0f : 60.0f, 15.0f, 0);
        radar_filter_apply(&f, &j, t, &out);
        t += 1000u;
    }
    assert_true(feq(out.heart_bpm, 0.0f));     /* expired */
}

/* Losing presence resets vitals even if stale vitals keep being latched. */
static void test_absence_resets_vitals(void **state)
{
    (void)state;
    RadarFilter f;
    radar_filter_init(&f);
    uint32_t t = 1000;

    RadarSample s = mk(true, 600, 100.0f, 15.0f, 0);
    RadarSample out = feed(&f, &s, 21, &t);
    assert_true(out.heart_bpm > 0.0f);

    /* person leaves; driver still reports last latched vitals briefly */
    RadarSample gone = mk(false, 0, 100.0f, 15.0f, 0);
    out = feed(&f, &gone, 9, &t);              /* > 8 s absence */
    assert_false(out.presence);
    assert_true(feq(out.heart_bpm, 0.0f));
    assert_true(feq(out.breath_rpm, 0.0f));
}

/* When heart frames stop, the value is HELD for RADAR_VITAL_HOLD_MS (20 s)
 * measured from the last real reading — spanning both the svf input-gap window
 * and the display-hold, never the two stacked — then expires and re-confirms.
 * Breath is unaffected throughout. */
static void test_vital_input_timeout_expires(void **state)
{
    (void)state;
    RadarFilter f;
    radar_filter_init(&f);
    uint32_t t = 1000;

    RadarSample s = mk(true, 600, 100.0f, 15.0f, 0);
    RadarSample out = feed(&f, &s, 21, &t);          /* heart locked, t->22000 */
    assert_true(out.heart_bpm > 0.0f);

    /* heart frames stop; presence + distance + breath keep flowing */
    RadarSample no_heart = mk(true, 600, 0.0f, 15.0f, 0);
    out = feed(&f, &no_heart, 19, &t);               /* 19 s since last reading */
    assert_true(feq(out.heart_bpm,                   /* still HELD (≤20 s) */
                    100.0f - RADAR_HEART_CAL_OFFSET_BPM));
    assert_true(feq(out.breath_rpm,                  /* unaffected */
                    15.0f - RADAR_BREATH_CAL_OFFSET_RPM));

    out = feed(&f, &no_heart, 3, &t);                /* now past 20 s */
    assert_true(feq(out.heart_bpm, 0.0f));           /* expired */
    assert_true(feq(out.breath_rpm, 15.0f - RADAR_BREATH_CAL_OFFSET_RPM));

    /* heart returns: must re-confirm for 10 s, not relock instantly */
    out = feed(&f, &s, 5, &t);
    assert_true(feq(out.heart_bpm, 0.0f));           /* still validating */
    out = feed(&f, &s, 7, &t);                       /* past 10 s confirm */
    assert_true(out.heart_bpm > 0.0f);
}

/* The MR60BHA2 reports heart and breath in long ALTERNATING bursts (HIL
 * 2026-06-11): while one streams the other is null for ~10 s.  With the 15 s
 * (heart) / 20 s (breath) timeouts each vital HOLDS through the other's burst,
 * so both stay co-stable (q=0) and the robust estimate completes — the whole
 * point of option 2.  Before the fix (6 s timeout) the idle vital expired
 * mid-burst and the estimate never fired. */
static void test_alternating_bursts_keep_costable(void **state)
{
    (void)state;
    RadarFilter f;
    radar_filter_init(&f);
    uint32_t t = 1000;

    RadarSample both = mk(true, 600, 100.0f, 15.0f, 0);
    RadarVitalsEstimate est;

    RadarSample out = feed(&f, &both, 21, &t);       /* lock both vitals */
    assert_int_equal(out.q, 0);
    assert_true(out.heart_bpm > 0.0f && out.breath_rpm > 0.0f);

    /* 10 s heart-only, 10 s breath-only, repeating */
    RadarSample heart_only  = mk(true, 600, 100.0f, 0.0f, 0);
    RadarSample breath_only = mk(true, 600, 0.0f, 15.0f, 0);
    bool fired = false;
    for (int i = 0; i < 60 && !fired; i++) {
        const RadarSample *raw = ((i / 10) % 2 == 0) ? &heart_only : &breath_only;
        radar_filter_apply(&f, raw, t, &out);
        assert_int_equal(out.q, 0);                  /* never drops to validating */
        assert_true(out.heart_bpm  > 0.0f);          /* held through breath burst */
        assert_true(out.breath_rpm > 0.0f);          /* held through heart burst  */
        fired = radar_filter_take_estimate(&f, &est);
        t += 1000u;
    }
    assert_true(fired);                              /* estimate now reachable */
    assert_true(feq(est.heart_bpm, 100.0f - RADAR_HEART_CAL_OFFSET_BPM));
    assert_true(feq(est.breath_rpm, 15.0f - RADAR_BREATH_CAL_OFFSET_RPM));
}

/* After a long gap in apply() calls, the first sample must NOT be instantly
 * stable — the stale candidate has to expire first (expire-before-update). */
static void test_feed_gap_not_instantly_stable(void **state)
{
    (void)state;
    RadarFilter f;
    radar_filter_init(&f);
    uint32_t t = 1000;

    RadarSample s = mk(true, 800, 0.0f, 0.0f, 2);
    RadarSample out = feed(&f, &s, 7, &t);           /* distance confirmed */
    assert_int_equal(out.distance_mm, 800);

    t += 8000;                                       /* 8 s with NO apply() */
    out = feed(&f, &s, 1, &t);
    assert_int_equal(out.distance_mm, 0);            /* must re-validate */
}

/* Invalid input with nothing held passes q=3 through. */
static void test_invalid_passthrough(void **state)
{
    (void)state;
    RadarFilter f;
    radar_filter_init(&f);
    uint32_t t = 1000;

    RadarSample bad = mk(false, 0, 0.0f, 0.0f, 3);
    RadarSample out = feed(&f, &bad, 3, &t);
    assert_int_equal(out.q, 3);
    assert_false(out.presence);
}

/* First estimate: presence locks ~10 s, vitals ~20 s, then the
 * RADAR_EST_WINDOW_MS (20 s) window closes ~40 s in, with the calibrated
 * robust mean and the floored spread.  Loop-until-fire so the test isn't
 * pinned to the exact fire-sample arithmetic. */
static void test_final_estimate_first_fire(void **state)
{
    (void)state;
    RadarFilter f;
    radar_filter_init(&f);
    uint32_t t = 1000;

    RadarSample s = mk(true, 600, 100.0f, 15.0f, 0);
    RadarVitalsEstimate est;

    int fired_at = -1;
    for (int i = 0; i < 70 && fired_at < 0; i++) {
        RadarSample out;
        radar_filter_apply(&f, &s, t, &out);
        if (radar_filter_take_estimate(&f, &est)) fired_at = i;
        t += 1000u;
    }
    assert_true(fired_at >= 0);
    assert_in_range(fired_at, 36, 46);         /* ~20 s lock + 20 s window */
    assert_true(feq(est.heart_bpm, 100.0f - RADAR_HEART_CAL_OFFSET_BPM));
    assert_true(feq(est.breath_rpm, 15.0f - RADAR_BREATH_CAL_OFFSET_RPM));
    assert_true(feq(est.heart_spread, RADAR_EST_MIN_ROBUST_SIGMA));
    assert_true(feq(est.breath_spread, RADAR_EST_MIN_ROBUST_SIGMA));
    assert_true(est.heart_n  >= RADAR_EST_MIN_SAMPLES);
    assert_true(est.breath_n >= RADAR_EST_MIN_SAMPLES);
}

/* The estimate REPEATS while the subject stays present: after the first fire,
 * continued stability re-arms and fires a fresh estimate each window-plus-gap
 * (the demo wants it to refresh, not print once per visit). */
static void test_final_estimate_repeats_while_present(void **state)
{
    (void)state;
    RadarFilter f;
    radar_filter_init(&f);
    uint32_t t = 1000;

    RadarSample s = mk(true, 600, 100.0f, 15.0f, 0);
    RadarVitalsEstimate est;

    int fires = 0;
    int gaps[4] = {0};
    int last_fire = -1;
    for (int i = 0; i < 140 && fires < 3; i++) {
        RadarSample out;
        radar_filter_apply(&f, &s, t, &out);
        if (radar_filter_take_estimate(&f, &est)) {
            if (last_fire >= 0) gaps[fires - 1] = i - last_fire;
            last_fire = i;
            fires++;
        }
        t += 1000u;
    }
    assert_int_equal(fires, 3);                 /* fired repeatedly */
    /* each re-fire is one fresh window-plus-confirm gap apart, not instant */
    assert_in_range(gaps[0], 18, 30);
    assert_in_range(gaps[1], 18, 30);
    assert_true(feq(est.heart_bpm, 100.0f - RADAR_HEART_CAL_OFFSET_BPM));
}

/* Losing presence discards any in-progress windows: no estimate fires while
 * absent, and after the subject returns a fresh episode fires anew. */
static void test_final_estimate_discarded_on_absence(void **state)
{
    (void)state;
    RadarFilter f;
    radar_filter_init(&f);
    uint32_t t = 1000;

    RadarSample s = mk(true, 600, 100.0f, 15.0f, 0);
    RadarVitalsEstimate est;

    /* fire once, then the person leaves part-way through the next window */
    bool fired = false;
    for (int i = 0; i < 70 && !fired; i++) {
        RadarSample out;
        radar_filter_apply(&f, &s, t, &out);
        fired = radar_filter_take_estimate(&f, &est);
        t += 1000u;
    }
    assert_true(fired);
    feed(&f, &s, 10, &t);                       /* partial next window */

    /* person leaves (> 8 s silence) → presence drops, windows discarded */
    RadarSample empty = mk(false, 0, 0.0f, 0.0f, 0);
    RadarSample out = feed(&f, &empty, 12, &t);
    assert_false(out.presence);
    assert_false(radar_filter_take_estimate(&f, &est));   /* nothing while gone */

    /* fresh episode after return: presence 10 s + vitals 10 s + 20 s window */
    fired = false;
    for (int i = 0; i < 70 && !fired; i++) {
        RadarSample o;
        radar_filter_apply(&f, &s, t, &o);
        fired = radar_filter_take_estimate(&f, &est);
        t += 1000u;
    }
    assert_true(fired);
    assert_true(feq(est.heart_bpm, 100.0f - RADAR_HEART_CAL_OFFSET_BPM));
}

/* The decoupling money-test: heart and breath are NEVER stable at the same
 * time, yet the estimate fires — each vital latches its window during its
 * own stable phase, and a latch survives its vital destabilising afterwards.
 * Phase A: heart steady, breath jumping (3 RPM guard trips every sample).
 * Phase B: breath steady, heart jumping (8 BPM guard trips every sample). */
static void test_decoupled_estimate_no_costability(void **state)
{
    (void)state;
    RadarFilter f;
    radar_filter_init(&f);
    uint32_t t = 1000;
    RadarVitalsEstimate est;

    /* Phase A: presence+distance+heart lock; breath alternates 10/20 RPM */
    for (int i = 0; i < 45; i++) {
        RadarSample s = mk(true, 600, 100.0f,
                           (i % 2) ? 10.0f : 20.0f, 0);
        RadarSample out;
        radar_filter_apply(&f, &s, t, &out);
        assert_true(feq(out.breath_rpm, 0.0f));    /* breath never stable */
        assert_false(radar_filter_take_estimate(&f, &est));
        t += 1000u;
    }

    /* Phase B: breath 15 steady; heart alternates 60/100 BPM, destabilising
     * it — but its already-latched window half must survive */
    bool fired = false;
    for (int i = 0; i < 45 && !fired; i++) {
        RadarSample s = mk(true, 600,
                           (i % 2) ? 60.0f : 100.0f, 15.0f, 0);
        RadarSample out;
        radar_filter_apply(&f, &s, t, &out);
        fired = radar_filter_take_estimate(&f, &est);
        t += 1000u;
    }
    assert_true(fired);
    assert_true(feq(est.heart_bpm, 100.0f - RADAR_HEART_CAL_OFFSET_BPM));
    assert_true(feq(est.breath_rpm, 15.0f - RADAR_BREATH_CAL_OFFSET_RPM));
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_presence_debounce),
        cmocka_unit_test(test_distance_gate_confirm_jump),
        cmocka_unit_test(test_vitals_lock_and_heart_offset),
        cmocka_unit_test(test_implausible_heart_rejected),
        cmocka_unit_test(test_vital_hold_through_jump),
        cmocka_unit_test(test_absence_resets_vitals),
        cmocka_unit_test(test_vital_input_timeout_expires),
        cmocka_unit_test(test_alternating_bursts_keep_costable),
        cmocka_unit_test(test_feed_gap_not_instantly_stable),
        cmocka_unit_test(test_invalid_passthrough),
        cmocka_unit_test(test_final_estimate_first_fire),
        cmocka_unit_test(test_final_estimate_repeats_while_present),
        cmocka_unit_test(test_final_estimate_discarded_on_absence),
        cmocka_unit_test(test_decoupled_estimate_no_costability),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
