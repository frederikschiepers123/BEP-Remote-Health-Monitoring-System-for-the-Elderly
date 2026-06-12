# ADR-0005: MCU-side radar plausibility filtering

**Status:** Accepted
**Date:** 2026-06-11 (amended same day for the supervisor's updated reference:
absence window 2 s → 8 s, breath calibration offset, final robust estimate;
then again after HIL: vital timeouts 6 s → 15 s/20 s, estimate window 60 s →
20 s — see "Hardware-in-the-loop tuning")

## Context

CLAUDE.md §2.3 originally placed all filtering of mmWave "ghost" readings on
the Radxa: the firmware publishes raw samples plus a quality flag, and the
aggregator decides what to trust. In practice the raw MR60BHA2 stream proved
too noisy for the system's primary consumer, the MagicMirror UI, which
subscribes to the **raw** topics directly (§9.5) and therefore renders every
flicker: vitals jump tens of BPM between samples, ghost targets blip the
presence flag, and distance jitters. The supervisor reviewed the live system
and directed that the radar readings be stabilised, providing a reference
filter implementation (Arduino sketch, `MR60 filtering.txt`) with presence
debouncing, plausibility gates, jump guards, confirmation windows, and
time-aware low-pass smoothing — explicitly approving MCU-side placement
("since it's pretty basic, it can be done on the MCU no problem").

Keeping the filter on the Radxa would not help the mirror: it consumes the
firmware's raw topics by design, and routing the mirror through the Radxa
would invert §9.5's architecture for a worse result.

## Decision

Port the supervisor's reference filter into the firmware as
`components/sensor_radar/radar_filter.{h,c}` — a pure-logic stage applied
**above** the `radar_driver_t` v-table, on the generic `RadarSample`:

- **Presence debounce** — 10 s of evidence (driver flag or in-gate distance)
  to assert presence; 8 s of silence to drop it (the reference's first
  revision used 2 s; the updated reference raised it to 8 s).
- **Distance** — gate 350–1500 mm, >200 mm jump restarts validation, 6 s
  confirm, then time-aware low-pass (τ = 8 s), 5 s input timeout.
- **Vitals (heart/breath)** — processed only while presence AND distance are
  stable; plausibility gates (45–125 BPM / 6–30 RPM), jump guards (8 BPM /
  3 RPM), 10 s confirm, low-pass (τ = 12 s / 15 s), input timeout 15 s (heart)
  / 20 s (breath) — raised from the reference's 6 s after HIL, see below;
  reset whenever the gating collapses.
- **Calibration** — `RADAR_HEART_CAL_OFFSET_BPM` (20 BPM) and
  `RADAR_BREATH_CAL_OFFSET_RPM` (2 RPM, added by the updated reference; both
  from bench observation) subtracted at output.
- **Final robust vitals estimate** (updated reference) — one calibrated
  sample per second is collected **per vital, over that vital's own stable
  runs** (decoupled after HIL — see below; the reference's simultaneous
  60 s window never completed on this sensor), into a 20 s window (min 15
  samples), then reduced with median + MAD outlier rejection (>2.5 robust σ
  discarded, σ floored at 0.5) and latched; both vitals latched ⇒ the
  combined estimate ± spread fires and the stage re-arms (windows + latches
  cleared), so it repeats roughly every window-plus-gap while the subject
  stays present (losing presence discards in-progress windows). Surfaced via
  `radar_filter_take_estimate()` on the **dev console log only** — the §9.1
  topic set and §9.2.2 radar schema are closed, so publishing it would be a
  separate ADR plus Radxa sign-off.

The published `RadarSample` keeps its shape and conventions (0/0.0 = absent →
JSON `null`; `q` 0/2/3), so no wire-contract, mirror, or SBC change is
needed: subscribers now simply see debounced presence and smoothed vitals,
with `q=2` while the filter is validating.

The filter is wired into both the production `radar_task` and the
`bringup_sensors` demo firmware, and is host-tested
(`test/host/test_radar_filter.c`).

## Hardware-in-the-loop tuning (2026-06-11)

