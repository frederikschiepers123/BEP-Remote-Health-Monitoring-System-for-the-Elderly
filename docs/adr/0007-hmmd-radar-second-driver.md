# ADR-0007: Second radar driver — Waveshare HMMD 24 GHz behind the v-table

**Status:** Accepted
**Date:** 2026-06-14

## Context

CLAUDE.md §7.4 and §3.2 describe the radar layer as a single driver
(`radar_bha2.c`, the Seeed MR60BHA2) sitting behind a `radar_driver_t`
v-table that is *explicitly retained as the extensibility seam for a future
radar*: "a future radar requires a new `radar_*.c` file and a new entry in
`radar_select.c` … It does not require changes to the task, to MQTT topics, or
to the payload schema." Until now v1 shipped exactly one driver. (A DFRobot
C1001 — a *different* 24 GHz module, unrelated to the HMMD — is tracked
separately on the same v-table seam; its current status lives in CLAUDE.md
§3.2, not in this ADR.)

Two facts make a second radar concrete now:

1. **ADR-0001 already defines a "generic" product variant** of the sensor
   module — same PCB and same firmware image — that pairs the GL5516 LDR with
   an **HMMD microwave-presence radar** instead of the MR60BHA2 + BH1750 of the
   "advanced" variant. The light half of that variant (`gl5516.c`) already
   exists; the radar half did not. Without an HMMD driver the generic variant
   cannot actually run this firmware.

2. The previous BAP group shipped a **working MicroPython HMMD driver**
   (`bestanden_vorige_BAP/sensor_module/microcontroller_files/lib/hmmd_mpy.py` — tree removed at handover; in pre-July-2026 git history),
   which CLAUDE.md §18 designates the behavioural specification. It pins the
   exact module (**Waveshare HMMD**, "Report Mode") and its wire protocol, so
   the C port is a faithful translation, not a guess.

This is the change the v-table was built for, requested directly. The Waveshare
HMMD uses its own framing — `F4 F3 F2 F1` header / `F8 F7 F6 F5` tail,
little-endian length, **no checksum** — which is a *third*, distinct protocol
from the MR60BHA2's SOF-`0x01` framing and from the Andar `0x53 0x59` family
(§3.2). Three radars, three protocols, one v-table.

## Decision

Add `components/sensor_radar/radar_hmmd.c`, a second `radar_driver_t`
implementation, and select it with `"radar": "hmmd"` in `/cfg/sensors.json`.
No change to `radar_task`, to the `RadarSample` struct, to the
`rmms/<uuid>/radar` topic, or to the §9.2 payload schema — the whole point of
the seam.

Report-mode frame (from the reference driver — unit-tested in
`test_radar_hmmd.c`):

```
[F4 F3 F2 F1] [LEN (2 B, little-endian)] PAYLOAD(LEN) [F8 F7 F6 F5]
  PAYLOAD[0]      presence       1 B   0 = no target, 1 = target
  PAYLOAD[1..2]   distance_raw   2 B   little-endian
  PAYLOAD[3..34]  gate energies  16 × 2 B little-endian (motion magnitude/gate)
  no checksum — the fixed header + fixed tail + length delimit the frame
```

`init()` first sends the one-shot Report-Mode command
(`FD FC FB FA 08 00 12 00 00 00 04 00 00 00 04 03 02 01`, verbatim from the
reference) and waits ~200 ms before reading frames.

**Capability: this radar has no vitals.** The Waveshare HMMD reports presence,
distance, and per-gate motion energy *only* — no respiration, no heart rate, no
breath-phase. So the driver always emits `breath_rpm = 0` and `heart_bpm = 0`
(→ `null`, §9.2.2) and leaves `resp_motion_amp_valid = false`, so the ADR-0006
breath-hold feature stays inert. That is exactly the graceful degradation
`radar_driver.h` documents for "a radar with no phase stream": on an HMMD module
the mirror's heart/breath tiles have no data, while presence + distance — the
generic module's ADL/presence job (ADR-0001) — work fully.

Only one numeric assumption needs the bench: **distance units.** The reference's
`distance_to_meters()` multiplied the raw value by a 0.7 m gate length, but on
this LD2410-class silicon the field reads as detection distance in centimetres,
which yields plausible indoor values; the driver converts cm→mm
(`HMMD_DIST_MM_PER_RAW`) and carries a `TODO(spec)` to confirm against a tape
measure (CLAUDE.md §16 convention). The ADR-0005 plausibility filter gates
absurd distances regardless, and a wrong unit never fabricates vitals (there are
none) — consistent with the audit rule against silently substituting bad values
(§13.6 / §19.1).

The HMMD module reuses the shared radar UART (`BOARD_RADAR_*`, uart1, 115200) —
**no `board_pico2wh.h` change**, so this ADR does not touch the pin map.

## Consequences

**Easier:**
- The ADR-0001 generic variant can now run the production firmware unchanged —
  only `/cfg/sensors.json` differs (`"radar":"hmmd"`, `"light":"gl5516"`).
- The seam is now proven by a second implementation, not just asserted; adding
  a third radar is again one file + one `radar_select.c` branch.
- Same `rmms/<uuid>/radar` schema for both radars, so the Radxa FHIR mapping
  (§9.6) and the MMM-SensorUI tiles need no per-radar branch.

**Harder:**
- The HMMD path cannot be hardware-verified in this project (we demo the
  advanced module). The protocol is a faithful port of the previous group's
  working driver and the framing is covered by host unit tests
  (`test/host/test_radar_hmmd.c`), but the one `TODO(spec)` — the distance unit
  — should be bench-confirmed before an HMMD deployment, same caveat ADR-0001
  records for the GL5516.
- An HMMD sensor module silently has no heart/breath data. That is by design
  (the part has no such sensor), but downstream consumers must treat those
  fields as legitimately `null`, not as a fault.

**Neutral:**
- One more `.c` file and one more `CfgRadarKind` value. The single-driver
  prose in CLAUDE.md §3.2 / §7.4 / §16 is updated to "two drivers behind the
  v-table" to keep the source-of-truth document honest.

## Implementation pointer

- HMMD driver: `components/sensor_radar/radar_hmmd.c` (`radar_hmmd_driver()`)
- Driver interface: `components/sensor_radar/radar_driver.h`
- Driver selector: `components/sensor_radar/radar_select.c` (`"hmmd"` branch)
- Build: `components/sensor_radar/CMakeLists.txt`
- Config schema: `components/cfg/cfg.{h,c}` — `CFG_RADAR_HMMD`
- Host tests: `test/host/test_radar_hmmd.c` (registered in `run.sh` + `CMakeLists.txt`)
- Bring-up: `docs/bring_up.md` step 7b (HMMD variant + selection)
- Behavioural reference: `bestanden_vorige_BAP/.../lib/hmmd_mpy.py` (CLAUDE.md §18; tree removed at handover — pre-July-2026 git history)
