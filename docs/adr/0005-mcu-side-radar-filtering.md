# ADR-0005: MCU-side radar plausibility filtering

**Status:** Accepted
**Date:** 2026-06-11

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
  to assert presence; 2 s of silence to drop it.
- **Distance** — gate 350–1500 mm, >200 mm jump restarts validation, 6 s
  confirm, then time-aware low-pass (τ = 8 s), 5 s input timeout.
- **Vitals (heart/breath)** — processed only while presence AND distance are
  stable; plausibility gates (45–125 BPM / 6–30 RPM), jump guards (8 BPM /
  3 RPM), 10 s confirm, low-pass (τ = 12 s / 15 s), 6 s input timeout; reset
  whenever the gating collapses.
- **Heart calibration** — `RADAR_HEART_CAL_OFFSET_BPM` (20 BPM, from the
  reference's bench observation) subtracted at output.

The published `RadarSample` keeps its shape and conventions (0/0.0 = absent →
JSON `null`; `q` 0/2/3), so no wire-contract, mirror, or SBC change is
needed: subscribers now simply see debounced presence and smoothed vitals,
with `q=2` while the filter is validating.

The filter is wired into both the production `radar_task` and the
`bringup_sensors` demo firmware, and is host-tested
(`test/host/test_radar_filter.c`).

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
