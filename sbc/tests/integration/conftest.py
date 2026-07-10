"""Fixtures for the in-process integration tests.

These exercise the REAL pipeline objects (Ingestor + Repository + FHIR builder +
post loop) wired together against a throwaway SQLite DB and stubbed network
edges (MQTT publish, FHIR HTTP). No Docker, so they run on CI and in the dev
sandbox; the live-broker / live-HAPI variant (CLAUDE.md §15.2) is the deployment
bring-up step, documented in RUNBOOK.md.
"""
from __future__ import annotations

import pytest

from rmms_aggregator.config import Settings
from rmms_aggregator.mqtt.subscriber import Ingestor
from rmms_aggregator.storage.models import Base
from rmms_aggregator.storage.repository import Repository


@pytest.fixture()
def settings() -> Settings:
    # Explicit kwargs (highest pydantic-settings priority) so the test never
    # depends on the host environment or a stray .env file.
    return Settings(
        broker_host="tablet.local",
        broker_ca_path="/dev/null",
        broker_cert_path="/dev/null",
        broker_key_path="/dev/null",
        fhir_endpoint="http://localhost:8080/fhir",
        db_path=":unused:",                  # repo fixture owns the real path
        estimated_transport_latency_ms=500,
        bundle_interval_s=0.05,              # keep the post loop snappy in tests
    )


@pytest.fixture()
def repo(tmp_path) -> Repository:
    r = Repository(str(tmp_path / "agg.db"))
    Base.metadata.create_all(r.engine)       # test-only; prod uses alembic (§10.3)
    return r


@pytest.fixture()
def published() -> list[tuple[str, str, bool]]:
    """Capture of (topic, payload, retain) for the time-sync publisher."""
    return []


@pytest.fixture()
def ingestor(repo, settings, published) -> Ingestor:
    def time_pub(uuid: str) -> None:
        published.append(("rmms/%s/time/set" % uuid, uuid, True))
    return Ingestor(repo, settings, time_sync_publisher=time_pub)
