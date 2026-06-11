"""Per-(device, sensor) sequence dedup (CLAUDE.md §6.3).

Pure module — stdlib only.

The firmware's `seq` is monotonic-with-gaps across reboots (it jumps forward on
boot so a value is never reused — firmware ADR-0003). So:
  - a forward jump (reboot gap) is `seq > last_seen` → accepted;
  - a spool re-send after an outage carries `seq <= last_seen` → dropped here as a
    duplicate.
This in-memory filter is the fast path; the FHIR-side `Observation.identifier`
conditional update (fhir/identifiers.py, §8.6) is the durable backstop that also
covers an SBC restart (when `last_seen` is empty).
"""
from __future__ import annotations


def should_accept(last_seen_seq: int | None, seq: int) -> bool:
    """True if this `seq` is newer than the last accepted one for the pair."""
    return last_seen_seq is None or seq > last_seen_seq
