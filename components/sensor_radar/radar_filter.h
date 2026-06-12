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
 *   4. Final robust vitals estimate — DECOUPLED per vital (HIL 2026-06-11:
 *      the MR60BHA2 alternates heart/breath bursts and a returning burst
 *      often jumps past the guard, so requiring simultaneous stability never
 *      completed on hardware).  While a vital is stable, one calibrated
 *      sample per second is collected into ITS OWN window; a vital
 *      destabilising discards only its own (unlatched) window.  When a
 *      window spans RADAR_EST_WINDOW_MS with ≥ RADAR_EST_MIN_SAMPLES, it is
 *      reduced with median + MAD outlier rejection (deviation >
 *      RADAR_EST_OUTLIER_K × robust σ discarded) and LATCHED.  Once both
 *      vitals have latched the combined estimate fires and the stage
 *      RE-ARMS (both windows + latches cleared), so a fresh estimate is
 *      produced repeatedly — roughly every window-plus-gap while the subject
 *      stays.  Losing presence discards any in-progress windows/latches.
 *      Consumed via radar_filter_take_estimate() — it is logged, NOT
 *      published (the §9.1 topic set and §9.2.2 radar schema are closed;
 *      putting it on the wire is an ADR).
 *
 * Calibration: the supervisor's reference subtracts fixed offsets from the
 * displayed vitals (bench observation: the MR60BHA2 reads high) —
 * RADAR_HEART_CAL_OFFSET_BPM from heart, RADAR_BREATH_CAL_OFFSET_RPM from
 * breath.  Applied at output time (and to the estimate window samples),
 * never to the filter's internal state.
 *
 * Repeated-sample note: drivers latch the last frame for up to their stale
 * window (MR60BHA2: 5 s), so apply() may see the same vital value several
 * times.  That is harmless — confirm windows are time-based, the LPF converges
 * on repeats, and the presence/distance gates (which clear vitals) react
 * within ABSENCE_CONFIRM_MS when the person actually leaves.  It also stacks
 * on the vital input-timeouts: with the 5 s latch the EFFECTIVE vitals expiry
 * is ~20 s (heart) / ~25 s (breath), and silence-driven absence ~13 s —
 * bounded display latency, accepted in ADR-0005.  The longer vital timeouts
 * (vs the reference's 6 s) are deliberate; see the HEART_TIMEOUT note below.
 *
 * Threading: no locks, no allocation, no HAL calls.  One RadarFilter instance
 * must only be fed from a single task (production: transport-side consumer is
 * unaffected — the filter runs in radar_task before enqueue).
 */

#include "radar_driver.h"   /* RadarSample */

#include <stdint.h>
#include <stdbool.h>

/* ── Tunables (from the supervisor's reference, cm → mm; HIL deviations are
 *    noted inline — vital timeouts and the estimate window/min-samples) ───── */

#define RADAR_FILT_PRESENCE_CONFIRM_MS 10000U /* evidence needed before "present" */
#define RADAR_FILT_ABSENCE_CONFIRM_MS  8000U  /* silence needed before "absent"   */

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
/* Bench finding (HIL 2026-06-11): the MR60BHA2 reports heart and breath in
 * long ALTERNATING bursts — while one streams, the other goes null for a
 * median ~9 s (heart) / ~14 s (breath), p90 ~20 s.  The reference's 6 s
 * timeout expired the idle vital mid-burst, so both were rarely co-stable
 * (longest both-stable window observed on hardware: 29 s) and the §4 robust
 * estimate never completed.  Timeouts raised to bridge the typical burst:
 * with the driver's 5 s latch (RADAR_STALE_MS) the effective hold is ~20 s
 * (heart) / ~25 s (breath).  Safe against stale-after-departure — presence
 * gating force-resets both vitals within ABSENCE_CONFIRM_MS (8 s) of the
 * subject leaving, so the longer timeout only extends holds WHILE present. */
#define RADAR_FILT_HEART_TIMEOUT_MS    15000U

#define RADAR_FILT_BREATH_MIN_RPM      6.0f
#define RADAR_FILT_BREATH_MAX_RPM      30.0f
#define RADAR_FILT_BREATH_JUMP_RPM     3.0f
#define RADAR_FILT_BREATH_CONFIRM_MS   10000U
#define RADAR_FILT_BREATH_TAU_MS       15000.0f
#define RADAR_FILT_BREATH_TIMEOUT_MS   20000U   /* see HEART_TIMEOUT note above */

/* Bench calibration: the MR60BHA2 reads high (supervisor's reference
 * subtracts 20 BPM / 2 RPM at output).  Set to 0.0f to disable. */
#define RADAR_HEART_CAL_OFFSET_BPM     20.0f
#define RADAR_BREATH_CAL_OFFSET_RPM    2.0f

/* Display hold: while present, keep emitting the last CONFIDENT vital value
 * (instead of null) through jump-reconfirm / input gaps, for up to this long
 * since it was last fresh — so the mirror tile always shows a vital ≤ this
 * old rather than freezing when heart/breath alternate.  A held value is
 * marked q=2 (preliminary), distinguishing it from a fresh lock (q=0) for the
 * SBC (§9.6).  The estimate stage still uses ONLY fresh values. */
#define RADAR_VITAL_HOLD_MS            20000U

/* Final robust vitals estimate (median + MAD outlier rejection), PER VITAL.
 * Window was 60 s in the reference, cut to 20 s after HIL: the MR60BHA2
 * alternates heart and breath bursts, so long simultaneous stability never
 * happens; each vital therefore fills its own 20 s window over its own
 * stable runs (see the stage-4 note above).  At the demo's ~1.3 s publish
 * cadence a 20 s window yields ~15 samples, hence MIN_SAMPLES 15. */
#define RADAR_EST_WINDOW_MS            20000U /* per-vital window span         */
#define RADAR_EST_SAMPLE_EVERY_MS      1000U  /* one filtered sample per second */
#define RADAR_EST_MAX_SAMPLES          60
#define RADAR_EST_MIN_SAMPLES          15     /* fewer ⇒ window not usable      */
#define RADAR_EST_OUTLIER_K            2.5f   /* reject > K × robust σ          */
#define RADAR_EST_MIN_ROBUST_SIGMA     0.5f   /* spread floor (identical samples) */

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
    float    samples[RADAR_EST_MAX_SAMPLES];
    int      count;
    uint32_t first_sample_ms;
} RobustWindowEstimator;

