"""FHIR builder: resources validate as FHIR R4, and the Bundle uses conditional
update entries (idempotency)."""
import json
from datetime import datetime, timezone

from rmms_aggregator.domain.quality import Quality
from rmms_aggregator.domain.sample import RadarSample
from rmms_aggregator.fhir.builder import build_and_validate, build_post_bundle
from rmms_aggregator.fhir.mapping import iter_observations

NOW = datetime(2024, 5, 20, 12, 0, 0, tzinfo=timezone.utc)


def _radar_planned():
    s = RadarSample("dev-1", 1, 1700000000000, 5, Quality.OK,
                    presence=True, distance_mm=2400, breath_bpm=16.0, heart_bpm=72.0)
    return iter_observations(s, NOW)


def test_observation_validates_and_has_identifier():
    planned = {p.code.obs_key: p for p in _radar_planned()}
    ident, fhir_json = build_and_validate(planned["heart"], "patient-9")
    assert ident == "dev-1-heart-5"
    obs = json.loads(fhir_json)
    assert obs["resourceType"] == "Observation"
    assert obs["identifier"][0] == {"system": "urn:rmms:seq", "value": "dev-1-heart-5"}
    assert obs["code"]["coding"][0]["code"] == "8867-4"            # LOINC heart rate
    assert obs["valueQuantity"]["value"] == 72.0
    assert obs["subject"]["reference"] == "Patient/patient-9"
    assert obs["device"]["reference"] == "Device/dev-1"


def test_presence_is_value_boolean():
    planned = {p.code.obs_key: p for p in _radar_planned()}
    _ident, fhir_json = build_and_validate(planned["presence"], "p1")
    obs = json.loads(fhir_json)
    assert obs["valueBoolean"] is True
    assert "valueQuantity" not in obs


def test_bundle_is_conditional_update():
    rows = [(1, "dev-1-heart-5", json.dumps({
        "resourceType": "Observation", "status": "final",
        "code": {"coding": [{"system": "http://loinc.org", "code": "8867-4"}]},
    }))]
    bundle = build_post_bundle(rows)
    assert bundle["type"] == "transaction"
    entry = bundle["entry"][0]
    assert entry["request"]["method"] == "PUT"
    assert entry["request"]["url"] == "Observation?identifier=urn:rmms:seq|dev-1-heart-5"
