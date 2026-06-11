"""FHIR R4 JSON shaping (pure — stdlib only, so it is unit-testable).

Produces the dict form of an Observation and of a transaction Bundle. The dicts
are validated through ``fhir.resources`` in ``builder.py`` before they are stored
or posted — so we get the spec-mandated build-time validation (§9.3) without this
module importing the heavy FHIR library.
"""
from __future__ import annotations

import html
import uuid
from datetime import timezone

from .codes import UCUM, VALUE_BOOLEAN
from .identifiers import conditional_query
from .mapping import PlannedObservation

CATEGORY_SYSTEM = "http://terminology.hl7.org/CodeSystem/observation-category"


def _fhir_datetime(planned: PlannedObservation) -> str:
    return (planned.effective.astimezone(timezone.utc)
            .isoformat(timespec="milliseconds").replace("+00:00", "Z"))


def _narrative(display: str, value_text: str) -> dict:
    """A minimal generated DomainResource.text (satisfies the dom-6 best practice
    so a strict validator run is clean). XHTML-escaped."""
    div = (f'<div xmlns="http://www.w3.org/1999/xhtml">'
           f'{html.escape(display)}: {html.escape(value_text)}</div>')
    return {"status": "generated", "div": div}


def observation_dict(planned: PlannedObservation, patient_id: str) -> dict:
    """A FHIR R4 Observation as a JSON dict (one measured value)."""
    code = planned.code
    obs: dict = {
        "resourceType": "Observation",
        "identifier": [{
            "system": planned.identifier_system,
            "value": planned.identifier_value,
        }],
        "status": planned.status,
        "category": [{"coding": [{"system": CATEGORY_SYSTEM, "code": code.category}]}],
        "code": {"coding": [{
            "system": code.code_system,
            "code": code.code,
            "display": code.display,
        }]},
        "subject": {"reference": f"Patient/{patient_id}"},
        "device": {"reference": f"Device/{planned.device_uuid}"},
        "effectiveDateTime": _fhir_datetime(planned),
    }
    if code.value_kind == VALUE_BOOLEAN:
        val = bool(planned.value)
        obs["valueBoolean"] = val
        value_text = "detected" if val else "not detected"
    else:
        quantity: dict = {"value": planned.value, "unit": code.unit}
        if code.ucum_code:
            quantity["system"] = UCUM
            quantity["code"] = code.ucum_code
        obs["valueQuantity"] = quantity
        value_text = f"{planned.value} {code.unit}"
    obs["text"] = _narrative(code.display, value_text)
    return obs


def conditional_url(planned: PlannedObservation) -> str:
    """`Observation?identifier=...` for the entry's conditional update (§8.6)."""
    return "Observation?" + conditional_query(
        planned.device_uuid, planned.code.obs_key, planned.seq)


def bundle_transaction_dict(items: list[tuple[str, dict]]) -> dict:
    """A FHIR transaction Bundle. `items` is (conditional_url, observation_dict).

    Each entry is a conditional update (PUT ?identifier=…), so re-posting an
    Observation that already exists updates-or-ignores it rather than creating a
    duplicate — the server-side half of the idempotency contract (§8.6/§9.2).

    Each entry carries a fullUrl (FHIR R4 requires it on non-POST transaction
    entries). It is a deterministic urn:uuid derived from the Observation's
    business identifier (parsed from the conditional url), so a re-sent Bundle
    produces the SAME fullUrl — stable, idempotent, and validator-clean."""
    entries = []
    for url, obs in items:
        ident_value = url.split("|", 1)[-1]   # ...?identifier=urn:rmms:seq|<value>
        full_url = "urn:uuid:" + str(uuid.uuid5(uuid.NAMESPACE_URL, ident_value))
        entries.append({
            "fullUrl": full_url,
            "resource": obs,
            "request": {"method": "PUT", "url": url},
        })
    return {"resourceType": "Bundle", "type": "transaction", "entry": entries}
