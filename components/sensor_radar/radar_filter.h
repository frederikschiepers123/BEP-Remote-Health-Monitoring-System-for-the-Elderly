#ifndef RADAR_FILTER_H
#define RADAR_FILTER_H

/* MCU-side radar plausibility filter (ADR-0005).
 *
 * The raw MR60BHA2 stream is too jumpy for direct display: vitals flicker,
 * ghost targets blip presence, and distance jitters.  This module ports the
 * supervisor-provided filter design (docs/MR60-filtering reference, originally
 * Arduino) into the firmware as a pure-logic stage applied ABOVE the
 * radar_driver_t v-table — it consumes the raw RadarSample a driver produced
 * and emits a debounced/validated/smoothed RadarSample with the same shape and
 * conventions (0 / 0.0f = field absent; q = 0 ok, 2 degraded/validating,
 * 3 invalid).  Because it sits on RadarSample, it applies unchanged to any
 * future radar behind the v-table (e.g. HMMD).
 *
 * Filter stages (constants below, from the supervisor's reference):
 *   1. Presence debounce  — presence evidence must persist PRESENCE_CONFIRM_MS
 *      before it is trusted; after evidence stops, ABSENCE_CONFIRM_MS before
 *      presence drops.  Evidence = driver presence flag OR an in-gate distance.
 *   2. Distance gate + stability — readings outside
 *      [RADAR_FILT_DIST_MIN_MM, RADAR_FILT_DIST_MAX_MM] are rejected; a jump
 *      larger than RADAR_FILT_DIST_JUMP_MM restarts validation; the value must
 *      be stable for RADAR_FILT_DIST_CONFIRM_MS, then a time-aware low-pass
 *      (tau RADAR_FILT_DIST_TAU_MS) smooths it.
 *   3. Vitals gating — heart/breath are only processed while presence AND
 *      distance are stable; each goes through the same plausibility-range /
 *      max-jump / confirm / low-pass pipeline, and resets if presence or
 *      distance destabilise or the input stream stops (timeout).
 *
 * Heart-rate calibration: the supervisor's reference subtracts a fixed
 * RADAR_HEART_CAL_OFFSET_BPM from the displayed heart rate (bench observation:
 * the MR60BHA2 reads high).  Applied here at output time, never to the
 * filter's internal state.
 *
 * Repeated-sample note: drivers latch the last frame for up to their stale
 * window (MR60BHA2: 5 s), so apply() may see the same vital value several
 * times.  That is harmless — confirm windows are time-based, the LPF converges
 * on repeats, and the presence/distance gates (which clear vitals) react
 * within ABSENCE_CONFIRM_MS when the person actually leaves.  It does stretch
 * the EFFECTIVE input-timeout figures by up to that latch window in
 * degraded-input cases (vitals expiry ~11 s instead of 6 s; silence-driven
 * absence ~7 s instead of 4.5 s in the reference) — bounded display latency,
 * accepted in ADR-0005.
 *
 * Threading: no locks, no allocation, no HAL calls.  One RadarFilter instance
 * must only be fed from a single task (production: transport-side consumer is
 * unaffected — the filter runs in radar_task before enqueue).
 */

#include "radar_driver.h"   /* RadarSample */

#include <stdint.h>
#include <stdbool.h>

/* ── Tunables (ported 1:1 from the supervisor's reference, cm → mm) ───────── */

#define RADAR_FILT_PRESENCE_CONFIRM_MS 10000U /* evidence needed before "present" */
#define RADAR_FILT_ABSENCE_CONFIRM_MS  2000U  /* silence needed before "absent"   */

#define RADAR_FILT_DIST_MIN_MM         350.0f
#define RADAR_FILT_DIST_MAX_MM         1500.0f
#define RADAR_FILT_DIST_JUMP_MM        200.0f
#define RADAR_FILT_DIST_CONFIRM_MS     6000U
#define RADAR_FILT_DIST_TAU_MS         8000.0f
#define RADAR_FILT_DIST_TIMEOUT_MS     5000U

#define RADAR_FILT_HEART_MIN_BPM       45.0f
#define RADAR_FILT_HEART_MAX_BPM       125.0f
#define RADAR_FILT_HEART_JUMP_BPM      8.0f
#define RADAR_FILT_HEART_CONFIRM_MS    10000U
#define RADAR_FILT_HEART_TAU_MS        12000.0f
#define RADAR_FILT_HEART_TIMEOUT_MS    6000U

#define RADAR_FILT_BREATH_MIN_RPM      6.0f
#define RADAR_FILT_BREATH_MAX_RPM      30.0f
#define RADAR_FILT_BREATH_JUMP_RPM     3.0f
#define RADAR_FILT_BREATH_CONFIRM_MS   10000U
#define RADAR_FILT_BREATH_TAU_MS       15000.0f
#define RADAR_FILT_BREATH_TIMEOUT_MS   6000U

/* Bench calibration: MR60BHA2 heart rate reads ~20 BPM high (supervisor's
 * reference subtracts 20 at output).  Set to 0.0f to disable. */
#define RADAR_HEART_CAL_OFFSET_BPM     20.0f

/* ── Internal state (treat as opaque; sized for static allocation) ────────── */

typedef struct {
    bool     candidate_active;
    bool     stable;
    bool     lpf_active;
    bool     has_input;      /* any accepted input since reset (wrap-proof) */
    float    candidate;
    float    value;
    uint32_t candidate_since_ms;
    uint32_t last_input_ms;
    uint32_t lpf_last_ms;
    /* per-instance tunables, fixed at init */
    float    min_value, max_value, max_jump, tau_ms;
    uint32_t confirm_ms, timeout_ms;
} StableValueFilter;

typedef struct {
    bool     stable_present;
    bool     candidate_active;
    uint32_t candidate_since_ms;
    uint32_t last_evidence_ms;
} PresenceGate;

typedef struct {
    PresenceGate      presence;
    StableValueFilter distance;   /* mm  */
    StableValueFilter heart;      /* BPM */
    StableValueFilter breath;     /* RPM */
} RadarFilter;

/* ── API ──────────────────────────────────────────────────────────────────── */

/** Reset all stages to "nothing trusted yet". */
void radar_filter_init(RadarFilter *f);

/**
 * Feed one raw driver sample, get the filtered sample to publish.
 *
 * @param f       Filter state (radar_filter_init'd once).
 * @param raw     Sample as produced by a radar_driver_t read_sample().
 * @param now_ms  Monotonic milliseconds (any epoch; must only move forward).
 * @param out     Filtered sample.  presence = debounced; distance_mm /
 *                heart_bpm / breath_rpm = smoothed value or 0 while
 *                unvalidated; q = 0 stable-or-confidently-empty,
 *                2 validating/degraded, 3 invalid input with nothing held.
 */
void radar_filter_apply(RadarFilter *f, const RadarSample *raw,
                        uint32_t now_ms, RadarSample *out);

#endif /* RADAR_FILTER_H */
