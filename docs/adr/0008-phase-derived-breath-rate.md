# ADR-0008: Phase-derived breath rate (own estimator, replaces the radar's BR)

**Status:** Accepted
**Date:** 2026-06-22
**Extends:** ADR-0006 (phase-based breath-hold detection)

## Context

The published breath rate (`rmms/<uuid>/radar` `breath_bpm`) was taken straight
from the MR60BHA2's own `0x0A14` breath-rate frame, post-processed by the
plausibility filter (ADR-0005: presence/distance gating, live median, a −2 RPM
calibration offset). Users reported it "never drops to the low teens" at slow
breathing.

Bench measurement settled why (2026-06-22, subject ~0.5 m, in-gate `q=0`, paced
to a 24 bpm metronome = **12 RPM true**, 105 s capture):

- The radar's own `0x0A14` BR read a **median of 19 RPM (spread 14–25)** and
  never approached 12. It over-reports by ~58% and floors out well above slow
  rates — this is the module's internal windowed-FFT respiration estimator, not
  our filter. No post-processing of that number can recover the true rate.
- The radar *also* streams the breath-**phase** waveform (`0x0A13`, the
  chest-displacement signal ADR-0006 already buffers for hold amplitude). Its
  spectrum over the same capture was a **single clean peak at exactly 12.0 RPM**
  (0.200 Hz, relative power 1.00; next peak 0.10). Offline estimators
  (`scripts/analyze_breath.py`) recovered **12.00 RPM** (FFT and autocorrelation)
  from that waveform — i.e. the truth the radar's BR missed by 7 RPM.

So the signal needed to compute an accurate breath rate is already on the MCU;
only the radar's *derived number* is bad.

## Decision

Compute the breath rate **on the MCU from the breath-phase waveform** and publish
that instead of the radar's `0x0A14` BR.

- New host-testable module `components/sensor_radar/breath_freq.c` (pure logic,
  no HAL): select the last `BREATH_FREQ_WIN_MS` (30 s) of phase samples →
  resample to a uniform 10 Hz grid (the stream drops frames to single-byte-SOF
  false-syncs) → linear detrend → Hann window → **Goertzel periodogram** across
  the 0.10–0.50 Hz (6–30 RPM) band → **parabolic-interpolate** the peak for
  sub-grid resolution → confidence = dominant-peak / mean-in-band power.
- `radar_bha2.c` enlarges the phase ring to hold `BHA2_PHASE_KEEP_MS` (32 s,
  time-bounded), runs the estimator (throttled ~2 Hz), and writes its result to
  `RadarSample.breath_rpm`. The ADR-0006 amplitude consumer sub-selects its own
  6 s window from the same ring.
- **Confidence-gated (primary):** when the spectral peak is prominent
  (`confidence ≥ BREATH_FREQ_MIN_CONF`), in band, and the window has real
  oscillation (`RMS ≥ BREATH_FREQ_MIN_AMP`, guarding the flat/no-signal case),
  the estimate is published; otherwise `breath_rpm = 0` (the filter then blanks
  the tile). We do **not** fall back to the radar's biased BR.
- The filter's breath calibration offset `RADAR_BREATH_CAL_OFFSET_RPM` is set to
  **0**: the estimator is unbiased (bench: 12.07 RPM at a true 12); the former
  −2 RPM corrected the radar's BR and must not be reused.

Heart rate is unchanged — it still comes from the `0x0A15` frame with its −20 BPM
offset. The HMMD driver (ADR-0007) has no phase stream and continues to emit
`breath_rpm = 0` (→ null), unaffected.

The MQTT contract (§9.1/§9.2) is **unchanged** — `breath_bpm` carries the same
units and meaning; only its derivation moved from the sensor to our firmware.

## Consequences

- **Accuracy at slow rates:** the published breath rate now tracks the true rate
  (validated to ~0.2 RPM on the bench capture) instead of sitting high.
- **Latency:** a 30 s analysis window plus the filter's live median makes breath
  rate slower to follow a *change* than heart rate. Acceptable for v1 (breathing
  rate is slow-moving; accuracy was the complaint). `BREATH_FREQ_WIN_MS` is the
  tuning knob; revisit if responsiveness matters for a demo.
- **Warm-up:** breath shows ~30 s after presence is acquired (window fill); heart
  is unaffected. `q=2` during that warm-up.
- **Cost:** a Goertzel periodogram (~120 bins × ~300 samples) at ~2 Hz on the
  M33 FPU — negligible.
- **CPU/RAM:** the phase ring grows to 384 samples (~3 KB in the driver ctx) plus
  ~7 KB of file-scope scratch in `breath_freq.c` (no malloc, single caller).
- **Confidence gate is the risk surface:** too strict blanks real breathing, too
  loose shows noise. Tuned from one bench capture (clean ≥4, flat rejected);
  `BREATH_FREQ_MIN_CONF` / `BREATH_FREQ_MIN_AMP` are HIL knobs.

## Alternatives rejected

- **Tune the radar's BR (offset / median).** Can't fix a biased input; the radar
  floors out above slow rates regardless of smoothing.
- **Header TYPE-allowlist to cut the false-sync log storm** (considered while
  investigating capture quality). Adversarial review showed it desyncs and eats
  the next real frame and would drop valid-but-unknown firmware frames; the data
  checksum is already the integrity gate. Not adopted.
- **Fuse phase estimate with the radar BR.** The BR is biased high at low rates,
  so blending drags the result off truth.

## Validation

- Host: `test/host/test_breath_freq.c` — a real 32 s capture (true 12 RPM, radar
  said ~19) → ~12; synthetic 12/20 RPM sinusoids; linear-drift rejection; the
  flat/no-signal guard; minimum-span guard; circular-ring (head≠0) indexing.
- Capture tooling (`BHA2_CAPTURE` in `radar_bha2.c` + `scripts/analyze_breath.py`)
  is temporary and to be removed once the estimator is hardware-validated; the
  `CAP,E` line logs the live estimate (rpm/confidence/valid) for that check.
