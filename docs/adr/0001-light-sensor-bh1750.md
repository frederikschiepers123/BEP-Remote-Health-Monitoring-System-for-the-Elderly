# ADR-0001: Replace GL5516 LDR with Rohm BH1750FVI for ambient light

**Status:** Accepted
**Date:** 2026-06-05

## Context

CLAUDE.md §3.2 originally specified a **GL5516 LDR** wired through a
voltage divider into one of the RP2350 ADC inputs as the ambient-light
sensor, producing a raw single-ended voltage that the firmware would
convert into a relative-light estimate.

While bringing the sensor module up against the live PCB, the populated
part turned out to be a **Rohm BH1750FVI** — a digital ambient-light IC
on the existing I²C0 bus (default address `0x23`). This is a deliberate
hardware choice, not a substitution-of-convenience, driven by a forward
requirement we hadn't recorded:

> The sensor module needs to be **re-targetable as a generic environmental
> monitor**. One of the configurations under consideration is using
> ambient-light changes as a coarse **presence-detection signal in
> non-bedroom rooms** (rooms where the mmWave radar isn't deployed, or
> where light-level patterns are a cheaper proxy for occupancy than a
> 60 GHz radar). For that, the firmware needs **calibrated lux** with a
> wide dynamic range, not a raw voltage that drifts with supply
> tolerance and LDR temperature coefficient.

The GL5516 fails on both counts: its output is uncalibrated (gain depends
on the divider resistor + ambient temperature + part-to-part tolerance,
typically ±50 %), and its useful range is roughly two decades. The
BH1750 produces 16-bit calibrated counts mapping linearly to lux across
**five decades** (~1 lx to ~65 535 lx), with a 1 lx-step resolution
mode that's adequate for room-level light-change detection.

## Decision

Use the **Rohm BH1750FVI** as the light sensor for v1 and document the
deviation in §3.2.

- I²C0 at the default `0x23` (no conflict with BME280 `0x76`,
  AHT21 `0x38`, ENS160 `0x53`, SH1122 `0x3C`).
- Continuous H-resolution mode (~120 ms per measurement, 1 lx
  resolution).
- Driver lives at `components/sensor_light/bh1750.{h,c}`. The
  `sensor_light/` directory dependency in CLAUDE.md §6 is updated;
  the planned `hardware_adc` link is dropped from `sensor_light/CMakeLists.txt`.
- MQTT payload (§9.2.2) is unchanged: `{"lux": <float>}` on
  `rmms/<uuid>/light`.

The GL5516 footprint is dropped from the PCB design (the BH1750
breakout occupies that role); restoring it would require a new ADR.

## Consequences

**Easier:**
- Lux output is directly usable for the "presence-via-light" downstream
  use case without per-device calibration.
- One less ADC channel + one less voltage divider on the PCB.
- The light driver shares the existing I²C0 bring-up; no separate
  ADC bring-up step in §15.

**Harder:**
- Adds one more participant to the shared I²C0 bus. The current set
  (`AHT21`, `ENS160`, `SH1122`, `BH1750`, optionally `BME280`) is well
  under the 7-bit address space and well under what 400 kHz can serve,
  but the bus is now full enough that any future I²C sensor must check
  for address conflicts before being added.
- I²C bus contention adds tail latency to OLED frame updates if the
  BH1750 happens to be in the middle of a continuous-mode read when the
  display flushes a frame. Mitigation: the `sh1122_flush` path already
  yields between row writes, and the BH1750 read is two bytes.

**Neutral:**
- Driver API and topic shape are unchanged from §9.2.2, so the FHIR
  mapping (§9.6) and the mirror UI (§9.5.1) need no changes. The
  Radxa-side FHIR builder doesn't care which physical part produced
  the lux value.

## Implementation pointer

- Driver: `components/sensor_light/bh1750.c`
- Pin/address map: `BOARD_BH1750_ADDR` in `components/board/board_pico2wh.h`
- Bring-up integration: `test/bringup/sensors_publish.c`
  (`SENSOR_LIGHT_ON` switch, publishes `rmms/<uuid>/light` at 0.2 Hz)
