"""`rmms-provision` — one-time deployment setup (CLAUDE.md §12).

    rmms-provision bind   --device <uuid> --patient <fhir-patient-id>
    rmms-provision device --uuid <uuid> [--model "RMMS-Sensor-v1"]
    rmms-provision patient --identifier "urn:rmms:demo|001" [--name ...] [--dob YYYY-MM-DD]
    rmms-provision healthcheck

`device`/`patient` create the FHIR resources via conditional update (idempotent on
their identifier). `bind` records the device→patient link in local SQLite — this
is what lets the service build Observation.subject (the patient identity never
touches the MCU). FHIR resources are built with `fhir.resources` (validated), not
hand-strung JSON.
"""
from __future__ import annotations

import argparse
import json
import sys
import time

import httpx

from ..config import get_settings
from ..fhir.oauth import TokenProvider
from ..storage.repository import Repository

DEVICE_SYSTEM = "urn:rmms:device"


def _now_ms() -> int:
    return int(time.time() * 1000)


def _client_and_headers(settings):
    tokens = TokenProvider(
        settings.fhir_oauth_token_url, settings.fhir_oauth_client_id,
        settings.fhir_oauth_client_secret.get_secret_value(), settings.fhir_oauth_scopes)
    headers = {"Content-Type": "application/fhir+json", "Accept": "application/fhir+json"}
    tok = tokens.token()
    if tok:
        headers["Authorization"] = f"Bearer {tok}"
    return tokens, headers


def _put_resource(settings, resource_type: str, identifier_query: str, body: dict) -> int:
    """Conditional update by identifier (idempotent). Returns HTTP status."""
    _tokens, headers = _client_and_headers(settings)
    url = f"{settings.fhir_endpoint.rstrip('/')}/{resource_type}?{identifier_query}"
    resp = httpx.put(url, json=body, headers=headers, timeout=30.0)
    print(f"{resource_type} -> HTTP {resp.status_code}")
    if not resp.is_success:
        print(resp.text[:1000], file=sys.stderr)
    return resp.status_code


def cmd_bind(args) -> int:
    repo = Repository(get_settings().db_path)
    repo.bind(args.device, args.patient, _now_ms())
    print(f"bound device {args.device} -> Patient/{args.patient}")
    return 0


def cmd_device(args) -> int:
    from fhir.resources.R4B.device import Device   # R4 family (see builder.py)
    body = {
        "resourceType": "Device",
        "identifier": [{"system": DEVICE_SYSTEM, "value": args.uuid}],
        "status": "active",
        "manufacturer": "TU Delft RMMS BAP",
        "deviceName": [{"name": args.model, "type": "user-friendly-name"}],
        "type": {"coding": [{
            "system": "http://snomed.info/sct", "code": "468063009",
            "display": "Vital signs monitor"}]},
    }
    Device.model_validate(body)   # build-time validation
    code = _put_resource(get_settings(), "Device",
                         f"identifier={DEVICE_SYSTEM}|{args.uuid}", body)
    return 0 if 200 <= code < 300 else 1


def cmd_patient(args) -> int:
    from fhir.resources.R4B.patient import Patient   # R4 family (see builder.py)
    system, _, value = args.identifier.partition("|")
    body: dict = {
        "resourceType": "Patient",
        "identifier": [{"system": system, "value": value}],
    }
    if args.name:
        body["name"] = [{"text": args.name}]
    if args.dob:
        body["birthDate"] = args.dob
    Patient.model_validate(body)
    code = _put_resource(get_settings(), "Patient",
                         f"identifier={args.identifier}", body)
    return 0 if 200 <= code < 300 else 1


def cmd_healthcheck(_args) -> int:
    settings = get_settings()
    ok = True
    # FHIR endpoint capability statement
    try:
        r = httpx.get(f"{settings.fhir_endpoint.rstrip('/')}/metadata", timeout=10.0)
        print(f"FHIR endpoint: HTTP {r.status_code}")
        ok = ok and r.is_success
    except httpx.RequestError as exc:
        print(f"FHIR endpoint UNREACHABLE: {exc}", file=sys.stderr)
        ok = False
    # OAuth token
    tokens = TokenProvider(
        settings.fhir_oauth_token_url, settings.fhir_oauth_client_id,
        settings.fhir_oauth_client_secret.get_secret_value(), settings.fhir_oauth_scopes)
    if tokens.enabled:
        try:
            tokens.token()
            print("OAuth: token acquired")
        except Exception as exc:
            print(f"OAuth FAILED: {exc}", file=sys.stderr)
            ok = False
    else:
        print("OAuth: disabled (dev)")
    print("healthcheck:", "OK" if ok else "FAILED")
    return 0 if ok else 1


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="rmms-provision")
    sub = p.add_subparsers(dest="cmd", required=True)

    b = sub.add_parser("bind", help="link a device UUID to a FHIR Patient id")
    b.add_argument("--device", required=True)
    b.add_argument("--patient", required=True)
    b.set_defaults(func=cmd_bind)

    d = sub.add_parser("device", help="create/update the FHIR Device for a sensor module")
    d.add_argument("--uuid", required=True)
    d.add_argument("--model", default="RMMS Sensor Module v1")
    d.set_defaults(func=cmd_device)

    pt = sub.add_parser("patient", help="create/update a FHIR Patient")
    pt.add_argument("--identifier", required=True, help="system|value, e.g. urn:rmms:demo|001")
    pt.add_argument("--name", default="")
    pt.add_argument("--dob", default="")
    pt.set_defaults(func=cmd_patient)

    h = sub.add_parser("healthcheck", help="verify broker/FHIR/OAuth reachability")
    h.set_defaults(func=cmd_healthcheck)

    args = p.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
