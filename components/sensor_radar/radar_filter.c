/* MCU-side radar plausibility filter (ADR-0005).  See radar_filter.h for the
 * stage-by-stage description; this file is a faithful C11 port of the
 * supervisor's reference filter (Arduino), with units in mm/ms and the
 * firmware's RadarSample conventions at the boundary.
 *
 * Pure logic: no HAL, no FreeRTOS, no allocation — host-tested in
 * test/host/test_radar_filter.c. */

#include "radar_filter.h"

#include <math.h>

/* ── StableValueFilter — plausibility gate + jump guard + confirm + LPF ──── */

static void svf_reset(StableValueFilter *s)
{
    s->candidate_active   = false;
    s->stable             = false;
    s->lpf_active         = false;
    s->has_input          = false;
    s->candidate          = 0.0f;
    s->value              = 0.0f;
    s->candidate_since_ms = 0;
    s->last_input_ms      = 0;
    s->lpf_last_ms        = 0;
}

static void svf_init(StableValueFilter *s, float min_v, float max_v,
                     float max_jump, float tau_ms,
                     uint32_t confirm_ms, uint32_t timeout_ms)
{
    s->min_value  = min_v;
    s->max_value  = max_v;
    s->max_jump   = max_jump;
    s->tau_ms     = tau_ms;
    s->confirm_ms = confirm_ms;
    s->timeout_ms = timeout_ms;
    svf_reset(s);
}

static void svf_update(StableValueFilter *s, float x, uint32_t now)
{
    /* Plausibility gate: silently ignore impossible readings. */
    if (!isfinite(x) || x < s->min_value || x > s->max_value) {
        return;
    }

    s->last_input_ms = now;
    s->has_input     = true;

    /* First value, or value jumped too far: restart the candidate window. */
    if (!s->candidate_active || fabsf(x - s->candidate) > s->max_jump) {
        s->candidate_active   = true;
        s->candidate          = x;
        s->candidate_since_ms = now;
        s->stable             = false;
        s->lpf_active         = false;
        return;
    }

    /* Candidate slowly follows the raw values so drift doesn't re-trigger
     * the jump guard. */
    s->candidate += 0.25f * (x - s->candidate);

    if ((uint32_t)(now - s->candidate_since_ms) < s->confirm_ms) {
        return;   /* still validating */
    }

    s->stable = true;

    /* Time-aware low-pass: alpha = dt / (tau + dt). */
    if (!s->lpf_active) {
        s->value       = x;
        s->lpf_active  = true;
        s->lpf_last_ms = now;
    } else {
        uint32_t dt = now - s->lpf_last_ms;
        s->lpf_last_ms = now;
        float alpha = (float)dt / (s->tau_ms + (float)dt);
        s->value += alpha * (x - s->value);
    }
}

static void svf_expire(StableValueFilter *s, uint32_t now)
{
    if (!s->has_input) {
        return;
    }
    if ((uint32_t)(now - s->last_input_ms) > s->timeout_ms) {
        svf_reset(s);
    }
}

static bool svf_stable(const StableValueFilter *s)
{
    return s->stable && s->lpf_active;
}

/* ── PresenceGate — debounced presence ───────────────────────────────────── */

static void gate_reset(PresenceGate *g)
{
    g->stable_present     = false;
    g->candidate_active   = false;
    g->candidate_since_ms = 0;
    g->last_evidence_ms   = 0;
}

static void gate_update(PresenceGate *g, bool evidence, uint32_t now)
{
    if (evidence) {
        g->last_evidence_ms = now;

        if (!g->candidate_active) {
            g->candidate_active   = true;
            g->candidate_since_ms = now;
        }
        if (!g->stable_present &&
            (uint32_t)(now - g->candidate_since_ms) >=
                RADAR_FILT_PRESENCE_CONFIRM_MS) {
            g->stable_present = true;
        }
    } else {
        if (!g->stable_present) {
            /* Evidence lapsed before confirmation — start over. */
            g->candidate_active   = false;
            g->candidate_since_ms = 0;
        }
        if (g->stable_present &&
            (uint32_t)(now - g->last_evidence_ms) >=
                RADAR_FILT_ABSENCE_CONFIRM_MS) {
            gate_reset(g);
        }
    }
}

static bool gate_warming_up(const PresenceGate *g)
{
    return g->candidate_active && !g->stable_present;
}

/* ── RobustWindowEstimator — windowed median + MAD reducer ───────────────── */

