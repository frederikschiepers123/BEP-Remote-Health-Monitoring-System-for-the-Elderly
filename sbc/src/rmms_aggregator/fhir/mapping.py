"""Plan the FHIR Observations for a sample — the pure half of the FHIR builder.

This module decides *what* Observations a sample yields, *which* code and value
each carries, *which* status, and *which* identifier — all without importing the
FHIR libraries, so the mapping is fully unit-testable. `fhir/builder.py` turns
each PlannedObservation into a validated `fhir.resources` resource.

Per CLAUDE.md §8.1 each measured value is a SEPARATE Observation (radar's
heart/breath/presence are not panel members). §8.4/§8.5 govern status and time.
"""
from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timedelta, timezone

from ..domain.quality import Quality, is_buildable, quality_to_status
from ..domain.sample import (
    AirSample, EnvSample, LightSample, RadarSample, Sample,
)
from . import identifiers
from .codes import CodeSpec, VALUE_BOOLEAN, VALUE_INTEGER, code_for

DEFAULT_LATENCY_MS = 500   # §8.5 ESTIMATED_TRANSPORT_LATENCY (config-tunable)


@dataclass(frozen=True, slots=True)
class PlannedObservation:
    device_uuid: str
    seq: int
    code: CodeSpec
    value: float | int | bool
    status: str                 # "final" | "preliminary"
    identifier_system: str
    identifier_value: str
    effective: datetime         # tz-aware UTC


def resolve_effective_datetime(
    wall_ms: int | None, now: datetime, latency_ms: int = DEFAULT_LATENCY_MS
) -> datetime:
    """Real wall-clock if the firmware had RTC sync, else a receive-time estimate
    (§8.5). `now` must be tz-aware UTC."""
    if wall_ms is not None:
        return datetime.fromtimestamp(wall_ms / 1000.0, tz=timezone.utc)
    return now - timedelta(milliseconds=latency_ms)


def _fields(sample: Sample) -> list[tuple[str, object]]:
    """(field_name, raw_value) pairs for a sample; None values are dropped by the
    caller. Field names match CODES keys and domain.sample attributes."""
    if isinstance(sample, EnvSample):
        return [("temp_c", sample.temp_c), ("hum_pct", sample.hum_pct),
                ("pres_hpa", sample.pres_hpa)]
    if isinstance(sample, AirSample):
        return [("co2_ppm", sample.co2_ppm), ("tvoc_ppb", sample.tvoc_ppb),
                ("aqi", sample.aqi)]
    if isinstance(sample, RadarSample):
        return [("heart_bpm", sample.heart_bpm), ("breath_bpm", sample.breath_bpm),
                ("presence", sample.presence), ("distance_mm", sample.distance_mm)]
    if isinstance(sample, LightSample):
        return [("lux", sample.lux)]
    raise TypeError(f"unknown sample type {type(sample).__name__}")


def _coerce(code: CodeSpec, raw: object) -> float | int | bool:
    if code.value_kind == VALUE_BOOLEAN:
        return bool(raw)
    if code.value_kind == VALUE_INTEGER:
        return int(round(float(raw) * code.scale))   # type: ignore[arg-type]
    return float(raw) * code.scale                    # VALUE_QUANTITY  # type: ignore[arg-type]


def iter_observations(
    sample: Sample, now: datetime, latency_ms: int = DEFAULT_LATENCY_MS
) -> list[PlannedObservation]:
    """Plan every Observation for `sample`.

    Returns [] for a q=INVALID sample — the caller must dead-letter it (never
    build it, §8.4). Fields that are None (not measured) are skipped. A sample
    without RTC sync (`wall_ms is None`) forces every Observation to
    `preliminary` regardless of quality (§8.4 — the timestamp itself is a guess).
    """
    if not is_buildable(sample.quality):
        return []

    status = quality_to_status(Quality(sample.quality))
    if sample.wall_ms is None:
        status = "preliminary"
    effective = resolve_effective_datetime(sample.wall_ms, now, latency_ms)

    planned: list[PlannedObservation] = []
    for field, raw in _fields(sample):
        if raw is None:
            continue
        code = code_for(field)
        if code is None:
            continue   # field with no code mapping (should not happen for known samples)
        planned.append(PlannedObservation(
            device_uuid=sample.device_uuid,
            seq=sample.seq,
            code=code,
            value=_coerce(code, raw),
            status=status,
            identifier_system=identifiers.IDENTIFIER_SYSTEM,
            identifier_value=identifiers.identifier_value(
                sample.device_uuid, code.obs_key, sample.seq),
            effective=effective,
        ))
    return planned
