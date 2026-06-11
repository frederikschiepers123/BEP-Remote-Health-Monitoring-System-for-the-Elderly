"""Pure-logic unit tests for the SBC aggregator — stdlib `unittest`, no external
deps, so they run anywhere (including this sandbox where pip is unavailable).

Covered (the requirements' core): JSON decode, quality→status, dedup, the
LOINC/SNOMED code table, the idempotency identifier scheme, and the
sample→Observation planning (codes, units/scale, null-skip, status, time).

Run:  PYTHONPATH=sbc/src python3 sbc/tests/pure/test_pure_core.py
"""
import json
import unittest
from datetime import datetime, timezone

from rmms_aggregator.domain import dedup
from rmms_aggregator.domain.decode import SchemaError, decode_sample, parse_topic
from rmms_aggregator.domain.quality import Quality, is_buildable, quality_to_status
from rmms_aggregator.domain.sample import (
    AirSample, EnvSample, LightSample, RadarSample,
)
from rmms_aggregator.fhir import identifiers
from rmms_aggregator.fhir.codes import CODES, LOINC, RMMS_PLACEHOLDER, SNOMED
from rmms_aggregator.fhir.mapping import iter_observations, resolve_effective_datetime

NOW = datetime(2024, 5, 20, 12, 0, 0, tzinfo=timezone.utc)


def envelope(seq=1, q=0, wall_ms=-1, **v):
    return json.dumps({"ts_us": 1000, "wall_ms": wall_ms, "seq": seq, "q": q, "v": v}).encode()


class TestDecode(unittest.TestCase):
    def test_topic(self):
        self.assertEqual(parse_topic("rmms/dev-1/env"), ("dev-1", "env"))
        for bad in ("rmms/dev-1", "x/dev-1/env", "rmms/dev-1/ir", "rmms//env"):
            with self.assertRaises(SchemaError):
                parse_topic(bad)

    def test_env(self):
        s = decode_sample("u", "env", envelope(temp_c=21.5, hum_pct=55.0, pres_hpa=1013.25))
        self.assertIsInstance(s, EnvSample)
        self.assertAlmostEqual(s.temp_c, 21.5)
        self.assertAlmostEqual(s.pres_hpa, 1013.25)
        self.assertEqual(s.quality, Quality.OK)

    def test_env_pres_null(self):
        s = decode_sample("u", "env", envelope(temp_c=20.0, hum_pct=50.0, pres_hpa=None))
        self.assertIsNone(s.pres_hpa)

    def test_wall_ms_sentinel(self):
        s = decode_sample("u", "env", envelope(wall_ms=-1, temp_c=1, hum_pct=2, pres_hpa=3))
        self.assertIsNone(s.wall_ms)
        s2 = decode_sample("u", "env", envelope(wall_ms=1700000000000, temp_c=1, hum_pct=2, pres_hpa=3))
        self.assertEqual(s2.wall_ms, 1700000000000)

    def test_radar_sentinels(self):
        # firmware encodes "not measured" as -1 / -1.0
        s = decode_sample("u", "radar",
                          envelope(presence=True, distance_mm=-1, breath_bpm=-1.0, heart_bpm=72.0))
        self.assertIsInstance(s, RadarSample)
        self.assertTrue(s.presence)
        self.assertIsNone(s.distance_mm)
        self.assertIsNone(s.breath_bpm)
        self.assertAlmostEqual(s.heart_bpm, 72.0)

    def test_air_and_light(self):
        a = decode_sample("u", "air", envelope(co2_ppm=600, tvoc_ppb=300, aqi=2))
        self.assertIsInstance(a, AirSample)
        self.assertEqual((a.co2_ppm, a.tvoc_ppb, a.aqi), (600, 300, 2))
        ls = decode_sample("u", "light", envelope(lux=123.5))
        self.assertIsInstance(ls, LightSample)
        self.assertAlmostEqual(ls.lux, 123.5)

    def test_bad_payloads(self):
        with self.assertRaises(SchemaError):
            decode_sample("u", "env", b"{not json")
        with self.assertRaises(SchemaError):       # missing v.hum_pct
            decode_sample("u", "env", envelope(temp_c=1, pres_hpa=3))
        with self.assertRaises(SchemaError):       # missing envelope field
            decode_sample("u", "env", json.dumps({"ts_us": 1, "seq": 1, "q": 0}).encode())
        with self.assertRaises(SchemaError):       # q out of range
            decode_sample("u", "light", envelope(q=7, lux=1.0))
        with self.assertRaises(SchemaError):       # presence not bool
            decode_sample("u", "radar",
                          envelope(presence=1, distance_mm=1, breath_bpm=1.0, heart_bpm=1.0))


class TestQualityDedup(unittest.TestCase):
    def test_status(self):
        self.assertEqual(quality_to_status(Quality.OK), "final")
        self.assertEqual(quality_to_status(Quality.STALE), "preliminary")
        self.assertEqual(quality_to_status(Quality.DEGRADED), "preliminary")
        with self.assertRaises(ValueError):
            quality_to_status(Quality.INVALID)
        self.assertFalse(is_buildable(Quality.INVALID))

    def test_dedup(self):
        self.assertTrue(dedup.should_accept(None, 0))     # first sample
        self.assertTrue(dedup.should_accept(5, 6))        # forward
        self.assertTrue(dedup.should_accept(5, 1000))     # reboot gap (jump)
        self.assertFalse(dedup.should_accept(5, 5))       # duplicate
        self.assertFalse(dedup.should_accept(5, 4))       # re-send of older


