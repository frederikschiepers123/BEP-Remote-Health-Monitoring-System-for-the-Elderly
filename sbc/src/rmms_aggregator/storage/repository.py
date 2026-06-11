"""SQLite persistence layer (CLAUDE.md §10).

The ``observations`` table is the store-and-forward queue that gives the SBC its
**≥24 h tolerance to a hospital/cloud outage**: built Observations sit at
``status='pending'`` and are POSTed when the endpoint is reachable. Nothing is
ever silently dropped — a failure becomes a logged retry or a ``dead_letter`` row.

Ingest idempotency is enforced at two levels: the in-memory dedup
(``domain.dedup``) is the fast path; the ``UNIQUE(device_uuid, sensor, seq)``
constraint here is the durable guard that survives a service restart.

Schema is created by Alembic in production (``alembic upgrade head``); tests call
``Base.metadata.create_all`` against a temp DB.
"""
from __future__ import annotations

from collections.abc import Iterator, Sequence
from contextlib import contextmanager

from sqlalchemy import create_engine, event, select, update
from sqlalchemy.exc import IntegrityError
from sqlalchemy.orm import Session, sessionmaker

from .models import (
    Binding, DeadLetter, Observation, Sample,
    STATUS_DEAD_LETTER, STATUS_PENDING, STATUS_POSTED,
)


