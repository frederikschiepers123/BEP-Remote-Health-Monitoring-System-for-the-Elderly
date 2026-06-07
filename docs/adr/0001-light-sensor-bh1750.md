# ADR-0001: Two light sensors — BH1750 (advanced) and GL5516 (generic)

**Status:** Accepted
**Date:** 2026-06-05  (revised 2026-06-07)

## Context

CLAUDE.md §3.2 originally specified a single light sensor (the GL5516 LDR
on ADC). During bring-up it became clear that the project has two
distinct sensor-module product variants on the same PCB layout, with
different sensor populations and different downstream consumers:

| Module variant | Radar (presence + vitals) | Light | Downstream |
|---|---|---|---|
| **Advanced** (this project's demo) | Seeed MR60BHA2 (60 GHz, heart + breath + presence) | **Rohm BH1750FVI** (on the MR60BHA2 breakout) over I²C0 at `0x23` | MagicMirror² UI |
| **Generic** (software + PCB compatible, not physically demoed) | HMMD microwave-presence on GPIO | **GL5516 LDR** on ADC0 / GPIO26 through a voltage-divider with a 1 kΩ resistor to GND | Raw MQTT only |

Why two light sensors instead of one? **Both are there to better map
Activities of Daily Living (ADL)** — specifically by recording when room
lights are switched on and off, which is a recognised behavioural proxy
for daily activity patterns in elderly home-monitoring. The two
variants chose different sensors based on what else lives in the room:

- The **advanced** module ships next to a MagicMirror² in the
  bedroom/bathroom; the MR60BHA2 breakout already integrates a BH1750
  so we take it for free. Calibrated lux out, no extra BoM line.
- The **generic** module is intended to be dropped into rooms with no
  radar UI and no two-way mirror, doing pure presence + light monitoring
  with the cheapest sensible parts. The GL5516 + 1 kΩ + ADC path fits
  that brief: ~€0.10 in components, no extra I²C address, calibration
  done in firmware via a power-law fit (`R_LDR = A × lux^(-B)`).

The original ADR draft (dated 2026-06-05) incorrectly described this as
a "GL5516 → BH1750 spec swap" with the GL5516 footprint being dropped.
That's wrong: the PCB intentionally supports **both populations**,
selectable per board at build/provisioning time. This revision corrects
the record.

## Decision

The firmware exposes both as first-class drivers behind a common
`light_driver_t` v-table (mirrors the `env_driver_t` pattern adopted for
the BME280/AHT21 choice in CLAUDE.md §3.2):

```c
typedef struct {
    err_t (*init)(void *ctx);
    err_t (*read_sample)(void *ctx, LightSample *out);
    const char *name;
    void *ctx;
} light_driver_t;

light_driver_t *light_bh1750_driver(void);   /* I²C0 @ 0x23 */
light_driver_t *light_gl5516_driver(void);   /* ADC0, GPIO26 */
light_driver_t *light_select_from_config(void);
```

Selection is by `/cfg/sensors.json`'s `"light"` field
(`"bh1750"` | `"gl5516"`), defaulting to `"bh1750"` so the advanced
module — which is what's physically demoed — needs no special
provisioning. The MQTT topic and payload are unchanged from §9.2.2:
`rmms/<uuid>/light` carrying `{"lux": <float>}`.

The GL5516 driver implements the math from the reference Arduino sketch:

```
V_adc      = ADC_counts × VCC / 4095
R_LDR      = R_fixed × (VCC / V_adc - 1)
lux        = (LDR_A / R_LDR) ^ (1 / LDR_B)
```

with `R_fixed = 1 kΩ`, `LDR_A = 50000`, `LDR_B = 0.7` as starting
constants. The constants are cross-calibrated against a BH1750 on the
same board (the advanced module is the reference) before each generic
module is shipped, and the calibrated values are written to
`/cfg/sensors.json` under a new `"light_cal"` object — out of scope for
this ADR; tracked separately.

## Consequences

**Easier:**
- Same firmware image runs both variants — only `/cfg/sensors.json`
  differs between them.
- ADL tracking gets a uniform `rmms/<uuid>/light` topic regardless of
  the underlying part, so the Radxa FHIR mapping (§9.6) and any future
  MagicMirror² tile don't need a per-variant branch.
- Adding a third light sensor later (e.g. VEML7700) means one new file
  + one new `light_select` case, no change to any consumer.

**Harder:**
- Two drivers + one vtable + one selector is more surface area than the
  original "one ADC pin" design. Worth it because it puts the variant
  switch in config instead of code.
- The GL5516 path can't be hardware-verified in this project (we don't
  build a generic module). Its driver is reviewed on logic but not on a
  bench; the calibration step before shipping a generic module would
  catch any decode bugs.

**Neutral:**
- I²C0 bus participants on the advanced module are unchanged: AHT21
  (0x38), SH1122 (0x3C), ENS160 (0x53), BME280 (0x76, only if that
  alternative env footprint is populated), BH1750 (0x23). No conflict.
- ADC0/GPIO26 is reserved for the GL5516 on the generic variant; on the
  advanced variant the pin is unused (the divider components are simply
  not populated). The pin map stays the same so the same board layout
  serves both.

## Implementation pointer

- `light_driver_t` interface: `components/sensor_light/light_driver.h`
- BH1750 driver: `components/sensor_light/bh1750.c`
- GL5516 driver: `components/sensor_light/gl5516.c`
- Driver selector: `components/sensor_light/light_select.c`
- Config schema extension: `components/cfg/cfg.{h,c}` — `CfgSensors.light`
- Pin map: `BOARD_BH1750_ADDR` (0x23) and `BOARD_LDR_ADC_INPUT` (0)
  in `components/board/board_pico2wh.h`
- Bring-up integration: `test/bringup/sensors_publish.c`
  (`SENSOR_LIGHT_ON` switch, publishes `rmms/<uuid>/light` at 0.2 Hz)
