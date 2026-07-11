# MMM-CustomMQTTBridge

Project-internal MagicMirror² module for the TU Delft RMMS (Remote Medical
Monitoring System) BAP project. It is the **adapter between the MQTT wire
contract and the mirror's in-process notification bus**:

- **Wire side** — connects over **mTLS** to the tablet's Mosquitto broker
  (`:8883`) and subscribes to the sensor firmware's raw
  `rmms/<uuid>/<kind>` topics (JSON envelope per root `CLAUDE.md` §9.2).
- **Mirror side** — parses each payload in the `node_helper` and re-broadcasts
  every logical sensor field as an `MQTT_SENSOR_UPDATE` MM² notification with
  payload `{topic: "sensors/<field>", message: "<string>"}`, which
  [`MMM-SensorUI`](../MMM-SensorUI/README.md) consumes.

These are **two deliberately distinct namespaces** (root `CLAUDE.md` §9.5.1):
`rmms/...` is the broker contract shared by the firmware, the Radxa/SBC
aggregator, and the mirror; `sensors/...` exists only inside the MM² process,
between this bridge and MMM-SensorUI. The firmware never emits `sensors/...`;
the tile code never sees `rmms/...` or wire JSON.

> **Repo note:** the surrounding `MagicMirror/` tree is a vendored upstream
> MagicMirror² checkout. Only `modules/MMM-CustomMQTTBridge/`,
> `modules/MMM-SensorUI/`, and the tablet-side `config/` + `css/`
> customisations are project work.

## Single source of truth

This module — specifically the mapping table and dispatch code at the top of
[`node_helper.js`](node_helper.js) — is the **canonical wire→mirror
translation**, referenced by root `CLAUDE.md` §9.5.1. Envelope parsing,
stringification, and null/warm-up handling all live here and nowhere else.
**Adding a new sensor field is a change in exactly one place:** one
`send("name", value)` call in the right `if (kind === ...)` branch of
`node_helper.js`. MMM-SensorUI then only needs a handler for the new
`sensors/<name>` notification; it never touches JSON.

## Configuration

Set in `MagicMirror/config/config.js` (gitignored — the checked-in
`config.js.sample` is the plain upstream sample, not an RMMS example).
Defaults from `MMM-CustomMQTTBridge.js`:

| Key | Default | Meaning |
|---|---|---|
| `broker` | `"mqtts://<tablet-ip>:8883"` | Broker URL. `mqtts://` + all three cert files present ⇒ mTLS; otherwise the bridge falls back to plain MQTT (local mock/test path only). |
| `clientId` | `"mirror-de4ed19a"` | MQTT client ID. **Must match the CN of the mirror certificate** (`mirror-<short-id>`) issued by `scripts/provision_ca.sh` — the broker uses `use_identity_as_username` and its ACL grants the mirror role `topic read rmms/+/+`. |
| `caFile` | `""` | Absolute path to the project CA cert (PEM, from `out/mirror-<id>/`). |
| `certFile` | `""` | Absolute path to the mirror client cert (PEM). |
| `keyFile` | `""` | Absolute path to the mirror private key (PEM). |
| `topics` | `["rmms/+/+"]` | Subscriptions (wildcard covers all devices). |

With TLS active, `rejectUnauthorized: true` is set — server cert validation is
never disabled. The helper keeps **one** MQTT client per process: because the
mirror runs `serveronly`, the browser module re-sends `CONNECT_MQTT` on every
socket reconnect, and without the guard duplicate clients sharing the mirror
`clientId` would kick each other off the broker in a loop.

## Topic → notification mapping

Copied from the canonical table in `node_helper.js`:

| Broker topic + field | Mirror notification | Value (string) |
|---|---|---|
| `rmms/<uuid>/env` · `v.temp_c` | `sensors/temperature` | float, `.toFixed(1)` |
| `rmms/<uuid>/env` · `v.hum_pct` | `sensors/humidity` | int, rounded |
| `rmms/<uuid>/air` · `v.aqi` (1–5) | `sensors/airquality` | UBA label (`Excellent` / `Good` / `Moderate` / `Poor` / `Unhealthy`); `aqi == 0` ⇒ `"WARMUP"` |
| `rmms/<uuid>/air` · `v.co2_ppm` | `sensors/co2` | int |
| `rmms/<uuid>/air` · `v.tvoc_ppb` | `sensors/tvoc` | int |
| `rmms/<uuid>/radar` · `v.heart_bpm` | `sensors/heartrate` | int, rounded; `""` when null |
| `rmms/<uuid>/radar` · `v.breath_bpm` | `sensors/respiratoryrate` | int, rounded; `""` when null |
| `rmms/<uuid>/radar` · `v.resp_motion` | `sensors/respiratorymotion` | `"true"` / `"false"` / `""` (tri-state breath-hold signal, firmware ADR-0006) |
| `rmms/<uuid>/info` · `v.text` | `sensors/infomessage` | text |
| `rmms/<uuid>/status` (raw string, retained) | `sensors/status` | `"online"` / `"offline"` (forwarded for introspection; MMM-SensorUI does not currently display it) |
| `rmms/<uuid>/light` | — | intentionally ignored in v1 |
| `rmms/<uuid>/log` | — | intentionally ignored (not sensor data) |
| `rmms/<uuid>/screen` | — | not mapped in the current code (the operator page-select topic from `CLAUDE.md` §9.5 is not wired through this bridge yet) |

Topics that don't match the `rmms/<uuid>/<kind>` shape are relayed **verbatim**
(topic + raw string), so legacy `sensors/...` test publishers keep working
against local mock setups.

## Null / quality / warm-up handling

What the code actually does:

- **Radar nulls clear the tile.** Firmware vitals are presence-gated: a `null`
  `heart_bpm` / `breath_bpm` means "no presence-confirmed reading". The bridge
  forwards `null` as an **empty string**, which MMM-SensorUI treats as
  "no value" ("Measuring..."), instead of freezing the last number after the
  person walks away. `resp_motion` `null`/absent likewise becomes `""`
  (undetermined — clears any hold state; old firmware without the field is
  therefore harmless).
- **ENS160 warm-up.** Only `aqi == 0` (no usable reading yet) is shown as
  `"WARMUP"`. Readings with `aqi >= 1` are forwarded even while the chip still
  flags `INITIAL_STARTUP` (envelope `q=2`, up to ~1 h) — those values are
  already meaningful.
- **Envelope `q` flag.** Beyond the AQI case above, the current code does
  **not** inspect the envelope's `q` quality field; the firmware itself nulls
  degraded vitals, and that nulling is what the bridge forwards. If `q`-based
  gating is ever needed on the mirror, it belongs **here**, not in
  MMM-SensorUI (per §9.5.1). Note from field experience: gating presence
  behaviour on `distance != null` is more robust than gating on `q`.
- **Non-JSON payloads** on an `rmms/...` topic are logged and dropped.
- The parser tolerates both the `{"ts_us":..., "v":{...}}` envelope and a flat
  object (`env.v || env`).

## Files

- `MMM-CustomMQTTBridge.js` — browser-side module: sends `CONNECT_MQTT` with
  the config, relays `MQTT_SENSOR_UPDATE` onto the MM² notification bus, and
  shows the last message / bridge status as a small debug DOM (header
  "MQTT bridge").
- `node_helper.js` — Node-side: MQTT (m)TLS client, subscription, JSON
  parsing, the canonical mapping table.
- Depends on the `mqtt` npm package (see `package.json`).
