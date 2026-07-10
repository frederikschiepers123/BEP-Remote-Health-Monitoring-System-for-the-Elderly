# FHIR R4 mapping (sensor JSON → Observation)

> **Generated** from `src/rmms_aggregator/fhir/codes.py` by
> `tools/gen_fhir_mapping_doc.py`. Do not edit by hand — change the CODES
> table and regenerate. This is the human-readable view of the firmware
> repo's `CLAUDE.md §9.6` contract, implemented per this repo's §8.

Each measured value becomes one FHIR R4 `Observation` (§8.1 — radar's
heart/breath/presence/distance are separate Observations, not panel
members). Fields encoded as `null` / a negative sentinel by the firmware
are skipped. A `q=3` (INVALID) sample builds nothing and is dead-lettered.

## Code table

| Sensor field | Topic | obs_key | Code system | Code | Display | Value | Unit (UCUM) | Category | Class | Status |
|---|---|---|---|---|---|---|---|---|---|---|
| `heart_bpm` | `rmms/<uuid>/radar` | `heart` | LOINC | `8867-4` | Heart rate | valueQuantity | /min (`/min`) | vital-signs | medical | ✓ verified |
| `breath_bpm` | `rmms/<uuid>/radar` | `breath` | LOINC | `9279-1` | Respiratory rate | valueQuantity | /min (`/min`) | vital-signs | medical | ✓ verified |
| `presence` | `rmms/<uuid>/radar` | `presence` | placeholder (urn:rmms) | `presence` | Person presence detected | valueBoolean | — | activity | medical | ⚠ verify (clinical sign-off) |
| `temp_c` | `rmms/<uuid>/env` | `temp` | placeholder (urn:rmms) | `ambient_temp_c` | Ambient temperature | valueQuantity | Cel (`Cel`) | survey | ambient | ⚠ verify (clinical sign-off) |
| `hum_pct` | `rmms/<uuid>/env` | `humidity` | LOINC | `19736-7` | Relative humidity in environment | valueQuantity | % (`%`) | survey | ambient | ✓ verified |
| `pres_hpa` | `rmms/<uuid>/env` | `pressure` | placeholder (urn:rmms) | `atmospheric_pressure_hpa` | Atmospheric pressure | valueQuantity | hPa (`hPa`) | survey | ambient | ⚠ verify (clinical sign-off) |
| `co2_ppm` | `rmms/<uuid>/air` | `co2` | placeholder (urn:rmms) | `co2_ppm` | Indoor equivalent CO2 | valueQuantity (int) | ppm (`[ppm]`) | survey | ambient | ⚠ verify (clinical sign-off) |
| `tvoc_ppb` | `rmms/<uuid>/air` | `tvoc` | placeholder (urn:rmms) | `tvoc_ppb` | Total volatile organic compounds | valueQuantity (int) | ppb (`[ppb]`) | survey | ambient | ⚠ verify (clinical sign-off) |
| `aqi` | `rmms/<uuid>/air` | `aqi` | placeholder (urn:rmms) | `uba_aqi` | UBA air-quality index (1-5) | valueQuantity (int) | {index} (annotated) | survey | ambient | ⚠ verify (clinical sign-off) |
| `lux` | `rmms/<uuid>/light` | `lux` | placeholder (urn:rmms) | `ambient_lux` | Ambient illuminance | valueQuantity | lx (`lx`) | survey | ambient | ⚠ verify (clinical sign-off) |
| `distance_mm` | `rmms/<uuid>/radar` | `distance` | placeholder (urn:rmms) | `subject_distance` | Device-to-subject distance | valueQuantity | mm (`mm`) | survey | device | ⚠ verify (clinical sign-off) |

**Verified vs placeholder.** Only a clinically-confirmed code uses the real
LOINC/SNOMED system. Everything still awaiting sign-off uses the explicit
`urn:rmms:obs-code` system (never an invented or real-but-wrong LOINC, §8.2).
UCUM (`http://unitsofmeasure.org`) codes the *unit* independently of the code system, so a placeholder
Observation can still carry a coded unit.

## Quality → Observation.status (§8.4)

| `q` | Quality | `Observation.status` |
|---|---|---|
| 0 | OK | `final` |
| 1 | STALE | `preliminary` |
| 2 | DEGRADED | `preliminary` |
| 3 | INVALID | **dead-letter (not built)** |

If `wall_ms` is the firmware's `-1` sentinel (no RTC sync), the status is
forced to `preliminary` regardless of quality — the timestamp itself is a
best-guess (`now − ESTIMATED_TRANSPORT_LATENCY`, §8.5).

## Idempotency identifier (§8.6)

Every Observation carries a stable business identifier:

```
system = urn:rmms:seq
value  = <device_uuid>-<obs_key>-<seq>
```

Bundles POST as `transaction` with each entry a conditional update
(`PUT Observation?identifier=<system>|<value>`), so a re-sent / retried /
restart-replayed sample updates-or-is-ignored by the server instead of
creating a duplicate. `seq` is stable across firmware spool re-sends and
monotonic-with-gaps across reboots (firmware ADR-0003).