class Repository:
    def __init__(self, db_path: str) -> None:
        # check_same_thread=False: the MQTT background callback and the post loop
        # may touch the DB from different threads; sessions are short-lived and
        # not shared, so this is safe with SQLite in WAL mode.
        self._engine = create_engine(
            f"sqlite:///{db_path}", future=True,
            connect_args={"check_same_thread": False},
        )

        @event.listens_for(self._engine, "connect")
        def _pragmas(dbapi_conn, _rec):  # noqa: ANN001
            cur = dbapi_conn.cursor()
            cur.execute("PRAGMA journal_mode=WAL")     # §10.2
            cur.execute("PRAGMA synchronous=NORMAL")
            cur.execute("PRAGMA foreign_keys=ON")
            # Wait (rather than immediately raising OperationalError) when the
            # background ingest thread and the post loop contend for the write
            # lock, so a transient lock never loses a QoS-1 sample.
            cur.execute("PRAGMA busy_timeout=5000")
            cur.close()

        self._Session = sessionmaker(self._engine, expire_on_commit=False, future=True)

    @property
    def engine(self):
        return self._engine

    @contextmanager
    def session(self) -> Iterator[Session]:
        s = self._Session()
        try:
            yield s
            s.commit()
        except Exception:
            s.rollback()
            raise
        finally:
            s.close()

    # ── Ingest ─────────────────────────────────────────────────────────────
    def ingest_sample(
        self, *, device_uuid: str, sensor: str, seq: int, ts_us: int,
        wall_ms: int | None, quality: int, raw_json: str, parsed_json: str,
        created_at: int,
    ) -> int | None:
        """Insert a sample. Returns its row id, or None if it is a duplicate
        (the UNIQUE(uuid, sensor, seq) constraint fired — at-least-once redelivery)."""
        row = Sample(
            device_uuid=device_uuid, sensor=sensor, seq=seq, ts_us=ts_us,
            wall_ms=wall_ms, quality=quality, raw_json=raw_json,
            parsed_json=parsed_json, created_at=created_at,
        )
        try:
            with self.session() as s:
                s.add(row)
                s.flush()
                return row.id
        except IntegrityError:
            return None   # duplicate (uuid, sensor, seq)

    def last_seq(self, device_uuid: str, sensor: str) -> int | None:
        with self.session() as s:
            return s.execute(
                select(Sample.seq)
                .where(Sample.device_uuid == device_uuid, Sample.sensor == sensor)
                .order_by(Sample.seq.desc()).limit(1)
            ).scalar_one_or_none()

    def mark_built(self, sample_ids: Sequence[int]) -> None:
        """Mark samples as built (Observations enqueued, dead-lettered, or
        nothing-to-build) so the re-drive does not reconsider them."""
        if not sample_ids:
            return
        with self.session() as s:
            s.execute(update(Sample).where(Sample.id.in_(sample_ids)).values(built=1))

    def fetch_unbuilt_bound(self, limit: int) -> list[tuple[int, str, str, str]]:
        """Oldest-first (sample_id, device_uuid, sensor, raw_json) for samples
        that are not yet built BUT whose device now has a patient binding — i.e.
        samples that arrived before binding and can now be re-driven (§12)."""
        with self.session() as s:
            rows = s.execute(
                select(Sample.id, Sample.device_uuid, Sample.sensor, Sample.raw_json)
                .join(Binding, Binding.device_uuid == Sample.device_uuid)
                .where(Sample.built == 0)
                .order_by(Sample.id.asc()).limit(limit)
            ).all()
            return [(r[0], r[1], r[2], r[3]) for r in rows]

    def enqueue_observation(self, *, sample_id: int, fhir_identifier: str,
                            fhir_json: str) -> bool:
        """Queue a pending Observation. Returns False if one with this identifier
        already exists (idempotent re-build after a restart)."""
        try:
            with self.session() as s:
                s.add(Observation(
                    sample_id=sample_id, fhir_identifier=fhir_identifier,
                    fhir_json=fhir_json, status=STATUS_PENDING,
                ))
            return True
        except IntegrityError:
            return False

    # ── Outbound queue ─────────────────────────────────────────────────────
    def fetch_pending(self, limit: int) -> list[tuple[int, str, str]]:
        """Oldest-first (id, fhir_identifier, fhir_json) for pending Observations."""
        with self.session() as s:
            rows = s.execute(
                select(Observation.id, Observation.fhir_identifier, Observation.fhir_json)
                .where(Observation.status == STATUS_PENDING)
                .order_by(Observation.id.asc()).limit(limit)
            ).all()
            return [(r[0], r[1], r[2]) for r in rows]

    def mark_posted(self, ids: Sequence[int], server_ids: dict[int, str], now_ms: int) -> None:
        with self.session() as s:
            for oid in ids:
                s.execute(update(Observation).where(Observation.id == oid).values(
                    status=STATUS_POSTED, last_attempt=now_ms,
                    server_id=server_ids.get(oid), attempts=Observation.attempts + 1))

    def record_failure(self, ids: Sequence[int], error: str, now_ms: int,
                       dead_letter: bool) -> None:
        new_status = STATUS_DEAD_LETTER if dead_letter else STATUS_PENDING
        with self.session() as s:
            for oid in ids:
                s.execute(update(Observation).where(Observation.id == oid).values(
                    status=new_status, last_attempt=now_ms, last_error=error[:2000],
                    attempts=Observation.attempts + 1))

    def dead_letter_raw(self, *, topic: str, raw: str, reason: str, now_ms: int) -> None:
        """Persist an undecodable / unbuildable payload (never silently drop)."""
        with self.session() as s:
            s.add(DeadLetter(topic=topic, raw=raw, reason=reason[:2000], created_at=now_ms))

    def dead_letter_exhausted(self, max_attempts: int, now_ms: int) -> int:
        """Move pending Observations that exhausted their retry budget to
        dead_letter so they are not re-fetched forever. Returns rows moved."""
        with self.session() as s:
            res = s.execute(
                update(Observation)
                .where(Observation.status == STATUS_PENDING,
                       Observation.attempts >= max_attempts)
                .values(status=STATUS_DEAD_LETTER, last_attempt=now_ms,
                        last_error="retry budget exhausted"))
            return res.rowcount or 0

    def counts(self) -> tuple[int, int]:
        """(pending, dead_letter) — for the health endpoint."""
        with self.session() as s:
            from sqlalchemy import func
            rows = dict(s.execute(
                select(Observation.status, func.count())
                .group_by(Observation.status)
            ).all())
            return rows.get(STATUS_PENDING, 0), rows.get(STATUS_DEAD_LETTER, 0)

    # ── Retention (§10.4) ──────────────────────────────────────────────────
    def purge_samples_older_than(self, created_before_ms: int) -> int:
        """Delete samples older than the cutoff, EXCEPT any still referenced by a
        retained Observation row (which are kept indefinitely as the audit trail,
        §10.4). Excluding referenced samples avoids a FOREIGN KEY violation
        (observations.sample_id has no ON DELETE). Returns rows removed."""
        from sqlalchemy import delete
        with self.session() as s:
            referenced = select(Observation.sample_id)
            res = s.execute(
                delete(Sample).where(
                    Sample.created_at < created_before_ms,
                    Sample.id.notin_(referenced),
                )
            )
            return res.rowcount or 0

    # ── Device → Patient binding (§12) ─────────────────────────────────────
    def get_patient(self, device_uuid: str) -> str | None:
        with self.session() as s:
            return s.execute(
                select(Binding.patient_id).where(Binding.device_uuid == device_uuid)
            ).scalar_one_or_none()

    def bind(self, device_uuid: str, patient_id: str, now_ms: int) -> None:
        with self.session() as s:
            existing = s.get(Binding, device_uuid)
            if existing:
                existing.patient_id = patient_id
            else:
                s.add(Binding(device_uuid=device_uuid, patient_id=patient_id,
                              created_at=now_ms))
