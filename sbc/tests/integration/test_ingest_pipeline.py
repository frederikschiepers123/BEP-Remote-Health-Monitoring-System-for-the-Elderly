"""End-to-end ingest pipeline (in-process): the REAL Ingestor driving the REAL
Repository + FHIR builder. This is the composition that the unit tests cover only
in isolation — decode → dedup → persist → patient-binding gate → build → enqueue,
plus every dead-letter path and the redrive-after-bind path.

The firmware contract (firmware §9.2) is the input; queued FHIR Observations
(idempotency identifier + correct code/value) are the output. Nothing is ever
silently dropped (CLAUDE.md §6.3/§9.3): an undecodable / unbuildable / invalid
payload always lands in the dead_letters table.
"""
from __future__ import annotations

from sqlalchemy import func, select

from rmms_aggregator.storage.models import DeadLetter, Sample

from payloads import env_payload, envelope, radar_payload

UUID = "dev-uuid-1"
ENV_TOPIC = f"rmms/{UUID}/env"
RADAR_TOPIC = f"rmms/{UUID}/radar"


# ── DB inspection helpers ───────────────────────────────────────────────────
def _samples(repo):
    with repo.session() as s:
        return s.execute(select(Sample.id, Sample.seq, Sample.built, Sample.wall_ms)).all()


def _dead_letters(repo):
    with repo.session() as s:
        return s.execute(select(DeadLetter.topic, DeadLetter.reason)).all()


def _pending_identifiers(repo):
    return sorted(ident for _id, ident, _json in repo.fetch_pending(100))


def _dead_letter_count(repo):
    with repo.session() as s:
        return s.execute(select(func.count()).select_from(DeadLetter)).scalar_one()


# ── happy path ──────────────────────────────────────────────────────────────
def test_env_sample_bound_builds_three_observations(repo, ingestor):
    repo.bind(UUID, "patient-9", now_ms=1)
    ingestor.handle(ENV_TOPIC, env_payload(seq=10))

    assert _pending_identifiers(repo) == [
        f"{UUID}-humidity-10", f"{UUID}-pressure-10", f"{UUID}-temp-10"]
    rows = _samples(repo)
    assert len(rows) == 1 and rows[0].built == 1     # built once Observations enqueued
    assert _dead_letter_count(repo) == 0


def test_radar_nulls_skipped_through_pipeline(repo, ingestor):
    repo.bind(UUID, "p1", now_ms=1)
    ingestor.handle(RADAR_TOPIC,
                    radar_payload(seq=5, distance_mm=-1, breath_bpm=-1.0, heart_bpm=72.0))
    # distance + breath were "not measured" sentinels → skipped; heart + presence remain
    assert _pending_identifiers(repo) == [f"{UUID}-heart-5", f"{UUID}-presence-5"]


def test_real_wall_clock_is_preserved(repo, ingestor):
    repo.bind(UUID, "p1", now_ms=1)
    ingestor.handle(ENV_TOPIC, env_payload(seq=1, wall_ms=1700000000000))
    assert _samples(repo)[0].wall_ms == 1700000000000


# ── dedup / idempotency ─────────────────────────────────────────────────────
def test_duplicate_seq_is_deduped(repo, ingestor):
    repo.bind(UUID, "p1", now_ms=1)
    ingestor.handle(ENV_TOPIC, env_payload(seq=10))
    ingestor.handle(ENV_TOPIC, env_payload(seq=10))      # exact re-send (broker redelivery)
    assert len(_samples(repo)) == 1
    assert len(_pending_identifiers(repo)) == 3          # not 6


def test_durable_dedup_survives_cursor_reset(repo, ingestor):
    """A re-send after the in-memory cursor is lost (service restart) is still
    rejected by the UNIQUE(uuid, sensor, seq) constraint."""
    repo.bind(UUID, "p1", now_ms=1)
    ingestor.handle(ENV_TOPIC, env_payload(seq=10))
    ingestor._last_seq.clear()                           # simulate restart
    ingestor.handle(ENV_TOPIC, env_payload(seq=10))
    assert len(_samples(repo)) == 1
    assert len(_pending_identifiers(repo)) == 3


def test_forward_seq_after_reboot_gap_accepted(repo, ingestor):
    repo.bind(UUID, "p1", now_ms=1)
    ingestor.handle(ENV_TOPIC, env_payload(seq=10))
    ingestor.handle(ENV_TOPIC, env_payload(seq=5000))    # reboot jump (firmware ADR-0003)
    assert {r.seq for r in _samples(repo)} == {10, 5000}


# ── dead-letter paths (never silently drop) ─────────────────────────────────
def test_invalid_quality_dead_letters(repo, ingestor):
    repo.bind(UUID, "p1", now_ms=1)
    ingestor.handle(ENV_TOPIC, env_payload(seq=1, q=3))   # q=INVALID
    assert _pending_identifiers(repo) == []               # nothing built
    dls = _dead_letters(repo)
    assert len(dls) == 1 and dls[0].reason == "quality:invalid"
    assert _samples(repo)[0].built == 1                   # accounted for, not re-driven


def test_malformed_json_dead_letters_without_storing_sample(repo, ingestor):
    repo.bind(UUID, "p1", now_ms=1)
    ingestor.handle(ENV_TOPIC, b"{not json")
    assert _samples(repo) == []
    dls = _dead_letters(repo)
    assert len(dls) == 1 and dls[0].reason.startswith("schema:")


def test_schema_violation_dead_letters(repo, ingestor):
    repo.bind(UUID, "p1", now_ms=1)
    ingestor.handle(ENV_TOPIC, envelope(seq=1, temp_c=1.0))   # missing hum_pct / pres_hpa
    assert _samples(repo) == []
    assert _dead_letter_count(repo) == 1


def test_unknown_topic_dead_letters(repo, ingestor):
    ingestor.handle(f"rmms/{UUID}/ir", b"{}")             # 'ir' is not a known sensor
    assert _samples(repo) == []
    dls = _dead_letters(repo)
    assert len(dls) == 1 and dls[0].reason.startswith("topic:")


# ── binding-after-the-fact (redrive) ────────────────────────────────────────
def test_sample_before_binding_stored_then_redriven(repo, ingestor):
    # No binding yet: the sample must be stored (built=0) and NOT dead-lettered.
    ingestor.handle(ENV_TOPIC, env_payload(seq=10))
    rows = _samples(repo)
    assert len(rows) == 1 and rows[0].built == 0
    assert _pending_identifiers(repo) == []
    assert _dead_letter_count(repo) == 0

    # Bind the device, then re-drive: the previously-stored sample now builds.
    repo.bind(UUID, "patient-9", now_ms=2)
    assert ingestor.redrive_unbuilt() == 1
    assert _pending_identifiers(repo) == [
        f"{UUID}-humidity-10", f"{UUID}-pressure-10", f"{UUID}-temp-10"]
    assert _samples(repo)[0].built == 1


def test_redrive_is_noop_without_binding(repo, ingestor):
    ingestor.handle(ENV_TOPIC, env_payload(seq=10))
    assert ingestor.redrive_unbuilt() == 0               # still unbound → nothing to do
    assert _pending_identifiers(repo) == []


# ── status / time-sync ──────────────────────────────────────────────────────
def test_status_online_publishes_time_sync(repo, ingestor, published):
    ingestor.handle(f"rmms/{UUID}/status", b"online")
    assert published == [(f"rmms/{UUID}/time/set", UUID, True)]


def test_status_offline_does_not_publish(repo, ingestor, published):
    ingestor.handle(f"rmms/{UUID}/status", b"offline")
    assert published == []
