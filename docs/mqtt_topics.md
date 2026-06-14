# MQTT Topic and Payload Schema

**Reference for the topics published or subscribed to by the sensor-module
firmware.** The authoritative contract is root `CLAUDE.md §9.1` (topics) and
`§9.2` (JSON payloads); `§9.6` covers the firmware→SBC FHIR mapping. This file
mirrors those sections for quick reference — if it ever drifts, the root file
wins.

All topics are rooted at `rmms/<uuid>/...` where `<uuid>` is the factory-provisioned
device UUID (36-character UUIDv4 string). **Payloads are JSON** (RFC 8259, UTF-8,
compact). CBOR was considered and rejected (root `CLAUDE.md §9.2`). All uplink
sensor topics are delivered at-least-once with QoS-1 retry from the device's
non-volatile flash spool ([ADR-0003](adr/0003-nv-flash-spool-and-time-sync.md)),
so a message may be delayed or re-sent after an outage — consumers dedup on `seq`.

---

## Topic table

| Topic | Direction | QoS | Retained | Notes |
|---|---|---|---|---|
| `rmms/<uuid>/env` | publish | 1 | no | Environment sample (BME280 or AHT21) |
| `rmms/<uuid>/air` | publish | 1 | no | Air-quality sample (ENS160) |
| `rmms/<uuid>/radar` | publish | 1 | no | Radar sample (raw + quality flag) |
| `rmms/<uuid>/light` | publish | 1 | no | Light sample (BH1750 or GL5516) |
| `rmms/<uuid>/status` | publish | 1 | **yes** | `"online"` / `"offline"` (LWT) |
| `rmms/<uuid>/cmd` | subscribe | 1 | — | Commands (see below) |
| `rmms/<uuid>/time/set` | subscribe | 1 | — | Wall-clock sync `{"epoch_ms":…}` (§9.2.5; ADR-0003) |
| `rmms/<uuid>/log` | publish | 0 | no | Critical log lines (text, best-effort) |

**Downlink topics** in the per-device tree that the firmware **never** publishes
or subscribes to (operator → mirror; documented here for completeness, root
`CLAUDE.md §9.1`):

| Topic | Publisher | Subscriber | Payload |
|---|---|---|---|
| `rmms/<uuid>/info` | operator cert (PoC laptop; prod Radxa relay) | mirror | `{"text":"...","wall_ms":…}` |
| `rmms/<uuid>/screen` | operator cert | mirror | `{"page":1..4,"wall_ms":…}` |

> There is **no IR camera** (dropped, root `CLAUDE.md §17`) and **no
> `rmms/ui/...` namespace** (retired — the mirror reads the raw topics directly,
> root `CLAUDE.md §9.5`). If you find either referenced anywhere, it's stale.

---

## Payload envelope (all sensor topics)

Every sensor sample uses the same outer wrapper (root `CLAUDE.md §9.2.1`):

```json
{
  "ts_us":   1234567890,
  "wall_ms": 1716210000000,
  "seq":     42,
  "q":       0,
  "v":       { }
}
```

| Field | Type | Meaning |
|---|---|---|
| `ts_us` | uint64 | Monotonic microseconds since boot. Always present, always valid. |
| `wall_ms` | int64 | Wall-clock ms since the Unix epoch. **`-1` sentinel** until the first `time/set`. **Never omitted** — use the sentinel, do not drop the field. |
| `seq` | uint32 | Per-topic monotonic counter, persisted across reboots (monotonic-with-gaps). Used by the SBC for dedup. |
| `q` | uint8 | Quality flag (below). |
| `v` | object | Sensor-specific body — see per-sensor sections. |

Quality flag (`q`):

| `q` | Meaning | Consumer action |
|---|---|---|
| 0 | OK — fresh, plausible | Use normally |
| 1 | Stale — cached value returned | Use with caution |
| 2 | Degraded / validating — ghost reading, warm-up, or carried on older window data | Use with caution; FHIR `preliminary` |
| 3 | Invalid — hardware error or driver not ready | Discard; do not display or forward to FHIR |

The firmware never silently drops a sample; it sets `q` instead (root
`CLAUDE.md §9.2.1`, audit anti-pattern §19.1).

---

## `rmms/<uuid>/env` — environment sample

Inner `v` object:

```json
{"temp_c": 21.500, "hum_pct": 55.000, "pres_hpa": 1013.250}
```

- `temp_c` — temperature in °C (`%.3f`).
- `hum_pct` — relative humidity 0–100 % (`%.3f`).
- `pres_hpa` — atmospheric pressure in hPa, **or `null`** when the AHT21 driver
  is active (it has no pressure sensor). BME280 always emits a number. Receivers
  must not assume `pres_hpa` is numeric (root `CLAUDE.md §9.2.2`/§9.2.3).

---

## `rmms/<uuid>/air` — air-quality sample (ENS160)

Inner `v` object:

```json
{"co2_ppm": 600, "tvoc_ppb": 300, "aqi": 2}
```

