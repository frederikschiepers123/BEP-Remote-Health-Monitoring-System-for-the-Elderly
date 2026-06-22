/* Phase-waveform breath-rate estimator — see breath_freq.h (ADR-0008, which
 * extends ADR-0006). Pure logic, no pico-sdk / FreeRTOS dependency, host-built. */
#include "breath_freq.h"

#include <math.h>

#define BF_PI 3.14159265358979f

/* Scratch buffers. File-scope (not stack) per CLAUDE.md §13.4 "no malloc in
 * steady state"; safe as plain statics because the only caller is the single
 * radar_task (one estimate at a time, never re-entrant). Sized for the worst
 * case of the tunables above:
 *   SEL : raw in-window samples — bounded by the caller's ring capacity.
 *   RS  : resampled grid = WIN_MS/1000 * RESAMPLE_HZ + 1 = 301.
 *   GRID: (BAND_HI-BAND_LO)/STEP + 1 = 121. */
#define BF_SEL_MAX   512
#define BF_RS_MAX    384
#define BF_GRID_MAX  160

static float    s_sel_v[BF_SEL_MAX];
static uint32_t s_sel_t[BF_SEL_MAX];
static float    s_rs[BF_RS_MAX];
static float    s_pow[BF_GRID_MAX];
static float    s_freq[BF_GRID_MAX];

/* Goertzel single-bin power at arbitrary frequency f (Hz) over x[0..n-1]
 * sampled at fs (Hz). Returns |X(f)|^2. */
