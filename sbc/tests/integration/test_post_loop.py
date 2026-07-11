"""Store-and-forward post loop (`__main__._post_loop`) driven against a real
Repository + real FHIR builder, with the FHIR HTTP edge replaced by a fake
client. This is the outbound half of the ≥24 h outage tolerance (CLAUDE.md §9.2,
§10): pending Observations drain to the endpoint, a transient (5xx / connection)
failure keeps them pending and retries, and a permanent (4xx) error dead-letters
them — never a silent drop.

The loop is run on a background thread and stopped as soon as the DB reaches the
expected terminal state; `stop.set()` interrupts any in-progress backoff wait, so
the tests are fast and deterministic without monkeypatching time.
"""
from __future__ import annotations

import threading
import time

from rmms_aggregator.__main__ import _post_loop
from rmms_aggregator.fhir.client import PostResult
from rmms_aggregator.storage.models import STATUS_DEAD_LETTER, STATUS_PENDING, Observation
from sqlalchemy import func, select

from payloads import env_payload, radar_payload

UUID = "dev-uuid-1"


class FakeClient:
    """Records posted bundles; returns a scripted PostResult per call (the last
    entry repeats for any further calls)."""

    def __init__(self, *results: PostResult) -> None:
        self._results = list(results) or [PostResult(True, False)]
        self.bundles: list[dict] = []

    def post_bundle(self, bundle: dict) -> PostResult:
        self.bundles.append(bundle)
        return self._results[min(len(self.bundles) - 1, len(self._results) - 1)]


def _run_until(repo, client, ingestor, settings, predicate, timeout=3.0) -> None:
    stop = threading.Event()
    t = threading.Thread(target=_post_loop,
                         args=(repo, client, ingestor, settings, stop), daemon=True)
    t.start()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and not predicate():
        time.sleep(0.01)
    stop.set()                 # interrupts any in-flight backoff wait
    t.join(timeout=3.0)
    assert not t.is_alive(), "post loop did not stop"


def _obs_status_counts(repo) -> dict[str, int]:
    with repo.session() as s:
        return dict(s.execute(
            select(Observation.status, func.count()).group_by(Observation.status)).all())


def _attempts(repo) -> list[int]:
    with repo.session() as s:
        return s.execute(select(Observation.attempts)).scalars().all()


def _enqueue_radar(repo, ingestor, seq=1):
    """Ingest one bound radar sample → 4 valid pending Observations."""
    repo.bind(UUID, "patient-9", now_ms=1)
    ingestor.handle(f"rmms/{UUID}/radar", radar_payload(seq=seq))


# ── success drain ───────────────────────────────────────────────────────────
def test_pending_drain_to_posted(repo, ingestor, settings):
    _enqueue_radar(repo, ingestor)
    assert _obs_status_counts(repo) == {STATUS_PENDING: 4}

    client = FakeClient(PostResult(True, False, locations=["Observation/1/_history/1"]))
    _run_until(repo, client, ingestor, settings,
               lambda: _obs_status_counts(repo).get(STATUS_PENDING, 0) == 0)

    assert _obs_status_counts(repo) == {"posted": 4}
    assert len(client.bundles) >= 1
    # all entries are conditional updates (idempotency, §8.6)
    assert all(e["request"]["method"] == "PUT" for e in client.bundles[0]["entry"])


def test_drain_respects_bundle_max_across_batches(repo, ingestor, settings):
    settings.bundle_max = 2                     # force >1 batch for 4 observations
    _enqueue_radar(repo, ingestor)
    client = FakeClient(PostResult(True, False))
    _run_until(repo, client, ingestor, settings,
               lambda: _obs_status_counts(repo).get(STATUS_PENDING, 0) == 0)
    assert _obs_status_counts(repo) == {"posted": 4}
    assert all(len(b["entry"]) <= 2 for b in client.bundles)
    assert len(client.bundles) >= 2


# ── transient failure → stay pending + retry ────────────────────────────────
def test_5xx_keeps_pending_and_increments_attempts(repo, ingestor, settings):
    _enqueue_radar(repo, ingestor)
    client = FakeClient(PostResult(False, retriable=True, error="503: busy"))
    _run_until(repo, client, ingestor, settings, lambda: any(a >= 1 for a in _attempts(repo)))

    # still pending (not dead-lettered) — the outage is recoverable
    assert _obs_status_counts(repo).get(STATUS_PENDING, 0) == 4
    assert _obs_status_counts(repo).get(STATUS_DEAD_LETTER, 0) == 0
    assert max(_attempts(repo)) >= 1


def test_connection_error_keeps_pending(repo, ingestor, settings):
    _enqueue_radar(repo, ingestor)
    client = FakeClient(PostResult(False, retriable=True, error="connection: refused"))
    _run_until(repo, client, ingestor, settings, lambda: any(a >= 1 for a in _attempts(repo)))
    assert _obs_status_counts(repo).get(STATUS_PENDING, 0) == 4


# ── permanent failure → dead-letter (never silently drop) ───────────────────
def test_4xx_dead_letters_the_batch(repo, ingestor, settings):
    _enqueue_radar(repo, ingestor)
    client = FakeClient(PostResult(False, retriable=False, error="400: invalid bundle"))
    _run_until(repo, client, ingestor, settings,
               lambda: _obs_status_counts(repo).get(STATUS_DEAD_LETTER, 0) == 4)

    assert _obs_status_counts(repo) == {STATUS_DEAD_LETTER: 4}
    with repo.session() as s:
        err = s.execute(select(Observation.last_error).limit(1)).scalar_one()
    assert "400" in err


# ── loop integrates redrive-after-bind, then posts ──────────────────────────
def test_loop_redrives_unbuilt_then_posts(repo, ingestor, settings):
    # Sample arrives BEFORE the device is bound: stored, nothing queued yet.
    ingestor.handle(f"rmms/{UUID}/env", env_payload(seq=7))
    assert _obs_status_counts(repo) == {}

    # Bind, then let the loop run: it re-drives the unbuilt sample into
    # Observations and posts them — binding-after-the-fact loses nothing (§12).
    repo.bind(UUID, "patient-9", now_ms=2)
    client = FakeClient(PostResult(True, False))
    _run_until(repo, client, ingestor, settings,
               lambda: _obs_status_counts(repo).get("posted", 0) == 3)
    assert _obs_status_counts(repo) == {"posted": 3}