static void rwe_reset(RobustWindowEstimator *e)
{
    e->count           = 0;
    e->first_sample_ms = 0;
}

static void rwe_add(RobustWindowEstimator *e, float x, uint32_t now)
{
    if (!isfinite(x)) {
        return;
    }
    if (e->count == 0) {
        e->first_sample_ms = now;
    }
    if (e->count < RADAR_EST_MAX_SAMPLES) {
        e->samples[e->count++] = x;
    }
}

static bool rwe_ready(const RobustWindowEstimator *e, uint32_t now)
{
    return e->count >= RADAR_EST_MIN_SAMPLES &&
           e->first_sample_ms != 0 &&
           (uint32_t)(now - e->first_sample_ms) >= RADAR_EST_WINDOW_MS;
}

static void rwe_sort(float *arr, int n)
{
    for (int i = 1; i < n; i++) {
        float key = arr[i];
        int j = i - 1;
        while (j >= 0 && arr[j] > key) {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }
}

static float rwe_median(float *arr, int n)   /* sorts arr in place */
{
    rwe_sort(arr, n);
    if (n % 2 == 1) {
        return arr[n / 2];
    }
    return 0.5f * (arr[n / 2 - 1] + arr[n / 2]);
}

/* Reduce the window to one robust value: median + MAD, discard samples
 * deviating more than RADAR_EST_OUTLIER_K × robust σ, mean of the rest. */
static bool rwe_estimate(const RobustWindowEstimator *e, float *value,
                         float *spread, int *used)
{
    if (e->count < RADAR_EST_MIN_SAMPLES) {
        return false;
    }

    float scratch[RADAR_EST_MAX_SAMPLES];

    for (int i = 0; i < e->count; i++) {
        scratch[i] = e->samples[i];
    }
    float med = rwe_median(scratch, e->count);

    for (int i = 0; i < e->count; i++) {
        scratch[i] = fabsf(e->samples[i] - med);
    }
    float mad = rwe_median(scratch, e->count);

    /* 1.4826 converts MAD to a standard-deviation-like robust spread. */
    float sigma = 1.4826f * mad;
    if (sigma < RADAR_EST_MIN_ROBUST_SIGMA) {
        sigma = RADAR_EST_MIN_ROBUST_SIGMA;
    }

    float threshold = RADAR_EST_OUTLIER_K * sigma;
    float sum       = 0.0f;
    int   n         = 0;
    for (int i = 0; i < e->count; i++) {
        if (fabsf(e->samples[i] - med) <= threshold) {
            sum += e->samples[i];
            n++;
        }
    }
    if (n < RADAR_EST_MIN_SAMPLES / 2) {
        return false;   /* window too contaminated to trust */
    }

    *value  = sum / (float)n;
    *spread = sigma;
    *used   = n;
    return true;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void radar_filter_init(RadarFilter *f)
{
    gate_reset(&f->presence);
    svf_init(&f->distance,
             RADAR_FILT_DIST_MIN_MM, RADAR_FILT_DIST_MAX_MM,
             RADAR_FILT_DIST_JUMP_MM, RADAR_FILT_DIST_TAU_MS,
             RADAR_FILT_DIST_CONFIRM_MS, RADAR_FILT_DIST_TIMEOUT_MS);
    svf_init(&f->heart,
             RADAR_FILT_HEART_MIN_BPM, RADAR_FILT_HEART_MAX_BPM,
             RADAR_FILT_HEART_JUMP_BPM, RADAR_FILT_HEART_TAU_MS,
             RADAR_FILT_HEART_CONFIRM_MS, RADAR_FILT_HEART_TIMEOUT_MS);
    svf_init(&f->breath,
             RADAR_FILT_BREATH_MIN_RPM, RADAR_FILT_BREATH_MAX_RPM,
             RADAR_FILT_BREATH_JUMP_RPM, RADAR_FILT_BREATH_TAU_MS,
             RADAR_FILT_BREATH_CONFIRM_MS, RADAR_FILT_BREATH_TIMEOUT_MS);
    rwe_reset(&f->heart_est);
    rwe_reset(&f->breath_est);
    f->heart_est_latched  = false;
    f->breath_est_latched = false;
    f->estimate           = (RadarVitalsEstimate){0};
    f->estimate_ready     = false;
    f->last_est_sample_ms = 0;
    f->heart_disp         = 0.0f;
    f->breath_disp        = 0.0f;
    f->heart_disp_ms      = 0;
    f->breath_disp_ms     = 0;
    f->heart_disp_valid   = false;
    f->breath_disp_valid  = false;
}

void radar_filter_apply(RadarFilter *f, const RadarSample *raw,
                        uint32_t now_ms, RadarSample *out)
{
    /* ── 1. Presence evidence ────────────────────────────────────────────
     * Driver presence flag, or a distance reading inside the gate, both only
     * from a sample that carries data (q != 3). */
    float dist_mm = (float)raw->distance_mm;
    bool dist_in_gate = (raw->q != 3) && (raw->distance_mm != 0u) &&
                        dist_mm >= RADAR_FILT_DIST_MIN_MM &&
                        dist_mm <= RADAR_FILT_DIST_MAX_MM;
    bool evidence = (raw->q != 3) && (raw->presence || dist_in_gate);

    gate_update(&f->presence, evidence, now_ms);

    /* ── 2. Distance ─────────────────────────────────────────────────────
     * Expire BEFORE update so a candidate left over from before an input gap
     * can never be confirmed by the first sample after the gap (the elapsed
     * gap time would otherwise count toward the confirm window).
     * 0 means "not reported" in RadarSample — never feed it. */
    svf_expire(&f->distance, now_ms);
    if (raw->q != 3 && raw->distance_mm != 0u) {
        svf_update(&f->distance, dist_mm, now_ms);
    }

    /* ── 3. Vitals — gated on stable presence AND stable distance ───────── */
    bool gating_open = f->presence.stable_present && svf_stable(&f->distance);
    bool heart_fed   = gating_open && raw->q != 3 && raw->heart_bpm  > 0.0f;
    bool breath_fed  = gating_open && raw->q != 3 && raw->breath_rpm > 0.0f;

    if (!gating_open) {
        svf_reset(&f->heart);
        svf_reset(&f->breath);
        /* Lost presence/distance — drop any held display value too, so the
         * tile doesn't keep showing a vital after the subject is gone. */
        f->heart_disp_valid  = false;
        f->breath_disp_valid = false;
    } else {
        svf_expire(&f->heart, now_ms);
        svf_expire(&f->breath, now_ms);
        if (heart_fed)  svf_update(&f->heart, raw->heart_bpm, now_ms);
        if (breath_fed) svf_update(&f->breath, raw->breath_rpm, now_ms);
    }

    /* ── 4. Compose the output sample ────────────────────────────────────── */
    out->presence    = f->presence.stable_present;
    out->distance_mm = svf_stable(&f->distance)
                           ? (uint32_t)(f->distance.value + 0.5f)
                           : 0u;

    /* Vitals output with display-hold: emit the calibrated value while stable;
     * otherwise hold the last confident value for RADAR_VITAL_HOLD_MS so the
     * mirror tile shows a vital ≤20 s old instead of nulling out during the
     * MR60BHA2's heart/breath alternation.  The hold-age is anchored to the
     * last FED-while-stable reading (heart_disp_ms updates only when a real
     * frame was incorporated), so a value held through both an svf input-gap
     * and a jump-reconfirm still expires ≤20 s after it was actually measured,
     * never stacking the two windows.  Held values are not "stable" so stage 5
     * marks them q=2; the estimate uses only fresh ones. */
    if (svf_stable(&f->heart)) {
        float hr = f->heart.value - RADAR_HEART_CAL_OFFSET_BPM;
        float hv = (hr > 0.0f) ? hr : 0.0f;
        if (heart_fed) {                /* fresh measurement → re-anchor age */
            f->heart_disp       = hv;
            f->heart_disp_ms    = now_ms;
            f->heart_disp_valid = (hv > 0.0f);
        }
        out->heart_bpm = hv;
    } else if (f->heart_disp_valid &&
               (uint32_t)(now_ms - f->heart_disp_ms) <= RADAR_VITAL_HOLD_MS) {
        out->heart_bpm = f->heart_disp;            /* held (≤20 s old) */
    } else {
        f->heart_disp_valid = false;
        out->heart_bpm      = 0.0f;
    }
    if (svf_stable(&f->breath)) {
        float br = f->breath.value - RADAR_BREATH_CAL_OFFSET_RPM;
        float bv = (br > 0.0f) ? br : 0.0f;
        if (breath_fed) {               /* fresh measurement → re-anchor age */
            f->breath_disp       = bv;
            f->breath_disp_ms    = now_ms;
            f->breath_disp_valid = (bv > 0.0f);
        }
        out->breath_rpm = bv;
    } else if (f->breath_disp_valid &&
               (uint32_t)(now_ms - f->breath_disp_ms) <= RADAR_VITAL_HOLD_MS) {
        out->breath_rpm = f->breath_disp;          /* held (≤20 s old) */
    } else {
        f->breath_disp_valid = false;
        out->breath_rpm      = 0.0f;
    }

    /* ── 5. Quality ──────────────────────────────────────────────────────
     *  0 — vitals stable, or a confidently-empty room
     *  2 — validating (presence/distance/vitals still confirming), or a
     *      stable presence whose vitals haven't locked
     *  3 — invalid input and nothing held by the filter                    */
    bool vitals_stable = svf_stable(&f->heart) || svf_stable(&f->breath);
    bool validating    = gate_warming_up(&f->presence) ||
                         (f->presence.stable_present && !vitals_stable);

    if (raw->q == 3 && !f->presence.stable_present &&
        !gate_warming_up(&f->presence)) {
        out->q = 3;
    } else if (vitals_stable) {
        out->q = 0;
    } else if (validating) {
        out->q = 2;
    } else {
        out->q = 0;   /* confidently empty: presence=false, no vitals */
    }

    /* ── 6. Final robust vitals estimate — per-vital windows, repeating ───
     * The MR60BHA2 alternates heart/breath bursts, so simultaneous stability
     * long enough for one shared window never happens on hardware (ADR-0005
     * HIL note).  Instead each vital fills its own window over its own
     * stable runs: a vital destabilising discards only its own unlatched
     * window; a window that spans RADAR_EST_WINDOW_MS is reduced
     * (median + MAD) and LATCHED into f->estimate.  When BOTH halves are
     * latched the combined estimate fires and the stage RE-ARMS (clears both
     * latches and windows) so a fresh estimate is produced each time both
     * vitals refill — roughly every window-plus-gap while the subject stays.
     * Losing presence discards any in-progress windows and half-latches. */
    if (!f->presence.stable_present) {
        rwe_reset(&f->heart_est);
        rwe_reset(&f->breath_est);
        f->heart_est_latched  = false;
        f->breath_est_latched = false;
        f->last_est_sample_ms = 0;
    } else {
        /* A vital that destabilises before its window completes loses only
         * that window; an already-latched half is kept until the pair fires. */
        if (!f->heart_est_latched && !svf_stable(&f->heart)) {
            rwe_reset(&f->heart_est);
        }
        if (!f->breath_est_latched && !svf_stable(&f->breath)) {
            rwe_reset(&f->breath_est);
        }

        if (f->last_est_sample_ms == 0 ||
            (uint32_t)(now_ms - f->last_est_sample_ms) >=
                RADAR_EST_SAMPLE_EVERY_MS) {
            f->last_est_sample_ms = now_ms;

            if (!f->heart_est_latched && svf_stable(&f->heart)) {
                rwe_add(&f->heart_est, out->heart_bpm, now_ms);
                if (rwe_ready(&f->heart_est, now_ms) &&
                    rwe_estimate(&f->heart_est, &f->estimate.heart_bpm,
                                 &f->estimate.heart_spread,
                                 &f->estimate.heart_n)) {
                    f->heart_est_latched = true;
                }
            }
            if (!f->breath_est_latched && svf_stable(&f->breath)) {
                rwe_add(&f->breath_est, out->breath_rpm, now_ms);
                if (rwe_ready(&f->breath_est, now_ms) &&
                    rwe_estimate(&f->breath_est, &f->estimate.breath_rpm,
                                 &f->estimate.breath_spread,
                                 &f->estimate.breath_n)) {
                    f->breath_est_latched = true;
                }
            }
            if (f->heart_est_latched && f->breath_est_latched) {
                f->estimate_ready = true;
                /* re-arm: next estimate starts from fresh windows */
                rwe_reset(&f->heart_est);
                rwe_reset(&f->breath_est);
                f->heart_est_latched  = false;
                f->breath_est_latched = false;
            }
        }
    }
}

bool radar_filter_take_estimate(RadarFilter *f, RadarVitalsEstimate *out)
{
    if (!f->estimate_ready) {
        return false;
    }
    *out = f->estimate;
    f->estimate_ready = false;
    return true;
}
