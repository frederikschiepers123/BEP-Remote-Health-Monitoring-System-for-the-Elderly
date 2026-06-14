# ADR-0006: Phase-based breath-hold detection

**Status:** Accepted
**Date:** 2026-06-14

## Context

The MR60BHA2 reports a respiratory **rate** (`rmms/<uuid>/radar` `breath_bpm`)
that the supervisor and the project want to use to surface a **breath-hold**
(apnea) on the mirror. It cannot do this. The module derives the rate by
spectral analysis (FFT) of the chest-wall motion over a multi-second sliding
window, so the rate is a *frequency* estimate, not an instantaneous airflow
measurement. During a breath-hold shorter than the window the dominant
frequency peak persists, so the reported rate stays **flat** and then, if the
hold is long enough, the module drops the breath value entirely — it never
"counts down". Bench observation (2026-06-14): during a ~20 s hold the published
breath rate held flat at ~18 RPM while heart rate rose 54→67 BPM. No firmware
filtering of the *rate* can change this; it is inherent to windowed frequency
estimation.

The module does, however, stream a **raw breath-phase** signal (the `0x0A13`
phase frame, three little-endian floats `[total][breath][heart]`, several Hz) —
the chest-displacement waveform itself. Unlike the rate, the breath-phase
**oscillation flattens within seconds** when chest motion stops. The driver
previously discarded this frame as "informational".

The mirror cannot derive a hold on its own: §9.5 says derived UI values that
are *a function of the raw topics* are computed on the mirror, but the phase is
**not** on the wire — only the MCU sees it. So detection must happen on the MCU,
and the result must be delivered to consumers as a new signal. Per the §9.6
new-field process this is a JSON-schema change and therefore an ADR.

## Decision

Detect a possible breath-hold on the MCU from the breath-phase amplitude, and
publish the result as a new tri-state radar field `resp_motion`.

**Driver (`radar_bha2.c`).** Decode the breath phase (offset 4 of `0x0A13`;
float order confirmed against the Seeed/ESPHome driver — and a wrong guess is
fail-safe because the heart phase keeps swinging during a hold, so it would
simply never flag one). Keep a rolling window (`BHA2_PHASE_WIN_MS` = 6 s, ≥ one
slow breath cycle) of breath-phase samples and report their **peak-to-peak
amplitude** (`RadarSample.resp_motion_amp` / `resp_motion_amp_valid`). The
amplitude is only marked valid with enough fresh samples spanning enough time,
so a stalled phase stream (UART dropout) ages out to *invalid* rather than a
false "flat".

**Filter (`radar_filter.c`, extends ADR-0005).** Threshold the amplitude only
while presence AND distance are locked AND the amplitude is valid:

- amplitude `< RADAR_HOLD_AMP_MIN` for `≥ RADAR_HOLD_CONFIRM_MS` (5 s) ⇒
  confirmed hold;
- a single real breath (amplitude `≥ RADAR_HOLD_AMP_RESUME`) clears it
  (hysteresis stops flicker on the quiet moments at the top/bottom of a breath);
- on a confirmed hold the sample's `breath_rpm` is **nulled** (a rate we have
  detected isn't happening is not reported) and `resp_motion` is set false;
- when motion is present `resp_motion` is true; when the stage cannot judge it
  (no presence/lock, or no valid amplitude) `resp_motion_valid` is false and the
  wire emits `resp_motion: null`.

Response time is ~5–10 s (confirm window + amplitude window), far better than the
~25–40 s it takes the rate to drop out.

**Wire (`json_encode.c`, §9.2.2).** Add `resp_motion` to the radar `v` body as a
tri-state: `true` (chest motion present), `false` (possible breath-hold), `null`
(undetermined). Always present per §9.2.3.

**Mirror.** The bridge forwards `v.resp_motion` →
`sensors/respiratorymotion` ("true"/"false"/""); `MMM-SensorUI` shows a
"No breathing" tile when it is "false" (§9.5 display logic stays on the mirror).

**Thresholds are HIL-tunable.** The breath-phase unit/scale is module-specific,
so `RADAR_HOLD_AMP_MIN` / `_RESUME` ship as placeholders. `radar_task` logs the
live amplitude on the dev console (`breath-phase amp=… hold=…`): on the bench,
breathe vs hold, read the two amplitude bands, set `MIN` between them and
`RESUME` a little above. Tuning procedure mirrors the ADR-0005 calibration
offsets.

## Consequences

- **It is a "possible/suspected" indicator, not a clinical apnea alarm.** A
  posture shift, very shallow breathing, or the subject leaving can all read as
  low motion. The confirm window + presence/distance gating reduce false
  positives but do not eliminate them; clinical-advisor sign-off (§9.5) is
  required before any deployment use, exactly as for the threshold colours.
- **A new wire field needs Radxa/FHIR mapping.** `resp_motion` is added to the
  §9.6 mapping table as **TBD** — the Radxa team chooses a code (likely a SNOMED
  respiratory-observation or a custom URN) when wiring it; until then the field
  is mirror-only and the SBC may ignore it. No LOINC/UCUM is asserted on the MCU
  (§9.6 unchanged in spirit).
- **Graceful degradation.** A radar with no phase stream (or older firmware)
  leaves `resp_motion_amp_valid` false → `resp_motion` is always null → the
  mirror behaves exactly as before. Nothing else on the wire changes shape.
- **Spool/record format.** Adding the `resp_motion_*` fields grew `RadarSample`
  from 16 to ~24 bytes, which would have broken the spool's
  `static_assert(sizeof(SpoolBody) == 16)`. So the spool no longer persists the
  whole `RadarSample`: a dedicated 16-byte `SpoolRadarBody`
  (`breath_rpm`, `heart_bpm`, `distance_mm`, `presence`, and `resp_motion` as
  the `-1/0/1` tri-state) holds only the wire fields — the driver-internal
  breath-phase amplitude never reaches flash. `spool_make_radar` copies those
  fields explicitly. `SPOOL_MAGIC` is bumped (`RMS1`→`RMS2`) so pre-ADR-0006
  records are treated as invalid on mount rather than mis-decoded under the new
  body layout; losing them is a non-issue across a reflash (the spool is a
  transient circular log).
- **Breadboard caveat.** The intermittent breath-frame dropouts traced to
  marginal MR60BHA2 UART/GND on the breadboard (to be cured by the PCB) make the
  amplitude *valid* gate flip off during a dropout — correct behaviour (we don't
  flag a hold we can't see), but it means hold detection, like the rate, depends
  on a solid radar link.

## Alternatives considered

- **Infer the hold on the mirror from `breath_bpm == null` while present.**
  Rejected: it aliases a hold with warm-up, the alternation gap, and a >25 s
  UART dropout — the mirror cannot tell them apart without the phase, which it
  doesn't have. The explicit `resp_motion` signal is unambiguous.
- **Make the breath *rate* reflect a hold.** Impossible with this sensor (see
  Context); the rate is a windowed frequency computed in the module's closed
  firmware.
- **Keep it off the wire and detect on the Radxa.** The Radxa also only receives
  the rate, not the phase, so it has the same blindness as the mirror. Detection
  has to be where the phase is — the MCU.
