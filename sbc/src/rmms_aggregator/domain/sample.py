"""Typed sensor-sample domain objects (CLAUDE.md §7).

One frozen dataclass per sensor topic. **No raw dicts past the JSON-decode
boundary** — every field is typed. `wall_ms` is `None` when the firmware sent the
`-1` sentinel (RTC not yet synced, firmware §9.2.1).

Pure module — stdlib only.
"""
from __future__ import annotations

from dataclasses import dataclass

from .quality import Quality


@dataclass(frozen=True, slots=True)
class EnvSample:
    device_uuid: str
    ts_us: int
    wall_ms: int | None
    seq: int
    quality: Quality
    temp_c: float
    hum_pct: float
    pres_hpa: float | None   # None on AHT21 boards (firmware §9.2.2)


@dataclass(frozen=True, slots=True)
class AirSample:
    device_uuid: str
    ts_us: int
    wall_ms: int | None
    seq: int
    quality: Quality
    co2_ppm: int
    tvoc_ppb: int
    aqi: int


@dataclass(frozen=True, slots=True)
class RadarSample:
    device_uuid: str
    ts_us: int
    wall_ms: int | None
    seq: int
    quality: Quality
    presence: bool
    distance_mm: int | None
    breath_bpm: float | None
    heart_bpm: float | None


@dataclass(frozen=True, slots=True)
class LightSample:
    device_uuid: str
    ts_us: int
    wall_ms: int | None
    seq: int
    quality: Quality
    lux: float


Sample = EnvSample | AirSample | RadarSample | LightSample

# Topic suffix → sensor name. The firmware publishes these under rmms/<uuid>/<sensor>
# (firmware §9.1). `air` is included: the firmware ships an ENS160 (firmware
# §9.2.2) even though the older subscription list in §6.2 predates it.
SENSORS = ("env", "air", "radar", "light")


def sensor_of(sample: Sample) -> str:
    return {
        EnvSample: "env",
        AirSample: "air",
        RadarSample: "radar",
        LightSample: "light",
    }[type(sample)]
