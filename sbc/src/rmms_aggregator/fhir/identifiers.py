"""Observation idempotency identifier scheme (CLAUDE.md §8.6).

Pure module — stdlib only.

Every Observation carries a stable, globally-unique business identifier derived
from the firmware envelope:

    system = "urn:rmms:seq"
    value  = "<device_uuid>-<obs_key>-<seq>"

`seq` is the firmware's per-topic sequence number (stable across spool re-sends,
monotonic-with-gaps across reboots — ADR-0003). `obs_key` distinguishes the
several Observations a single sample yields (e.g. a radar sample → heart, breath,
presence, distance), so each is uniquely identified even though they share `seq`.

The SBC POSTs with a FHIR conditional update keyed on this identifier, so a
re-send / retry / restart **updates or is ignored** by the server rather than
creating a duplicate.
"""
from __future__ import annotations

IDENTIFIER_SYSTEM = "urn:rmms:seq"


def identifier_value(device_uuid: str, obs_key: str, seq: int) -> str:
    return f"{device_uuid}-{obs_key}-{seq}"


def conditional_query(device_uuid: str, obs_key: str, seq: int) -> str:
    """`identifier=<system>|<value>` for a conditional update / If-None-Exist."""
    return f"identifier={IDENTIFIER_SYSTEM}|{identifier_value(device_uuid, obs_key, seq)}"
