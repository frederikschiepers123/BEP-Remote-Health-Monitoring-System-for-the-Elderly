"""Canned firmware sensor payloads (firmware §9.2) shared by the integration
tests. A uniquely-named helper module (not conftest) so it imports cleanly under
pytest's prepend path insertion without colliding with tests/unit/conftest.py.
"""
from __future__ import annotations

import json


def envelope(seq: int = 1, q: int = 0, wall_ms: int = -1, ts_us: int = 1000, **v) -> bytes:
    return json.dumps(
        {"ts_us": ts_us, "wall_ms": wall_ms, "seq": seq, "q": q, "v": v}
    ).encode()


def env_payload(seq=1, q=0, wall_ms=-1, temp_c=21.5, hum_pct=55.0, pres_hpa=1013.25) -> bytes:
    return envelope(seq=seq, q=q, wall_ms=wall_ms,
                    temp_c=temp_c, hum_pct=hum_pct, pres_hpa=pres_hpa)


def radar_payload(seq=1, q=0, wall_ms=-1, presence=True, distance_mm=2400,
                  breath_bpm=16.0, heart_bpm=72.0) -> bytes:
    return envelope(seq=seq, q=q, wall_ms=wall_ms, presence=presence,
                    distance_mm=distance_mm, breath_bpm=breath_bpm, heart_bpm=heart_bpm)
