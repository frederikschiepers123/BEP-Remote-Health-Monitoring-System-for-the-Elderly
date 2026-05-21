# MQTT Topic and Payload Schema

**Authoritative reference for all topics published or subscribed to by the sensor-module firmware.**

All topics are rooted at `rmms/<uuid>/...` where `<uuid>` is the device UUID from the factory-provisioned identity (36-character UUIDv4 string).

Payloads are **CBOR** (RFC 8949) unless otherwise noted.  JSON is not used in this firmware.

---

## Topic table

| Topic | Direction | QoS | Retained | Notes |
|---|---|---|---|---|
| `rmms/<uuid>/env` | publish | 1 | no | BME280 environment sample |
| `rmms/<uuid>/radar` | publish | 1 | no | Radar sample (raw + quality flag) |
| `rmms/<uuid>/light` | publish | 1 | no | LDR light sample |
| `rmms/<uuid>/ir/meta` | publish | 1 | no | IR frame metadata |
| `rmms/<uuid>/ir/frame` | publish | 1 | no | Binary IR frame payload |
| `rmms/<uuid>/status` | publish | 1 | **yes** | `"online"` / `"offline"` (LWT) |
| `rmms/<uuid>/cmd` | subscribe | 1 | — | Commands from Radxa (see below) |
| `rmms/<uuid>/log` | publish | 0 | no | Critical log lines (text, best-effort) |

---

## Payload envelope (all sensor topics)

Every sensor sample payload (env, radar, light, ir/meta) uses the same outer wrapper:

```
{
  "ts_us":   uint64,    # monotonic microseconds since boot (always present)
  "wall_ms": uint64,    # wall-clock milliseconds (optional; 0 or absent if RTC not synced)
  "seq":     uint32,    # per-topic monotonic sequence counter (starts at 0, wraps at 2^32)
  "v":       <body>,    # sensor-specific inner object — see sections below
  "q":       uint8      # quality flag: 0=ok  1=stale  2=degraded  3=invalid
}
```

Quality flag semantics:

| `q` | Meaning | Action for consumers |
|---|---|---|
| 0 | OK — fresh, plausible reading | Use normally |
| 1 | Stale — measurement triggered but driver returned cached value | Use with caution |
| 2 | Degraded — ghost reading or sensor in reduced-quality state | Use with caution; apply additional filtering |
| 3 | Invalid — hardware error or driver not yet ready | Discard; do not display or forward to FHIR |

---

## `rmms/<uuid>/env` — environment sample

Inner `v` object:

```
{
  "temp_c":        float32,   # temperature in degrees Celsius
  "humidity_pct":  float32,   # relative humidity 0.0–100.0 %
  "pressure_hpa":  float32    # atmospheric pressure in hPa (= mbar)
}
```

Example (CBOR diagnostic notation):
```
{
  "ts_us":       1234567890,
  "wall_ms":     0,
  "seq":         42,
  "v": {
    "temp_c":      21.45,
    "humidity_pct": 58.3,
    "pressure_hpa": 1013.25
  },
  "q": 0
}
```

---

## `rmms/<uuid>/radar` — radar sample

Published by both the Seeed MR60BHA2 and DFRobot C1001 drivers via the common `radar_driver_t` interface.  The inner object is identical regardless of which radar is attached.

Inner `v` object:

```
{
  "presence":    bool,      # true if a person is detected
  "distance_mm": uint32,   # distance to closest target in mm; 0 = not reported by this radar
  "breath_rpm":  float32,  # breathing rate in breaths per minute; 0.0 = not reported
  "heart_bpm":   float32   # heart rate in beats per minute; 0.0 = not reported
}
```

Quality flag notes:
- `q=2` (degraded): presence is asserted but vital signs are implausibly low (ghost-detection heuristic).  Radxa should apply additional filtering before clinical use.
- `q=3` (invalid): UART error or driver not yet initialised.

Example:
```
{
  "ts_us":  9876543210,
  "wall_ms": 0,
  "seq":    100,
  "v": {
    "presence":    true,
    "distance_mm": 1200,
    "breath_rpm":  15.0,
    "heart_bpm":   72.0
  },
  "q": 0
}
```

---

## `rmms/<uuid>/light` — light sample

Inner `v` object:

```
{
  "lux": float32    # approximate illuminance in lux (GL5516 LDR, 10 kΩ divider)
}
```

Example:
```
{
  "ts_us":  111222333,
  "wall_ms": 0,
  "seq":    7,
  "v": {
    "lux": 150.4
  },
  "q": 0
}
```

---

## `rmms/<uuid>/ir/meta` — IR frame metadata

**Note:** IR camera part is not yet confirmed (CLAUDE.md §16 Q1).  The stub driver publishes `q=3` until the part is resolved.

Inner `v` object:

```
{
  "width":  uint16,   # frame width in pixels
  "height": uint16    # frame height in pixels
}
```

Example (when operational):
```
{
  "ts_us":  555666777,
  "wall_ms": 0,
  "seq":    3,
  "v": {
    "width":  32,
    "height": 24
  },
  "q": 0
}
```

---

## `rmms/<uuid>/ir/frame` — binary IR frame

Raw pixel data with no CBOR envelope.  Format depends on the IR camera part (TBD per CLAUDE.md §16 Q1).  QoS 1, not retained.

---

## `rmms/<uuid>/status` — device status (retained)

Plain UTF-8 string, not CBOR.  Two values only:

| Value | When published |
|---|---|
| `"online"` | Immediately after a successful MQTT CONNECT/CONNACK |
| `"offline"` | LWT — published by the broker if the connection is lost |

Retained so a subscriber that connects later immediately sees the current state.

---

## `rmms/<uuid>/cmd` — command (subscribed)

The firmware subscribes to this topic and processes the following closed set of commands.  All commands are CBOR maps with at least a `"cmd"` key.

| Command | CBOR payload | Action |
|---|---|---|
| `activate` | `{"cmd": "activate"}` | Start publishing sensor data |
| `deactivate` | `{"cmd": "deactivate"}` | Stop publishing, keep connection alive |
| `deregister` | `{"cmd": "deregister"}` | Clear all `/cfg/` littlefs config (not certs), factory reset to unconfigured state |
| `shutdown` | `{"cmd": "shutdown"}` | Drop links and idle; recover on power cycle |

Adding new commands requires updating the Radxa team's contract first (CLAUDE.md §9.3).

---

## `rmms/<uuid>/log` — log line

Plain UTF-8 text, not CBOR.  Best-effort (QoS 0), not retained.  One log line per publish.  Published only for critical errors (LOG_E level).

---

## Namespace rules

- The firmware **never publishes to `rmms/ui/...`**.  That namespace is owned by the Radxa Dragon Q6A (CLAUDE.md §9.5).
- The firmware **never publishes raw numeric vitals in a format intended for direct human display**.
- No spaces, no Dutch diacritics, no PII in topic strings.
