"""FHIR resource construction with build-time validation (CLAUDE.md §9.3).

Thin adapter over the pure ``resource_json`` shaping: it runs each Observation
(and the outbound Bundle) through ``fhir.resources`` so cardinality / typing
errors are caught at build time. A validation failure raises
``FhirValidationError`` and the caller dead-letters the sample (never silently
drop, §9.3). This module is the only place that imports the FHIR library.
"""
from __future__ import annotations

import json

# FHIR R4 (R4B = FHIR 4.3, the R4-family release carried by the pydantic-v2
# fhir.resources 7.x line). The top-level `fhir.resources.*` modules in 7.x are
# R5, so R4 resources are imported from the .R4B subpackage. See pyproject.toml.
from fhir.resources.R4B.bundle import Bundle
from fhir.resources.R4B.observation import Observation

from .identifiers import IDENTIFIER_SYSTEM
from .mapping import PlannedObservation
from .resource_json import bundle_transaction_dict, observation_dict


class FhirValidationError(ValueError):
    """A constructed resource failed FHIR R4 validation."""


def build_and_validate(planned: PlannedObservation, patient_id: str) -> tuple[str, str]:
    """Return (identifier_value, compact FHIR JSON) for one Observation.

    Raises FhirValidationError if the resource is not valid FHIR R4."""
    data = observation_dict(planned, patient_id)
    try:
        Observation.model_validate(data)
    except Exception as exc:   # pydantic ValidationError et al.
        raise FhirValidationError(f"invalid Observation {planned.identifier_value}: {exc}") from exc
    return planned.identifier_value, json.dumps(data, separators=(",", ":"))


def build_post_bundle(rows: list[tuple[int, str, str]]) -> dict:
    """Build + validate a transaction Bundle from queued (id, identifier, json) rows.

    Each entry is a conditional update keyed on the Observation identifier, so the
    server updates-or-ignores rather than duplicating (§8.6/§9.2)."""
    items: list[tuple[str, dict]] = [
        (f"Observation?identifier={IDENTIFIER_SYSTEM}|{ident}", json.loads(fhir_json))
        for (_id, ident, fhir_json) in rows
    ]
    bundle = bundle_transaction_dict(items)
    Bundle.model_validate(bundle)
    return bundle


def partition_valid(
    rows: list[tuple[int, str, str]]
) -> tuple[list[tuple[int, str, str]], list[int]]:
    """Split queued rows into (valid, bad_ids) by validating each Observation
    individually. Used to isolate one corrupt stored Observation so it can be
    dead-lettered without discarding the rest of the batch (§9.3)."""
    valid: list[tuple[int, str, str]] = []
    bad: list[int] = []
    for (oid, ident, fhir_json) in rows:
        try:
            Observation.model_validate(json.loads(fhir_json))
            valid.append((oid, ident, fhir_json))
        except Exception:
            bad.append(oid)
    return valid, bad
