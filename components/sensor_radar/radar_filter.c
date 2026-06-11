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
    if (!f->presence.stable_present || !svf_stable(&f->distance)) {
        svf_reset(&f->heart);
        svf_reset(&f->breath);
    } else {
        svf_expire(&f->heart, now_ms);
        svf_expire(&f->breath, now_ms);
        if (raw->q != 3 && raw->heart_bpm > 0.0f) {
            svf_update(&f->heart, raw->heart_bpm, now_ms);
        }
        if (raw->q != 3 && raw->breath_rpm > 0.0f) {
            svf_update(&f->breath, raw->breath_rpm, now_ms);
        }
    }

    /* ── 4. Compose the output sample ────────────────────────────────────── */
    out->presence    = f->presence.stable_present;
    out->distance_mm = svf_stable(&f->distance)
                           ? (uint32_t)(f->distance.value + 0.5f)
                           : 0u;

    if (svf_stable(&f->heart)) {
        float hr = f->heart.value - RADAR_HEART_CAL_OFFSET_BPM;
        out->heart_bpm = (hr > 0.0f) ? hr : 0.0f;
    } else {
        out->heart_bpm = 0.0f;
    }
    out->breath_rpm = svf_stable(&f->breath) ? f->breath.value : 0.0f;

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
}