class TestIdentifiers(unittest.TestCase):
    def test_value_and_query(self):
        self.assertEqual(identifiers.identifier_value("u", "heart", 7), "u-heart-7")
        self.assertEqual(
            identifiers.conditional_query("u", "heart", 7),
            "identifier=urn:rmms:seq|u-heart-7")


class TestCodes(unittest.TestCase):
    def test_cardiopulmonary_vitals_are_loinc(self):
        # Requirement: medical measurements use LOINC. Heart + breath rate have
        # verified LOINC codes.
        for field in ("heart_bpm", "breath_bpm"):
            self.assertEqual(CODES[field].code_system, LOINC, field)
            self.assertTrue(CODES[field].confirmed, field)

    def test_no_unverified_loinc_is_shipped(self):
        # Safety invariant (the review caught real-but-WRONG LOINCs): every code
        # that claims the LOINC system must be a verified one. Anything not
        # verified must use the explicit placeholder system, never a real LOINC.
        for field, spec in CODES.items():
            if spec.code_system == LOINC:
                self.assertTrue(spec.confirmed,
                                f"{field}: unverified code on the LOINC system")
            else:
                self.assertIn(spec.code_system, (SNOMED, RMMS_PLACEHOLDER), field)

    def test_placeholders_are_flagged(self):
        for field, spec in CODES.items():
            if spec.code_system == RMMS_PLACEHOLDER:
                self.assertFalse(spec.confirmed,
                                 f"{field}: placeholder must be flagged unconfirmed")

    def test_obs_keys_unique(self):
        keys = [s.obs_key for s in CODES.values()]
        self.assertEqual(len(keys), len(set(keys)))


class TestMapping(unittest.TestCase):
    def test_env_three_observations_kpa(self):
        s = EnvSample("u", 1, 1700000000000, 10, Quality.OK,
                      temp_c=21.5, hum_pct=55.0, pres_hpa=1013.25)
        obs = iter_observations(s, NOW)
        by_key = {o.code.obs_key: o for o in obs}
        self.assertEqual(set(by_key), {"temp", "humidity", "pressure"})
        self.assertAlmostEqual(by_key["pressure"].value, 1013.25)   # raw hPa (placeholder)
        self.assertAlmostEqual(by_key["temp"].value, 21.5)
        for o in obs:
            self.assertEqual(o.status, "final")
            self.assertEqual(o.effective, datetime.fromtimestamp(1700000000.0, tz=timezone.utc))

    def test_env_pres_null_skipped(self):
        s = EnvSample("u", 1, 1700000000000, 10, Quality.OK, 20.0, 50.0, pres_hpa=None)
        keys = {o.code.obs_key for o in iter_observations(s, NOW)}
        self.assertEqual(keys, {"temp", "humidity"})

    def test_radar_nulls_skipped_and_presence_bool(self):
        s = RadarSample("u", 1, 1700000000000, 5, Quality.OK,
                        presence=True, distance_mm=None, breath_bpm=None, heart_bpm=72.0)
        by_key = {o.code.obs_key: o for o in iter_observations(s, NOW)}
        self.assertEqual(set(by_key), {"heart", "presence"})
        self.assertIs(by_key["presence"].value, True)
        self.assertAlmostEqual(by_key["heart"].value, 72.0)

    def test_invalid_quality_yields_nothing(self):
        s = LightSample("u", 1, 1700000000000, 1, Quality.INVALID, lux=100.0)
        self.assertEqual(iter_observations(s, NOW), [])

    def test_no_rtc_forces_preliminary(self):
        s = LightSample("u", 1, None, 1, Quality.OK, lux=100.0)   # wall_ms None
        obs = iter_observations(s, NOW, latency_ms=500)
        self.assertEqual(len(obs), 1)
        self.assertEqual(obs[0].status, "preliminary")            # forced despite q=OK
        # effective = now - latency
        self.assertEqual(obs[0].effective, resolve_effective_datetime(None, NOW, 500))

    def test_degraded_is_preliminary(self):
        s = AirSample("u", 1, 1700000000000, 3, Quality.DEGRADED, 600, 300, 2)
        for o in iter_observations(s, NOW):
            self.assertEqual(o.status, "preliminary")

    def test_identifiers_unique_within_sample(self):
        s = RadarSample("u", 1, 1700000000000, 5, Quality.OK,
                        presence=True, distance_mm=2400, breath_bpm=16.0, heart_bpm=72.0)
        obs = iter_observations(s, NOW)
        ids = [o.identifier_value for o in obs]
        self.assertEqual(len(ids), 4)
        self.assertEqual(len(ids), len(set(ids)))          # all unique
        self.assertIn("u-heart-5", ids)
        for o in obs:
            self.assertEqual(o.identifier_system, "urn:rmms:seq")


if __name__ == "__main__":
    unittest.main(verbosity=2)
