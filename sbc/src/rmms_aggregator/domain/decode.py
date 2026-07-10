"""Decode raw MQTT topic + JSON payload into a typed Sample (firmware §9.2).

Pure module — stdlib `json` only, so it is unit-testable without a broker.

A malformed payload raises `SchemaError`; the caller dead-letters it with the raw
bytes (never silently drop — CLAUDE.md §6.3/§9.3).
"""
from __future__ import annotations

import json

from .quality import Quality
from .sample import AirSample, EnvSample, LightSample, RadarSample, Sample, SENSORS

TOPIC_ROOT = "rmms"
STATUS_SUFFIX = "status"

# The topic suffixes the SBC subscribes to (CLAUDE.md §6.2): the four sensor
# topics plus the retained `status` topic. `status` is NOT a sensor sample — the
# subscriber routes it to the device-online / time-sync path — but it IS a valid
# firmware topic, so parse_topic must accept it rather than dead-letter it (which
# would also suppress the time-sync publish on `status="online"`, §9.2.5).
_VALID_SUFFIXES = frozenset((*SENSORS, STATUS_SUFFIX))


class SchemaError(ValueError):
    """The payload (or topic) did not match the firmware §9.2 contract."""


def parse_topic(topic: str) -> tuple[str, str]:
    """`rmms/<uuid>/<suffix>` → (uuid, suffix) for a sensor topic or `status`.

    Raises SchemaError for any other shape / unknown suffix (the caller
    dead-letters it — the firmware should never publish a topic the SBC does not
    recognise, §6.3)."""
    parts = topic.split("/")
    if len(parts) != 3 or parts[0] != TOPIC_ROOT:
        raise SchemaError(f"unexpected topic shape: {topic!r}")
    uuid, suffix = parts[1], parts[2]
    if suffix not in _VALID_SUFFIXES:
        raise SchemaError(f"unknown topic suffix: {suffix!r}")
    if not uuid:
        raise SchemaError("empty device uuid in topic")
    return uuid, suffix


def _num(v: object, field: str) -> float:
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        raise SchemaError(f"field {field!r} must be a number, got {type(v).__name__}")
    return float(v)


def _int(v: object, field: str) -> int:
    if isinstance(v, bool) or not isinstance(v, int):
        raise SchemaError(f"field {field!r} must be an integer, got {type(v).__name__}")
    return v


def _opt_pos_num(v: object, field: str) -> float | None:
    """Firmware encodes "not measured" as null or a negative sentinel (§9.2.2)."""
    if v is None:
        return None
    n = _num(v, field)
    return n if n >= 0 else None


def _opt_pos_int(v: object, field: str) -> int | None:
    if v is None:
        return None
    n = _int(v, field)
    return n if n >= 0 else None


def _envelope(payload: bytes) -> tuple[dict, int, int | None, int, Quality]:
    try:
        obj = json.loads(payload)
    except (json.JSONDecodeError, UnicodeDecodeError) as exc:
        raise SchemaError(f"invalid JSON: {exc}") from exc
    if not isinstance(obj, dict):
        raise SchemaError("payload is not a JSON object")
    try:
        ts_us = _int(obj["ts_us"], "ts_us")
        seq = _int(obj["seq"], "seq")
        q_raw = _int(obj["q"], "q")
        wall_raw = _int(obj["wall_ms"], "wall_ms")
        v = obj["v"]
    except KeyError as exc:
        raise SchemaError(f"missing envelope field {exc}") from exc
    if not isinstance(v, dict):
        raise SchemaError("envelope `v` is not an object")
    if q_raw not in (0, 1, 2, 3):
        raise SchemaError(f"q out of range: {q_raw}")
    wall_ms = None if wall_raw == -1 else wall_raw   # §9.2.1 sentinel → None
    return v, ts_us, wall_ms, seq, Quality(q_raw)


def decode_sample(uuid: str, sensor: str, payload: bytes) -> Sample:
    """Build the typed Sample for `sensor`. Raises SchemaError on any mismatch."""
    v, ts_us, wall_ms, seq, q = _envelope(payload)

    def vget(key: str) -> object:
        if key not in v:
            raise SchemaError(f"missing v.{key} for {sensor} sample")
        return v[key]

    if sensor == "env":
        return EnvSample(
            uuid, ts_us, wall_ms, seq, q,
            temp_c=_num(vget("temp_c"), "temp_c"),
            hum_pct=(None if vget("hum_pct") is None else _num(v["hum_pct"], "hum_pct")),
            pres_hpa=(None if vget("pres_hpa") is None else _num(v["pres_hpa"], "pres_hpa")),
        )
    if sensor == "air":
        return AirSample(
            uuid, ts_us, wall_ms, seq, q,
            co2_ppm=_int(vget("co2_ppm"), "co2_ppm"),
            tvoc_ppb=_int(vget("tvoc_ppb"), "tvoc_ppb"),
            aqi=_int(vget("aqi"), "aqi"),
        )
    if sensor == "radar":
        pres = vget("presence")
        if not isinstance(pres, bool):
            raise SchemaError("v.presence must be a boolean")
        return RadarSample(
            uuid, ts_us, wall_ms, seq, q,
            presence=pres,
            distance_mm=_opt_pos_int(vget("distance_mm"), "distance_mm"),
            breath_bpm=_opt_pos_num(vget("breath_bpm"), "breath_bpm"),
            heart_bpm=_opt_pos_num(vget("heart_bpm"), "heart_bpm"),
        )
    if sensor == "light":
        return LightSample(
            uuid, ts_us, wall_ms, seq, q,
            lux=_num(vget("lux"), "lux"),
        )
    raise SchemaError(f"unhandled sensor {sensor!r}")   # unreachable via parse_topic
