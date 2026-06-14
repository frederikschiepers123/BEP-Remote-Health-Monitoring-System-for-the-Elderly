# ADR-0007: Second radar driver — Seeed 24 GHz HMMD behind the v-table

**Status:** Accepted
**Date:** 2026-06-14

## Context

CLAUDE.md §7.4 and §3.2 describe the radar layer as a single driver
(`radar_bha2.c`, the Seeed MR60BHA2) sitting behind a `radar_driver_t`
v-table that is *explicitly retained as the extensibility seam for a future
radar*: "a future radar requires a new `radar_*.c` file and a new entry in
`radar_select.c` … It does not require changes to the task, to MQTT topics, or
to the payload schema." Until now v1 shipped exactly one driver, and the
DFRobot C1001 (the previously-carried alternative) had been removed.

Two facts make a second radar concrete now:

1. **ADR-0001 already defines a "generic" product variant** of the sensor
   module — same PCB and same firmware image — that pairs the GL5516 LDR with
   an **HMMD microwave-presence radar** instead of the MR60BHA2 + BH1750 of the
   "advanced" variant. The light half of that variant (`gl5516.c`) already
   exists; the radar half did not. Without an HMMD driver the generic variant
   cannot actually run this firmware.

2. The previous BAP group's module was an **HMMD** sensor (CLAUDE.md §18 cites
   its protocol as behavioural reference). Supporting it lets the same image
   drive both the new advanced module and a board carrying the previous-style
   24 GHz part.

This is the change the v-table was built for, requested directly. The HMMD
module uses the Seeed/Andar 24 GHz framing — `0x53 0x59 … 0x54 0x43` — which
CLAUDE.md §3.2 already calls out as *the framing the MR60BHA2 does NOT use*
(i.e. the firmware always knew this was the "other" family).

## Decision

Add `components/sensor_radar/radar_hmmd.c`, a second `radar_driver_t`
implementation, and select it with `"radar": "hmmd"` in `/cfg/sensors.json`.
No change to `radar_task`, to the `RadarSample` struct, to the
`rmms/<uuid>/radar` topic, or to the §9.2 payload schema — the whole point of
the seam.

Frame envelope (the stable, well-documented part — unit-tested rigorously):

```
[0x53][0x59] [CTRL] [CMD] [LEN_H][LEN_L] ── DATA(LEN) ── [CKSUM] [0x54][0x43]
  LEN   big-endian, length of DATA only
  CKSUM (sum of every byte from 0x53 through the last DATA byte) & 0xFF
  tail  literal 0x54 0x43
```

`(CTRL, CMD)` → `RadarSample` field mapping (presence, respiration rate, heart
rate, distance) is encoded as **named constants marked `TODO(spec)`**: the
codes are firmware-revision-dependent on these modules, so a bench session can
correct one number without touching the parser. This mirrors exactly how the
BHA2 driver was treated before its bench bring-up resolved §16 Q2, and the
repo's standing `TODO(spec)` convention (§16).

Two graceful-degradation choices, both already sanctioned by the contract:

- **No breath-phase stream.** HMMD modules do not emit per-sample
  chest-displacement data, so the driver leaves `resp_motion_amp_valid = false`
  and the ADR-0006 breath-hold feature stays inert downstream — exactly the
  behaviour `radar_driver.h` documents for "a radar with no phase stream".
- **Heart rate optional.** 24 GHz micro-motion variants often report only
  presence + respiration; when no heart frame arrives `heart_bpm` stays `0` →
  `null` on the wire, which §9.2.2 explicitly permits ("may be `null`").

A mis-decoded `(CTRL, CMD)` is fail-safe: the frame is skipped (its bytes still
count as link activity for presence staleness), so a wrong code degrades to
"presence only", never to fabricated vitals — consistent with the audit rule
against silently substituting bad values (CLAUDE.md §13.6 / §19.1).

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
  advanced module). The driver is reviewed on logic and covered by host unit
  tests on the frame envelope + field wiring (`test/host/test_radar_hmmd.c`),
  but the `TODO(spec)` `(CTRL, CMD)` codes must be bench-confirmed before any
  HMMD deployment — same caveat ADR-0001 records for the GL5516.

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
- Bring-up: `docs/bring_up.md` step 10 (HMMD) and step 11 (selection)