A live bench run (Pico 2 W → tablet Mosquitto over the iPhone hotspot, ~5 min
occupied) confirmed the core filter on hardware: presence locked at 10.0 s
(measured against device `ts_us`), distance smoothed cleanly, vitals locked
~10 s after presence with calibrated values (breath 15.8–22.0 RPM, heart
43–84 BPM displayed), and the 8 s absence window dropped presence ~8 s after
the subject left. The empty-room baseline never false-asserted presence.

It also surfaced one sensor behaviour the reference's constants did not
anticipate: **the MR60BHA2 reports heart and breath in long alternating
bursts.** While one streams, the other is null for a median ~9 s (heart) /
~14 s (breath), p90 ~20 s. The reference's 6 s vital input-timeout expired the
idle vital mid-burst, so the two were rarely stable simultaneously (longest
co-stable window observed: 29 s) and the robust estimate — which requires
*both* stable for its whole window — never completed on hardware.

Two coordinated changes resolve this (the estimate is a convenience readout,
not on the wire, so neither affects the MQTT contract):

- **Vital input-timeouts raised** `RADAR_FILT_HEART_TIMEOUT_MS` 6 s → 15 s and
  `RADAR_FILT_BREATH_TIMEOUT_MS` 6 s → 20 s, so each vital *holds* through the
  other's burst instead of expiring. With the driver's 5 s latch the effective
  hold is ~20 s / ~25 s. This is safe against stale-after-departure because the
  presence/distance gating force-resets both vitals within `ABSENCE_CONFIRM_MS`
  (8 s) of the subject leaving — the longer timeout only extends holds *while
  present*. Jump guards (8 BPM / 3 RPM) are unchanged; only ~4% of heart deltas
  tripped them, so timeout-expiry, not jump, was the dominant reset.
- **Estimate window shortened** `RADAR_EST_WINDOW_MS` 60 s → 20 s and
  `RADAR_EST_MIN_SAMPLES` 45 → 15, so the window fits inside the co-stability
  the sensor can actually sustain. This was a two-step cut: 60 s → 30 s up
  front, then 30 s → 20 s after a second HIL run showed the window where
  *both* vitals are stable at once tops out at ~29 s (and is usually shorter,
  because breath's 3 RPM jump guard trips on real 14↔21 RPM swings), so even
  30 s never completed. At the demo build's ~1.3 s publish cadence a 20 s
  window yields ~15 collected samples, hence the 15-sample floor.

A host test (`test_alternating_bursts_keep_costable`) models the alternating
pattern and asserts both vitals stay `q=0` and the estimate fires.

The live tiles — the actual mirror deliverable — were verified good after the
timeout change (q held 0 for 64% of presence, heart stable in runs up to 35 s,
all sensors up).

A third HIL run showed the simultaneous-window estimate was still a lottery:
the longest both-stable run was 8 s, because a vital returning after a burst
often comes back shifted past its jump guard (real heart swings 66→93→46 raw),
forcing a 10 s re-confirm — a failure mode timeouts cannot bridge. The
estimate stage was therefore **decoupled**: each vital fills its own 20 s
window over its *own* stable runs (a vital destabilising discards only its
own unlatched window), the completed window is reduced (median + MAD) and
latched, and the combined estimate fires once both vitals have latched. It
then **re-arms** (clearing both windows and latches) so a fresh estimate is
produced repeatedly — roughly every window-plus-gap — while the subject stays
present, rather than once per visit (a live monitor should refresh; losing
presence discards any in-progress windows). Host tests:
`test_decoupled_estimate_no_costability` drives heart and breath stable in
strictly alternating phases (never simultaneously) and asserts the estimate
fires; `test_final_estimate_repeats_while_present` asserts it re-fires on a
window-plus-gap cadence under sustained presence.

## Consequences

- Easier: stable mirror tiles and presence-driven screen switching without
  any mirror/Radxa logic; one filter serves any future radar behind the
  v-table (e.g. HMMD), since it sees only `RadarSample`.
- Harder: the wire no longer carries the raw radar values (the Radxa receives
  filtered data too); a reading is only published as valid after its
  confirmation window (~20 s to first vitals on a fresh lock), and tuning
  the filter now requires a firmware reflash.
- CLAUDE.md §2.3 is amended accordingly: radar plausibility filtering is
  MCU-side (this ADR); the Radxa retains clinical thresholding and any
  further aggregation-level analysis.
