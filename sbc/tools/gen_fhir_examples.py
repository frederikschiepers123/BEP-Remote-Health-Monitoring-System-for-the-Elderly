"""Generate example FHIR R4 JSON from the real SBC pipeline (no hospital, no deps).

Runs a canned sample of each sensor through the same pure mapping + shaping the
service uses, and writes the resulting Observation resources + one transaction
Bundle to sbc/examples/fhir/. Drop those files into any FHIR R4 validator
(e.g. https://validator.fhir.org or the HAPI validator JAR) to confirm the
output is standard-conformant.

    PYTHONPATH=sbc/src python3 sbc/tools/gen_fhir_examples.py
"""
from __future__ import annotations

import json
import pathlib
from datetime import datetime, timezone

from rmms_aggregator.domain.quality import Quality
from rmms_aggregator.domain.sample import (
    AirSample, EnvSample, LightSample, RadarSample,
)
from rmms_aggregator.fhir.mapping import iter_observations
from rmms_aggregator.fhir.resource_json import (
    bundle_transaction_dict, conditional_url, observation_dict,
)

PATIENT = "example-patient-1"
NOW = datetime(2026, 6, 10, 8, 30, 0, tzinfo=timezone.utc)
WALL_MS = 1717920000000  # a synced wall clock → status can be 'final'

OUT = pathlib.Path(__file__).resolve().parents[1] / "examples" / "fhir"


def _samples():
    uuid = "550e8400-e29b-41d4-a716-446655440000"
    return [
        EnvSample(uuid, 1, WALL_MS, 100, Quality.OK,
                  temp_c=21.5, hum_pct=55.0, pres_hpa=1013.25),
        AirSample(uuid, 2, WALL_MS, 100, Quality.OK,
                  co2_ppm=600, tvoc_ppb=300, aqi=2),
        RadarSample(uuid, 3, WALL_MS, 100, Quality.OK,
                    presence=True, distance_mm=2400, breath_bpm=16.0, heart_bpm=72.0),
        LightSample(uuid, 4, WALL_MS, 100, Quality.OK, lux=123.5),
    ]


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    bundle_items: list[tuple[str, dict]] = []

    for sample in _samples():
        for planned in iter_observations(sample, NOW):
            obs = observation_dict(planned, PATIENT)
            (OUT / f"observation_{planned.code.obs_key}.json").write_text(
                json.dumps(obs, indent=2) + "\n")
            bundle_items.append((conditional_url(planned), obs))

    bundle = bundle_transaction_dict(bundle_items)
    (OUT / "bundle_transaction.json").write_text(json.dumps(bundle, indent=2) + "\n")

    print(f"wrote {len(bundle_items)} Observation files + 1 transaction Bundle to {OUT}")


if __name__ == "__main__":
    main()
