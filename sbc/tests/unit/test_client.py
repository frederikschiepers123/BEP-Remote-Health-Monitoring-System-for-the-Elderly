"""FHIR client: response classification (success / retriable / permanent)."""
import httpx
import respx

from rmms_aggregator.fhir.client import FhirClient
from rmms_aggregator.fhir.oauth import TokenProvider

ENDPOINT = "https://fhir.example.org/fhir"
NO_AUTH = TokenProvider("", "", "", "")   # disabled → no Authorization header


def _client():
    return FhirClient(ENDPOINT, NO_AUTH, timeout_s=5.0)


@respx.mock
def test_success_parses_locations():
    respx.post(ENDPOINT).mock(return_value=httpx.Response(200, json={
        "resourceType": "Bundle", "type": "transaction-response",
        "entry": [{"response": {"status": "201", "location": "Observation/1/_history/1"}}],
    }))
    r = _client().post_bundle({"resourceType": "Bundle", "type": "transaction", "entry": []})
    assert r.ok and not r.retriable
    assert r.locations == ["Observation/1/_history/1"]


@respx.mock
def test_429_is_retriable_with_retry_after():
    respx.post(ENDPOINT).mock(return_value=httpx.Response(429, headers={"Retry-After": "12"}))
    r = _client().post_bundle({"resourceType": "Bundle", "type": "transaction"})
    assert not r.ok and r.retriable and r.retry_after_s == 12.0


@respx.mock
def test_5xx_is_retriable():
    respx.post(ENDPOINT).mock(return_value=httpx.Response(503, text="busy"))
    r = _client().post_bundle({"resourceType": "Bundle", "type": "transaction"})
    assert not r.ok and r.retriable


@respx.mock
def test_4xx_is_permanent():
    respx.post(ENDPOINT).mock(return_value=httpx.Response(400, text="bad bundle"))
    r = _client().post_bundle({"resourceType": "Bundle", "type": "transaction"})
    assert not r.ok and not r.retriable


@respx.mock
def test_connection_error_is_retriable():
    respx.post(ENDPOINT).mock(side_effect=httpx.ConnectError("refused"))
    r = _client().post_bundle({"resourceType": "Bundle", "type": "transaction"})
    assert not r.ok and r.retriable
