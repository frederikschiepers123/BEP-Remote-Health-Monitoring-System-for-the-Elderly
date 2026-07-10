"""Full end-to-end over a real socket: firmware JSON → Ingestor → Repository →
FHIR builder → real httpx FhirClient → an in-process FHIR-shaped HTTP server.

This is the closest thing to the live-HAPI bring-up (CLAUDE.md §16 step 7) that
runs without Docker: it exercises the actual network client, the actual
transaction-Bundle serialization on the wire, and the actual FHIR
transaction-response parsing — the layer respx stubs out elsewhere. It answers
the project's focus question directly: a real client POSTs a standards-shaped FHIR
R4 transaction Bundle and the data round-trips.
"""
from __future__ import annotations

import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from fhir.resources.R4B.bundle import Bundle

from rmms_aggregator.__main__ import _post_loop
from rmms_aggregator.fhir.client import FhirClient
from rmms_aggregator.fhir.oauth import TokenProvider
from rmms_aggregator.storage.models import Observation
from sqlalchemy import func, select

from payloads import env_payload, radar_payload

UUID = "dev-uuid-1"


class _FhirServer:
    """Minimal FHIR transaction endpoint: accepts a transaction Bundle POST,
    records it, and returns a transaction-response Bundle with per-entry
    locations (what a real server returns for a conditional update)."""

    def __init__(self) -> None:
        self.received: list[dict] = []
        server = self
        self._lock = threading.Lock()

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):  # noqa: N802
                body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
                bundle = json.loads(body)
                with server._lock:
                    server.received.append(bundle)
                n = len(bundle.get("entry", []))
                resp = {
                    "resourceType": "Bundle", "type": "transaction-response",
                    "entry": [{"response": {
                        "status": "200", "location": f"Observation/{i+1}/_history/1"}}
                        for i in range(n)],
                }
                payload = json.dumps(resp).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/fhir+json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

            def log_message(self, *a):  # quiet
                pass

        self._httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self._httpd.server_address[1]
        self._thread = threading.Thread(target=self._httpd.serve_forever, daemon=True)

    def __enter__(self):
        self._thread.start()
        return self

    def __exit__(self, *exc):
        self._httpd.shutdown()


def _posted_count(repo) -> int:
    with repo.session() as s:
        return s.execute(select(func.count()).select_from(Observation)
                         .where(Observation.status == "posted")).scalar_one()


def test_end_to_end_real_http(repo, ingestor, settings):
    repo.bind(UUID, "patient-9", now_ms=1)
    # ingest one of each interesting sensor (env=3 obs, radar=4 obs) = 7 total
    ingestor.handle(f"rmms/{UUID}/env", env_payload(seq=1, wall_ms=1700000000000))
    ingestor.handle(f"rmms/{UUID}/radar", radar_payload(seq=1, wall_ms=1700000000000))

    with _FhirServer() as srv:
        client = FhirClient(f"http://127.0.0.1:{srv.port}/fhir", TokenProvider("", "", "", ""))
        stop = threading.Event()
        t = threading.Thread(target=_post_loop,
                             args=(repo, client, ingestor, settings, stop), daemon=True)
        t.start()
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline and _posted_count(repo) < 7:
            time.sleep(0.02)
        stop.set()
        t.join(timeout=3.0)
        assert not t.is_alive()

        # the service marked every Observation posted
        assert _posted_count(repo) == 7
        # the server received standards-shaped FHIR R4 transaction Bundle(s)
        assert srv.received, "server received nothing"
        all_entries = []
        for b in srv.received:
            Bundle.model_validate(b)                 # real FHIR R4 validation of the wire bytes
            assert b["type"] == "transaction"
            all_entries.extend(b["entry"])
        assert len(all_entries) == 7
        # every entry is a conditional update keyed on the idempotency identifier
        for e in all_entries:
            assert e["request"]["method"] == "PUT"
            assert e["request"]["url"].startswith("Observation?identifier=urn:rmms:seq|")
            assert e["resource"]["resourceType"] == "Observation"
        # server_ids were captured back from the FHIR response locations
        with repo.session() as s:
            server_ids = s.execute(select(Observation.server_id)).scalars().all()
        assert all(sid and sid.startswith("Observation/") for sid in server_ids)