- `co2_ppm` — equivalent CO₂ estimate, integer ppm.
- `tvoc_ppb` — total VOC estimate, integer ppb.
- `aqi` — UBA air-quality index, integer 1–5 (1 excellent … 5 unhealthy).

The ENS160 needs an undocumented warm-up (~5–10 min) after power-up; samples
during that window carry `q=2`. The env driver's last temp/hum is written to the
ENS160 TEMP_IN/RH_IN compensation registers every cycle.

---

## `rmms/<uuid>/radar` — radar sample

Produced by either radar driver — the Seeed MR60BHA2 (60 GHz, `"radar":"bha2"`)
or the Seeed 24 GHz HMMD module (`"radar":"hmmd"`, ADR-0007) — via the common
`radar_driver_t` interface, so the inner object is radar-independent (root
`CLAUDE.md §3.2`/§7.4). Fields a given radar does not report are sent as the
documented sentinel / `null` (the HMMD module has no breath-phase stream, so its
`resp_motion` is always `null`, and it often reports no `heart_bpm`).

Inner `v` object:

```json
{"presence": true, "distance_mm": 2400, "breath_bpm": 16.5, "heart_bpm": 72.0, "resp_motion": true}
```

- `presence` — bool, always present.
- `distance_mm` — distance to closest target in mm; `null`/`0` if not reported.
- `breath_bpm` — breathing rate; `null` if not reported this cycle **or
  suppressed during a breath-hold**.
- `heart_bpm` — heart rate; `null` if not reported this cycle.
- `resp_motion` — ADR-0006 phase-based breath-hold detection: `true` = chest
  motion present, `false` = no respiratory motion (possible hold; `breath_bpm`
  is nulled for that sample), `null` = undetermined (no presence/distance lock or
  no valid breath-phase amplitude). Always present. A suspected indicator, not a
  clinical apnea alarm; FHIR mapping TBD (root `CLAUDE.md §9.6`).

Quality notes: `q=2` when presence is asserted but vitals are implausible
(ghost-detection) or values are carried on older window data; `q=3` on UART
error / driver not ready. (The plausibility/median filtering is
[ADR-0005](adr/0005-mcu-side-radar-filtering.md).)

---

## `rmms/<uuid>/light` — light sample

Inner `v` object:

```json
{"lux": 120.5}
```

Same payload for both light variants (root `CLAUDE.md §3.2`, ADR-0001): BH1750
(I²C, calibrated lux directly — the default) or GL5516 LDR (ADC0 + 1 kΩ divider,
per-board power-law calibration).

---

## `rmms/<uuid>/status` — device status (retained)

Plain UTF-8 string (not a JSON object). Two values only:

| Value | When |
|---|---|
| `"online"` | published retained on each MQTT CONNACK |
| `"offline"` | the broker publishes this LWT if the connection drops |

Retained, so a late subscriber immediately sees the current state.

---

## `rmms/<uuid>/cmd` — command (subscribed)

The firmware subscribes and processes this **closed** set. Payloads are JSON
objects with a `"cmd"` key (parsed by the jsmn tokenizer, root `CLAUDE.md §9.2`):

| Command | Payload | Action |
|---|---|---|
| `activate` | `{"cmd":"activate"}` | Start publishing sensor data |
| `deactivate` | `{"cmd":"deactivate"}` | Stop publishing, keep the connection |
| `deregister` | `{"cmd":"deregister"}` | Clear all `/cfg/` config (not certs); factory reset to unconfigured |
| `shutdown` | `{"cmd":"shutdown"}` | Drop links and idle; recover on power cycle |

Adding commands requires the Radxa team's sign-off first (root `CLAUDE.md §9.3`).
`deregister_sbc` from the previous group is removed and must not return.

---

## `rmms/<uuid>/time/set` — wall-clock sync (subscribed)

```json
{"epoch_ms": 1716210000000}
```

Published per-device by the broker LAN's time-sync publisher (the SBC aggregator
in the live stack) when a device connects. The firmware maintains a wall-clock
offset and stamps each spooled record's `wall_ms` (§9.2.5, ADR-0003). No
NTP-from-WAN.

---

## `rmms/<uuid>/log` — log line

Plain UTF-8 text (not a JSON object). Best-effort QoS 0, not retained. One line
per publish, only for critical errors (`LOG_E`).

---

## Namespace rules

- The firmware publishes **only** the topics listed as `publish` above
  (`env`, `air`, `radar`, `light`, `status`, `log`). It publishes raw numeric
  vitals — that *is* the contract; the mirror does its own thresholding in
  `MMM-SensorUI.js` (root `CLAUDE.md §9.5`).
- The firmware subscribes **only** to `rmms/<uuid>/cmd` and
  `rmms/<uuid>/time/set`.
- The firmware **never** publishes or subscribes to `info` / `screen` (operator →
  mirror downlink) and there is **no `rmms/ui/...` namespace**.
- No spaces, no Dutch diacritics, no PII in topic strings.
