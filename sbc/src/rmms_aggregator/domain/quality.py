"""Sample quality flag (firmware §9.2.1) and its FHIR status mapping.

Pure module — stdlib only, so it is unit-testable without the service's
runtime dependencies.
"""
from __future__ import annotations

from enum import IntEnum


class Quality(IntEnum):
    """Envelope `q` field, per firmware CLAUDE.md §9.2.1."""

    OK = 0        # steady-state, reading usable
    STALE = 1     # last-known value re-emitted
    DEGRADED = 2  # usable but suspect (e.g. ENS160 warm-up, radar ghost)
    INVALID = 3   # output unusable


# FHIR R4 Observation.status. INVALID is intentionally absent: a q=3 sample is
# never turned into an Observation — it is dead-lettered (CLAUDE.md §8.4).
_QUALITY_STATUS = {
    Quality.OK: "final",
    Quality.STALE: "preliminary",
    Quality.DEGRADED: "preliminary",
}


def quality_to_status(q: Quality) -> str:
    """Map a (buildable) quality to an Observation.status.

    Raises ValueError for Quality.INVALID — the caller must dead-letter such a
    sample rather than post it (never silently drop, CLAUDE.md §8.4/§9.3).
    """
    try:
        return _QUALITY_STATUS[q]
    except KeyError as exc:  # Quality.INVALID
        raise ValueError(f"quality {q!r} is not buildable; dead-letter it") from exc


def is_buildable(q: Quality) -> bool:
    """True if a sample with this quality may be turned into an Observation."""
    return q in _QUALITY_STATUS