static float goertzel_power(const float *x, int n, float f, float fs) {
    float w     = 2.0f * BF_PI * f / fs;
    float coeff = 2.0f * cosf(w);
    float s1 = 0.0f, s2 = 0.0f;
    for (int i = 0; i < n; i++) {
        float s0 = x[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

BreathFreqResult breath_freq_estimate(const float *val, const uint32_t *ts_ms,
                                      int cap, int head, int count,
                                      uint32_t now_ms) {
    BreathFreqResult r = { 0.0f, 0.0f, false };
    if (count < BREATH_FREQ_MIN_SAMPLES || cap <= 0) {
        return r;
    }

    /* 1. Select in-window samples (time-ordered: ring is oldest→newest). */
    int n = 0;
    for (int i = 0; i < count && n < BF_SEL_MAX; i++) {
        int idx = (head + i) % cap;
        uint32_t age = now_ms - ts_ms[idx];      /* unsigned wrap-safe */
        if (age <= BREATH_FREQ_WIN_MS) {
            s_sel_t[n] = ts_ms[idx];
            s_sel_v[n] = val[idx];
            n++;
        }
    }
    if (n < BREATH_FREQ_MIN_SAMPLES) {
        return r;
    }
    uint32_t span_ms = s_sel_t[n - 1] - s_sel_t[0];
    if (span_ms < BREATH_FREQ_MIN_SPAN_MS) {
        return r;
    }

    /* 2. Resample to a uniform grid (linear interpolation). */
    const float fs = BREATH_FREQ_RESAMPLE_HZ;
    int N = (int)((float)span_ms / 1000.0f * fs) + 1;
    if (N > BF_RS_MAX) {
        N = BF_RS_MAX;
    }
    if (N < BREATH_FREQ_MIN_SAMPLES) {
        return r;
    }
    uint32_t t0 = s_sel_t[0];
    int j = 0;
    for (int k = 0; k < N; k++) {
        float tk_ms = (float)k / fs * 1000.0f;
        while (j < n - 2 && (float)(s_sel_t[j + 1] - t0) < tk_ms) {
            j++;
        }
        float ta = (float)(s_sel_t[j]     - t0);
        float tb = (float)(s_sel_t[j + 1] - t0);
        float dt = tb - ta;
        float fr = (dt <= 0.0f) ? 0.0f : (tk_ms - ta) / dt;
        s_rs[k] = s_sel_v[j] + fr * (s_sel_v[j + 1] - s_sel_v[j]);
    }

    /* 3. Linear detrend (remove DC + slow drift). */
    float mid = (float)(N - 1) / 2.0f;
    float mean = 0.0f;
    for (int k = 0; k < N; k++) {
        mean += s_rs[k];
    }
    mean /= (float)N;
    float sxx = 0.0f, sxy = 0.0f;
    for (int k = 0; k < N; k++) {
        float dx = (float)k - mid;
        sxx += dx * dx;
        sxy += dx * (s_rs[k] - mean);
    }
    float slope = (sxx > 0.0f) ? sxy / sxx : 0.0f;
    float ss = 0.0f;
    for (int k = 0; k < N; k++) {
        s_rs[k] -= mean + slope * ((float)k - mid);
        ss += s_rs[k] * s_rs[k];
    }
    /* Reject a flat / no-oscillation window (nobody breathing): otherwise DC and
     * rounding leakage through the Hann window fabricates a low-frequency peak
     * that would clear the confidence gate as a bogus ~6 RPM reading. */
    if (sqrtf(ss / (float)N) < BREATH_FREQ_MIN_AMP) {
        return r;
    }

    /* 4. Hann window. */
    for (int k = 0; k < N; k++) {
        float wnd = 0.5f - 0.5f * cosf(2.0f * BF_PI * (float)k / (float)(N - 1));
        s_rs[k] *= wnd;
    }

    /* 5. Goertzel periodogram across the respiration band. */
    int M = 0;
    for (float f = BREATH_FREQ_BAND_LO_HZ;
         f <= BREATH_FREQ_BAND_HI_HZ + 1e-6f && M < BF_GRID_MAX;
         f += BREATH_FREQ_STEP_HZ) {
        s_freq[M] = f;
        s_pow[M]  = goertzel_power(s_rs, N, f, fs);
        M++;
    }
    if (M < 3) {
        return r;
    }

    /* 6. Peak + parabolic interpolation for sub-grid resolution. */
    int im = 0;
    for (int i = 1; i < M; i++) {
        if (s_pow[i] > s_pow[im]) {
            im = i;
        }
    }
    /* Reject a peak pinned at the lowest in-band bin. A non-breathing window that
     * is merely non-flat — a step, a settling/exponential drift, a slow bump that
     * survives the linear detrend — has a ~1/f spectrum that decays monotonically
     * across the band, so its maximum sits at the very first bin (0.10 Hz / 6 RPM).
     * Such an input clears both the RMS floor and the peak/mean confidence gate
     * (it is not flat, and the edge bin towers over the decaying tail), yet it is
     * not respiration. A genuine tone — even true 6 RPM bradypnea — would also pin
     * here and is sacrificed, but that is the conservative trade: 6 RPM is the band
     * floor (MIN_RPM) where a bin-0 peak is inherently ambiguous between bradypnea
     * and a slow transient, and real respiration in this system sits well inside
     * the band (the validated 12 RPM capture lands at bin ~30). */
    if (im == 0) {
        return r;
    }
    float f_peak = s_freq[im];
    if (im > 0 && im < M - 1) {
        float y0 = s_pow[im - 1], y1 = s_pow[im], y2 = s_pow[im + 1];
        float den = y0 - 2.0f * y1 + y2;
        if (den != 0.0f) {
            float off = 0.5f * (y0 - y2) / den;
            if (off >  1.0f) off =  1.0f;
            if (off < -1.0f) off = -1.0f;
            f_peak = s_freq[im] + off * BREATH_FREQ_STEP_HZ;
        }
    }

    /* 7. Confidence = how far the dominant peak stands above the mean in-band
     * power (an SNR-like ratio).  Mean-based (not peak/next-peak) so a flat /
     * no-signal input — all powers ~0 — fails the guard below instead of
     * dividing by a zero "second peak" and falsely passing. */
    float band_mean = 0.0f;
    for (int i = 0; i < M; i++) {
        band_mean += s_pow[i];
    }
    band_mean /= (float)M;
    if (s_pow[im] <= 0.0f || band_mean <= 0.0f) {
        return r;                       /* no spectral energy → no breathing */
    }
    float conf = s_pow[im] / band_mean;
    float rpm  = f_peak * 60.0f;

    r.confidence = conf;
    r.valid = (conf >= BREATH_FREQ_MIN_CONF) &&
              (rpm >= BREATH_FREQ_MIN_RPM) && (rpm <= BREATH_FREQ_MAX_RPM);
    r.rpm = r.valid ? rpm : 0.0f;
    return r;
}
