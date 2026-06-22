#ifndef BREATH_FREQ_H
#define BREATH_FREQ_H

#include <stdbool.h>
#include <stdint.h>

/* Phase-waveform breath-rate estimator (ADR-0008, extends ADR-0006).
 *
 * The MR60BHA2's OWN breath rate (the 0x0A14 frame) over-reports badly at slow
 * breathing — bench-measured ~19 RPM while the subject paced a true 12, and it
 * never reaches the low teens (see docs/adr/ADR-0006 + project memory). Its
 * breath-PHASE waveform (0x0A13, the chest-displacement signal we already buffer
 * for hold detection), however, is a clean sinusoid at the true rate.
 *
 * This module estimates the breath rate directly from that waveform:
 *   1. select the last BREATH_FREQ_WIN_MS of samples,
 *   2. resample to a uniform grid (the phase stream is ~uniform but drops
 *      frames to single-byte-SOF false-syncs),
 *   3. remove DC + linear drift, apply a Hann window,
 *   4. Goertzel periodogram across the respiration band,
 *   5. parabolic-interpolate the peak for sub-grid resolution,
 *   6. report an SNR-like confidence (dominant peak / mean in-band power) so the
 *      caller can blank the value when the signal is just noise.
 *
 * Pure logic, no HAL — host-testable (test/host/test_breath_freq.c). Validated
 * on a real 105 s capture: returns 12.0-12.2 RPM where the radar reported 19.
 */

/* ── Tunables ──────────────────────────────────────────────────────────────
 * WIN_MS sets accuracy-vs-responsiveness: 30 s gives ~0.2 RPM error at 12 RPM
 * with confidence ~5 on the bench capture; shorter tracks faster but resolves
 * low rates less well. MIN_CONF (peak/mean-band-power) gates out noise: clean
 * breathing scored ~4-12, a flat/absent signal fails the zero-energy guard. */
#define BREATH_FREQ_RESAMPLE_HZ   10.0f
#define BREATH_FREQ_WIN_MS        30000u   /* analysis window                   */
#define BREATH_FREQ_BAND_LO_HZ    0.10f    /* 6 RPM                             */
#define BREATH_FREQ_BAND_HI_HZ    0.50f    /* 30 RPM                            */
#define BREATH_FREQ_STEP_HZ       0.00333f /* ~0.2 RPM grid                     */
#define BREATH_FREQ_MIN_SPAN_MS   20000u   /* need this much data before judging */
#define BREATH_FREQ_MIN_SAMPLES   60       /* raw in-window samples required     */
#define BREATH_FREQ_MIN_CONF      3.0f     /* peak/mean-band-power gate          */
#define BREATH_FREQ_MIN_AMP       0.01f    /* detrended-RMS floor (phase units): */
                                           /* reject a flat / no-oscillation     */
                                           /* window; real breathing RMS ~0.6    */
#define BREATH_FREQ_MIN_RPM       6.0f
#define BREATH_FREQ_MAX_RPM       30.0f

typedef struct {
    float rpm;         /* estimated breaths/min (0 when !valid)                  */
    float confidence;  /* dominant-peak / mean-in-band power (SNR-like)           */
    bool  valid;       /* span + samples + confidence + range all satisfied      */
} BreathFreqResult;

/* Estimate the breath rate from a ring of breath-phase samples.
 *
 * val[] / ts_ms[] form a circular buffer of capacity `cap` holding `count`
 * time-ordered entries starting at index `head` (oldest). `now_ms` is the
 * current device time; only samples within BREATH_FREQ_WIN_MS of it are used.
 * The buffer is read-only — no eviction here. */
BreathFreqResult breath_freq_estimate(const float *val, const uint32_t *ts_ms,
                                      int cap, int head, int count,
                                      uint32_t now_ms);

#endif /* BREATH_FREQ_H */