/** Result of the robust vitals estimate (already calibrated). */
typedef struct {
    float heart_bpm;
    float heart_spread;    /* robust ±σ (1.4826 × MAD, floored)        */
    int   heart_n;         /* samples surviving outlier rejection      */
    float breath_rpm;
    float breath_spread;
    int   breath_n;
} RadarVitalsEstimate;

typedef struct {
    PresenceGate          presence;
    StableValueFilter     distance;   /* mm  */
    StableValueFilter     heart;      /* BPM */
    StableValueFilter     breath;     /* RPM */
    /* display-hold: last CONFIDENT calibrated vital + when it was fresh */
    float                 heart_disp, breath_disp;
    uint32_t              heart_disp_ms, breath_disp_ms;
    bool                  heart_disp_valid, breath_disp_valid;
    /* final-estimate stage (per-vital windows; see stage-4 note) */
    RobustWindowEstimator heart_est;
    RobustWindowEstimator breath_est;
    bool                  heart_est_latched;   /* heart half complete   */
    bool                  breath_est_latched;  /* breath half complete  */
    RadarVitalsEstimate   estimate;        /* latch target              */
    bool                  estimate_ready;  /* both latched, not taken   */
    uint32_t              last_est_sample_ms;  /* 0 = none yet          */
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

/**
 * Consume the latest robust vitals estimate, if one just became ready.
 *
 * Returns true once each time both per-vital windows (RADAR_EST_WINDOW_MS
 * each, collected over their own stable runs — they need not overlap) have
 * latched; the stage then re-arms, so while a subject stays present a fresh
 * estimate becomes ready roughly every window-plus-gap.  Losing presence
 * discards any in-progress windows.  Call once per apply() and act on true.
 *
 * @param f    Filter state.
 * @param out  Filled with the calibrated estimate when true is returned.
 * @return true when a fresh estimate is ready (clears the ready flag), else false.
 */
bool radar_filter_take_estimate(RadarFilter *f, RadarVitalsEstimate *out);

#endif /* RADAR_FILTER_H */
