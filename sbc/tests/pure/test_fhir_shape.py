"""Structural FHIR R4 conformance of the shaped Observation / Bundle JSON.

These assert the R4 rules we can check with stdlib only (no fhir.resources needed
here): the construction-time Pydantic validation in builder.py is the second,
stricter layer that runs on the Radxa / CI. Together they answer the project's
focus question — "is the data in correct FHIR R4 standard format?".

Run:  PYTHONPATH=sbc/src python3 -m unittest discover -s sbc/tests/pure -t sbc/tests/pure
"""
import unittest
from datetime import datetime, timezone

from rmms_aggregator.domain.quality import Quality
from rmms_aggregator.domain.sample import (
    AirSample, EnvSample, LightSample, RadarSample,
)
from rmms_aggregator.fhir.mapping import iter_observations
from rmms_aggregator.fhir.resource_json import (
    bundle_transaction_dict, conditional_url, observation_dict,
)

NOW = datetime(2026, 6, 10, 8, 30, 0, tzinfo=timezone.utc)
WALL = 1717920000000
PATIENT = "p-1"
UUID = "dev-uuid-1"

# FHIR R4 Observation.status value set.
R4_STATUS = {"registered", "preliminary", "final", "amended", "corrected",
             "cancelled", "entered-in-error", "unknown"}


def _all_observations():
    samples = [
        EnvSample(UUID, 1, WALL, 1, Quality.OK, 21.5, 55.0, 1013.25),
        AirSample(UUID, 2, WALL, 1, Quality.OK, 600, 300, 2),
        RadarSample(UUID, 3, WALL, 1, Quality.OK, True, 2400, 16.0, 72.0),
        LightSample(UUID, 4, WALL, 1, Quality.OK, 123.5),
    ]
    out = []
    for s in samples:
        for p in iter_observations(s, NOW):
            out.append((p, observation_dict(p, PATIENT)))
    return out


class TestObservationShape(unittest.TestCase):
    def test_every_observation_is_well_formed_r4(self):
        for planned, obs in _all_observations():
            key = planned.code.obs_key
            self.assertEqual(obs["resourceType"], "Observation", key)
            self.assertIn(obs["status"], R4_STATUS, key)

            # identifier (idempotency): system + non-empty value
            ident = obs["identifier"][0]
            self.assertTrue(ident["system"] and ident["value"], key)

            # code: required, with a coding that has system + code
            coding = obs["code"]["coding"][0]
            self.assertTrue(coding["system"] and coding["code"], key)

            # category coding present
            cat = obs["category"][0]["coding"][0]
            self.assertTrue(cat["system"] and cat["code"], key)

            # subject + device references in canonical form
            self.assertTrue(obs["subject"]["reference"].startswith("Patient/"), key)
            self.assertTrue(obs["device"]["reference"].startswith("Device/"), key)

            # exactly ONE value[x] (FHIR choice element)
            value_keys = [k for k in obs if k.startswith("value")]
            self.assertEqual(len(value_keys), 1, f"{key}: value[x] = {value_keys}")

            # effectiveDateTime must be a parseable instant ending in Z
            dt = obs["effectiveDateTime"]
            self.assertTrue(dt.endswith("Z"), key)
            datetime.fromisoformat(dt.replace("Z", "+00:00"))   # raises if malformed

            # generated narrative (dom-6 best practice → clean validator run)
            self.assertEqual(obs["text"]["status"], "generated", key)
            self.assertIn('xmlns="http://www.w3.org/1999/xhtml"', obs["text"]["div"], key)

    def test_valuequantity_has_value_and_unit(self):
        for planned, obs in _all_observations():
            if "valueQuantity" not in obs:
                continue
            q = obs["valueQuantity"]
            self.assertIn("value", q)
            self.assertTrue(q["unit"])
            # if a UCUM system is claimed, a UCUM code must accompany it
            if "system" in q:
                self.assertTrue(q["code"])

    def test_presence_uses_valueboolean(self):
        by_key = {p.code.obs_key: obs for p, obs in _all_observations()}
        self.assertIn("valueBoolean", by_key["presence"])
        self.assertIs(by_key["presence"]["valueBoolean"], True)
        self.assertNotIn("valueQuantity", by_key["presence"])

    def test_pressure_is_raw_hpa(self):
        by_key = {p.code.obs_key: obs for p, obs in _all_observations()}
        self.assertAlmostEqual(by_key["pressure"]["valueQuantity"]["value"], 1013.25)


class TestBundleShape(unittest.TestCase):
    def test_transaction_bundle_conditional_update(self):
        items = [(conditional_url(p), obs) for p, obs in _all_observations()]
        bundle = bundle_transaction_dict(items)
        self.assertEqual(bundle["resourceType"], "Bundle")
        self.assertEqual(bundle["type"], "transaction")
        self.assertEqual(len(bundle["entry"]), len(items))
        for entry in bundle["entry"]:
            self.assertEqual(entry["resource"]["resourceType"], "Observation")
            # FHIR R4 requires fullUrl on non-POST transaction entries
            self.assertTrue(entry["fullUrl"].startswith("urn:uuid:"))
            req = entry["request"]
            self.assertEqual(req["method"], "PUT")                 # conditional update
            self.assertTrue(req["url"].startswith("Observation?identifier="))

    def test_fullurl_is_deterministic(self):
        # A re-sent Bundle must produce the SAME fullUrl per Observation (stable
        # identity → idempotent).
        items = [(conditional_url(p), obs) for p, obs in _all_observations()]
        a = bundle_transaction_dict(items)
        b = bundle_transaction_dict(items)
        self.assertEqual([e["fullUrl"] for e in a["entry"]],
                         [e["fullUrl"] for e in b["entry"]])


if __name__ == "__main__":
    unittest.main(verbosity=2)
