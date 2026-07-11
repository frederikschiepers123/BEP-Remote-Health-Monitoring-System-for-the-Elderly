# MMM-SensorUI

Project-internal MagicMirror² module for the TU Delft RMMS (Remote Medical
Monitoring System) BAP project. It renders the smart-mirror UI behind the
two-way acrylic: **per-sensor tiles with numeric values and a traffic-light
severity indicator** (`green` / `yellow` / `red`), plus an operator info panel
and a "last updated" footer.

> **Repo note:** the surrounding `MagicMirror/` tree is a vendored upstream
> MagicMirror² checkout. Only `modules/MMM-SensorUI/`,
> `modules/MMM-CustomMQTTBridge/`, and the tablet-side `config/` + `css/`
> customisations are project work.

## Data source — `sensors/*` notifications only

This module consumes **only** `MQTT_SENSOR_UPDATE` MM² notifications with
payload `{topic: "sensors/<field>", message: "<string>"}`, produced by
[`MMM-CustomMQTTBridge`](../MMM-CustomMQTTBridge/README.md). It **never**
connects to MQTT, never parses wire JSON, and knows nothing about the
`rmms/<uuid>/...` topic tree, the §9.2 envelope, or the `q`/`wall_ms`/`seq`
fields — that translation lives entirely in the bridge (root `CLAUDE.md`
§9.5.1). All values arrive as pre-stringified messages; an **empty string
means "no value"** and clears the tile back to "Measuring...".

Handled notifications: `sensors/heartrate`, `sensors/respiratoryrate`,
`sensors/respiratorymotion`, `sensors/temperature`, `sensors/humidity`,
`sensors/airquality`, `sensors/infomessage`. (`sensors/co2` and
`sensors/tvoc` only refresh the internal environment timestamp; they have no
tile. `sensors/status` is forwarded by the bridge but not handled here.)

## Tiles

| Tile | Notification | Display |
|---|---|---|
| Heart rate | `sensors/heartrate` | number + "BPM" + severity icon |
| Breath rate | `sensors/respiratoryrate` + `sensors/respiratorymotion` | number + "breaths per min" + severity icon. When `respiratorymotion == "false"` (confirmed breath-hold, firmware ADR-0006), the tile shows **"No breathing"** in red instead — the hold overrides the (firmware-suppressed) rate. |
| Temperature | `sensors/temperature` | number + "°C" + severity icon |
| Humidity | `sensors/humidity` | number + "%" + severity icon |
| Air quality | `sensors/airquality` | text label only (UBA label from the bridge, e.g. "Good", or "WARMUP") + severity icon — no number |
| Info panel | `sensors/infomessage` | operator text message (from `rmms/<uuid>/info` via the bridge), shown above the footer timestamp |
| Footer | — | "Vitals last updated: HH:MM:SS" — timestamp of the last non-empty vitals message, persisted in `localStorage` so a kiosk page reload restores it. An environment timestamp is tracked internally but not rendered. |

Vitals tiles are independent: an absent heart or breath value shows a
per-tile "Measuring..." card without blanking the other; only when both are
absent (and no breath-hold is active) do they collapse into one combined
"Measuring..." panel. The environment row does the same when any of
temperature / humidity / air quality is missing.

**Not implemented in this module version:** the `sensors/status`
online/offline footer and the `screen.v.page` layout switching (pages 1–4)
described in root `CLAUDE.md` §9.5. The bridge forwards `sensors/status`, but
this module ignores it, and the `rmms/<uuid>/screen` topic is not mapped by
the bridge at all.

## Severity thresholds (as coded)

The traffic-light logic lives in `THRESHOLDS` / `AIR_QUALITY_STATUS` at the
top of `MMM-SensorUI.js`. A value inside the `green` range is green; else
inside the `yellow` range is yellow; else **red**. Empty or non-numeric values
get no severity (never render red).

| Measure | Green | Yellow | Red |
|---|---|---|---|
| Heart rate (BPM) | 60–100 | 50–110 | otherwise |
| Respiratory rate (breaths/min) | 12–20 | 10–24 | otherwise |
| Temperature (°C) | 20–24 | 18–27 | otherwise |
| Humidity (%) | 40–60 | 30–70 | otherwise |

Air quality maps the UBA label directly:
`Excellent`/`Good` → green, `Moderate` → yellow, `Poor`/`Unhealthy` → red,
anything unrecognised (including `"WARMUP"`) → red.

> **Handover warning:** root `CLAUDE.md` §9.5 requires each threshold range to
> carry a comment citing its source (WHO, AHA, ASHRAE, ...) and clinical
> advisor sign-off before any deployment beyond the project review. **The
> ranges above currently have no citations in the code.** Do not treat them
> as clinically validated; obtain sign-off (and add the citations) before real
> deployment. Note also that §9.5 specifies four severity levels including
> `orange`; the code implements three (`green`/`yellow`/`red`).

## Configuration

`defaults` is empty (`{}`) — there are **no config.js keys**. Behaviour is
tuned via constants at the top of `MMM-SensorUI.js`:

- `DISPLAY_MODE` — `"icons"` (default: severity shown as a check /
  exclamation / warning-triangle icon under the value) or `"colors"`
  (severity colours the value text instead).
- `THRESHOLDS`, `AIR_QUALITY_STATUS` — the tables above.

Styling is in `MMM-SensorUI.css` (viewport-relative sizing, tile wrappers,
severity colours `#4CAF50` / `#FFC107` / `#F44336`, "Measuring..." layouts).
Requires Font Awesome (loaded via MagicMirror's bundled `font-awesome.css`).

## Adding a new field

Per §9.5.1: add the wire→string translation as one `send(...)` call in the
bridge's `node_helper.js`, then add a `sensors/<name>` handler and tile here.
Derived UI values are computed **here** (mirror-side JS), never in the
firmware; new raw topics require a new sensor on the board.
