"""Storage layer: dedup, the pending→posted/dead-letter queue, retention, binding."""
from rmms_aggregator.retry.deadletter import MAX_ATTEMPTS


def _sample_kw(seq=1, **over):
    kw = dict(device_uuid="u", sensor="env", seq=seq, ts_us=1, wall_ms=None,
              quality=0, raw_json="{}", parsed_json="{}", created_at=1000)
    kw.update(over)
    return kw


def test_ingest_is_idempotent(repo):
    assert repo.ingest_sample(**_sample_kw(seq=1)) is not None
    # same (uuid, sensor, seq) → duplicate rejected (durable dedup)
    assert repo.ingest_sample(**_sample_kw(seq=1)) is None
    # different seq → accepted
    assert repo.ingest_sample(**_sample_kw(seq=2)) is not None


def test_queue_flow_and_counts(repo):
    sid = repo.ingest_sample(**_sample_kw(seq=1))
    assert repo.enqueue_observation(sample_id=sid, fhir_identifier="u-temp-1",
                                    fhir_json='{"resourceType":"Observation"}')
    # duplicate identifier → not re-queued
    assert not repo.enqueue_observation(sample_id=sid, fhir_identifier="u-temp-1",
                                        fhir_json="{}")
    rows = repo.fetch_pending(10)
    assert [r[1] for r in rows] == ["u-temp-1"]
    assert repo.counts() == (1, 0)
    repo.mark_posted([rows[0][0]], {rows[0][0]: "Observation/123"}, now_ms=2000)
    assert repo.counts() == (0, 0)
    assert repo.fetch_pending(10) == []


def test_dead_letter_exhausted(repo):
    sid = repo.ingest_sample(**_sample_kw(seq=1))
    repo.enqueue_observation(sample_id=sid, fhir_identifier="u-temp-1", fhir_json="{}")
    rows = repo.fetch_pending(10)
    ids = [r[0] for r in rows]
    # fail it up to the attempt budget, then sweep
    for _ in range(MAX_ATTEMPTS):
        repo.record_failure(ids, "boom", now_ms=1, dead_letter=False)
    moved = repo.dead_letter_exhausted(MAX_ATTEMPTS, now_ms=2)
    assert moved == 1
    assert repo.counts() == (0, 1)
    assert repo.fetch_pending(10) == []   # dead-lettered rows are not re-fetched


def test_non_retriable_dead_letters_immediately(repo):
    sid = repo.ingest_sample(**_sample_kw(seq=1))
    repo.enqueue_observation(sample_id=sid, fhir_identifier="u-temp-1", fhir_json="{}")
    ids = [r[0] for r in repo.fetch_pending(10)]
    repo.record_failure(ids, "400 bad", now_ms=1, dead_letter=True)
    assert repo.counts() == (0, 1)


def test_binding_roundtrip(repo):
    assert repo.get_patient("u") is None
    repo.bind("u", "patient-9", now_ms=1)
    assert repo.get_patient("u") == "patient-9"
    repo.bind("u", "patient-10", now_ms=2)   # idempotent update
    assert repo.get_patient("u") == "patient-10"


def test_retention_purge(repo):
    repo.ingest_sample(**_sample_kw(seq=1, created_at=1000))
    repo.ingest_sample(**_sample_kw(seq=2, created_at=9000))
    removed = repo.purge_samples_older_than(5000)
    assert removed == 1


def test_purge_keeps_referenced_samples(repo):
    # An old sample still referenced by a retained Observation must NOT be purged
    # (it would violate the FK). Regression for the review's purge finding.
    old = repo.ingest_sample(**_sample_kw(seq=1, created_at=1000))
    repo.enqueue_observation(sample_id=old, fhir_identifier="u-temp-1", fhir_json="{}")
    assert repo.purge_samples_older_than(5000) == 0


def test_unbuilt_redrive_requires_binding(repo):
    # Samples that arrive before a patient binding are stored built=0 and become
    # re-drivable only once bound — never silently lost. Regression for the
    # "no patient binding → silent loss" finding.
    sid = repo.ingest_sample(**_sample_kw(seq=1))
    assert repo.fetch_unbuilt_bound(10) == []        # no binding → not eligible yet
    repo.bind("u", "patient-9", now_ms=1)
    assert [r[0] for r in repo.fetch_unbuilt_bound(10)] == [sid]
    repo.mark_built([sid])
    assert repo.fetch_unbuilt_bound(10) == []        # built → no longer re-driven
