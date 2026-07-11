"""Generate docs/fhir-mapping.md (CLAUDE.md §4) from the live CODES table, so the
human-readable mapping can never drift from fhir/codes.py — the single source of
truth (§8.2). Re-run after any change to the table.

    PYTHONPATH=sbc/src python3 sbc/tools/gen_fhir_mapping_doc.py
"""
from __future__ import annotations

import pathlib

from rmms_aggregator.domain.quality import Quality, _QUALITY_STATUS
from rmms_aggregator.domain.sample import (
    AirSample, EnvSample, LightSample, RadarSample, sensor_of,
)
from rmms_aggregator.fhir.codes import CODES, LOINC, RMMS_PLACEHOLDER, SNOMED, UCUM
from rmms_aggregator.fhir.identifiers import IDENTIFIER_SYSTEM
from rmms_aggregator.fhir.mapping import _fields

OUT = pathlib.Path(__file__).resolve().parents[1] / "docs" / "fhir-mapping.md"

_SYSTEM_LABEL = {LOINC: "LOINC", SNOMED: "SNOMED CT", RMMS_PLACEHOLDER: "placeholder (urn:rmms)"}
_VALUE_LABEL = {"quantity": "valueQuantity", "integer": "valueQuantity (int)", "boolean": "valueBoolean"}


def _field_to_topic() -> dict[str, str]:
    """field name → sensor topic, derived from the domain samples (no duplication)."""
    dummies = [
        EnvSample("u", 0, None, 0, Quality.OK, 0.0, 0.0, 0.0),
        AirSample("u", 0, None, 0, Quality.OK, 0, 0, 0),
        RadarSample("u", 0, None, 0, Quality.OK, False, 0, 0.0, 0.0),
        LightSample("u", 0, None, 0, Quality.OK, 0.0),
    ]
    out: dict[str, str] = {}
    for sample in dummies:
        for field, _raw in _fields(sample):
            out[field] = sensor_of(sample)
    return out


def main() -> None:
    topic_of = _field_to_topic()
    lines: list[str] = []
    w = lines.append

    w("# FHIR R4 mapping (sensor JSON → Observation)")
    w("")
    w("> **Generated** from `src/rmms_aggregator/fhir/codes.py` by")
    w("> `tools/gen_fhir_mapping_doc.py`. Do not edit by hand — change the CODES")
    w("> table and regenerate. This is the human-readable view of the firmware")
    w("> repo's `CLAUDE.md §9.6` contract, implemented per this repo's §8.")
    w("")
    w("Each measured value becomes one FHIR R4 `Observation` (§8.1 — radar's")
    w("heart/breath/presence/distance are separate Observations, not panel")
    w("members). Fields encoded as `null` / a negative sentinel by the firmware")
    w("are skipped. A `q=3` (INVALID) sample builds nothing and is dead-lettered.")
    w("")
    w("## Code table")
    w("")
    w("| Sensor field | Topic | obs_key | Code system | Code | Display | Value | Unit (UCUM) | Category | Class | Status |")
    w("|---|---|---|---|---|---|---|---|---|---|---|")
    for field, c in CODES.items():
        unit = "—" if c.unit is None else (f"{c.unit} (`{c.ucum_code}`)" if c.ucum_code else f"{c.unit} (annotated)")
        scale = "" if c.scale == 1.0 else f" ×{c.scale}"
        status = "✓ verified" if c.confirmed else "⚠ verify (clinical sign-off)"
        w(f"| `{field}` | `rmms/<uuid>/{topic_of.get(field, '?')}` | `{c.obs_key}` "
          f"| {_SYSTEM_LABEL.get(c.code_system, c.code_system)} | `{c.code}` | {c.display} "
          f"| {_VALUE_LABEL.get(c.value_kind, c.value_kind)}{scale} | {unit} | {c.category} "
          f"| {c.klass} | {status} |")
    w("")
    w("**Verified vs placeholder.** Only a clinically-confirmed code uses the real")
    w("LOINC/SNOMED system. Everything still awaiting sign-off uses the explicit")
    w(f"`{RMMS_PLACEHOLDER}` system (never an invented or real-but-wrong LOINC, §8.2).")
    w("UCUM (`%s`) codes the *unit* independently of the code system, so a placeholder" % UCUM)
    w("Observation can still carry a coded unit.")
    w("")
    w("## Quality → Observation.status (§8.4)")
    w("")
    w("| `q` | Quality | `Observation.status` |")
    w("|---|---|---|")
    for q in Quality:
        status = _QUALITY_STATUS.get(q)
        rendered = f"`{status}`" if status else "**dead-letter (not built)**"
        w(f"| {int(q)} | {q.name} | {rendered} |")
    w("")
    w("If `wall_ms` is the firmware's `-1` sentinel (no RTC sync), the status is")
    w("forced to `preliminary` regardless of quality — the timestamp itself is a")
    w("best-guess (`now − ESTIMATED_TRANSPORT_LATENCY`, §8.5).")
    w("")
    w("## Idempotency identifier (§8.6)")
    w("")
    w("Every Observation carries a stable business identifier:")
    w("")
    w("```")
    w(f"system = {IDENTIFIER_SYSTEM}")
    w("value  = <device_uuid>-<obs_key>-<seq>")
    w("```")
    w("")
    w("Bundles POST as `transaction` with each entry a conditional update")
    w("(`PUT Observation?identifier=<system>|<value>`), so a re-sent / retried /")
    w("restart-replayed sample updates-or-is-ignored by the server instead of")
    w("creating a duplicate. `seq` is stable across firmware spool re-sends and")
    w("monotonic-with-gaps across reboots (firmware ADR-0003).")
    w("")

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("\n".join(lines))
    print(f"wrote {OUT} ({len(CODES)} fields)")


if __name__ == "__main__":
    main()
