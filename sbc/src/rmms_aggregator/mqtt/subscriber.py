"""Inbound message handling: decode → dedup → persist → build Observations.

Topic dispatch (CLAUDE.md §6.2): sensor topics are decoded into typed samples,
stored, and turned into queued FHIR Observations; the retained `status` topic
drives the device-online state and (optionally) the time-sync publish.

Every failure path persists a dead-letter row rather than dropping (§6.3/§9.3).
"""
from __future__ import annotations

import dataclasses
import json
import logging
import time
from datetime import datetime, timezone

from ..config import Settings
from ..domain import dedup
from ..domain.decode import SchemaError, decode_sample, parse_topic
from ..domain.quality import Quality
from ..fhir.builder import FhirValidationError, build_and_validate
from ..fhir.mapping import iter_observations
from ..storage.repository import Repository

log = logging.getLogger(__name__)


def _now_ms() -> int:
    return int(time.time() * 1000)


def _record_json(sample) -> str:
    d = dataclasses.asdict(sample)
    d["quality"] = int(sample.quality)   # IntEnum → int for JSON
    return json.dumps(d, separators=(",", ":"))


class Ingestor:
    def __init__(self, repo: Repository, settings: Settings,
                 time_sync_publisher=None) -> None:   # callable(uuid) | None
        self._repo = repo
        self._settings = settings
        self._publish_time = time_sync_publisher
        # in-memory fast-path dedup cursor per (uuid, sensor)
        self._last_seq: dict[tuple[str, str], int] = {}

    def handle(self, topic: str, payload: bytes) -> None:
        try:
            uuid, sensor = parse_topic(topic)
        except SchemaError as exc:
            log.warning("undecodable topic %r: %s", topic, exc)
            self._repo.dead_letter_raw(topic=topic, raw=_safe(payload),
                                       reason=f"topic: {exc}", now_ms=_now_ms())
            return
        if sensor == "status":
            self._on_status(uuid, payload)
            return
        self._on_sample(topic, uuid, sensor, payload)

    # ── status ─────────────────────────────────────────────────────────────
    def _on_status(self, uuid: str, payload: bytes) -> None:
        online = payload.strip() == b"online"
        log.info("device %s %s", uuid, "online" if online else "offline")
        if online and self._publish_time and self._settings.publish_time_sync:
            try:
                self._publish_time(uuid)
            except Exception:
                log.exception("time-sync publish to %s failed", uuid)

    # ── sensor sample ──────────────────────────────────────────────────────
    def _on_sample(self, topic: str, uuid: str, sensor: str, payload: bytes) -> None:
        try:
            sample = decode_sample(uuid, sensor, payload)
        except SchemaError as exc:
            log.warning("schema violation on %s: %s", topic, exc)
            self._repo.dead_letter_raw(topic=topic, raw=_safe(payload),
                                       reason=f"schema: {exc}", now_ms=_now_ms())
            return

        key = (uuid, sensor)
        if key not in self._last_seq:
            # Seed the fast-path cursor from the DB so dedup survives a restart
            # (the durable UNIQUE constraint is the backstop either way).
            self._last_seq[key] = self._repo.last_seq(uuid, sensor)
        if not dedup.should_accept(self._last_seq.get(key), sample.seq):
            return   # duplicate re-send (normal after a firmware/broker reconnect)

        try:
            sid = self._repo.ingest_sample(
                device_uuid=uuid, sensor=sensor, seq=sample.seq, ts_us=sample.ts_us,
                wall_ms=sample.wall_ms, quality=int(sample.quality),
                raw_json=_safe(payload), parsed_json=_record_json(sample),
                created_at=_now_ms(),
            )
        except Exception:
            # A transient persist failure must NOT silently lose a QoS-1 sample.
            log.exception("ingest persist failed on %s", topic)
            self._repo.dead_letter_raw(topic=topic, raw=_safe(payload),
                                       reason="ingest_error", now_ms=_now_ms())
            return
        if sid is None:
            return   # durable dedup (UNIQUE constraint) — already ingested
        self._last_seq[key] = sample.seq

        patient = self._repo.get_patient(uuid)
        if patient is None:
            # No subject reference possible yet. The sample stays stored with
            # built=0 and is re-driven by redrive_unbuilt() once the device is
            # bound (§12) — it is NOT silently dropped.
            log.info("no patient binding for %s; sample stored, will re-drive after bind", uuid)
            return

        self._build_sample(sid, sample, patient, topic, _safe(payload))

    # ── build one stored sample into queued Observations ─────────────────────
    def _build_sample(self, sid: int, sample, patient: str, topic: str, raw: str) -> None:
        """Build + enqueue every Observation for a sample, then mark it built so
        the re-drive won't reconsider it. INVALID quality is dead-lettered (§8.4).
        On an UNEXPECTED error the sample is left unbuilt so the re-drive retries
        it — never silently lost."""
        try:
            if sample.quality == Quality.INVALID:
                self._repo.dead_letter_raw(topic=topic, raw=raw,
                                           reason="quality:invalid", now_ms=_now_ms())
                self._repo.mark_built([sid])
                return
            now = datetime.now(timezone.utc)
            planned = iter_observations(
                sample, now, self._settings.estimated_transport_latency_ms)
            for p in planned:
                try:
                    ident, fhir_json = build_and_validate(p, patient)
                except FhirValidationError as exc:
                    log.error("FHIR build failed for %s: %s", p.identifier_value, exc)
                    self._repo.dead_letter_raw(topic=topic, raw=raw,
                                               reason=f"fhir: {exc}", now_ms=_now_ms())
                    continue
                self._repo.enqueue_observation(sample_id=sid, fhir_identifier=ident,
                                               fhir_json=fhir_json)
            self._repo.mark_built([sid])
        except Exception:
            log.exception("build failed for sample %d; left unbuilt for re-drive", sid)

    def redrive_unbuilt(self, limit: int = 200) -> int:
        """Re-drive samples that were stored before their device→patient binding
        existed (or any still-unbuilt bound sample). Called periodically by the
        post loop so binding-after-the-fact never loses data. Returns count
        processed."""
        rows = self._repo.fetch_unbuilt_bound(limit)
        for sid, uuid, sensor, raw in rows:
            patient = self._repo.get_patient(uuid)
            if patient is None:
                continue   # binding vanished between query and now — leave for next pass
            t = f"rmms/{uuid}/{sensor}"
            try:
                sample = decode_sample(uuid, sensor, raw.encode())
            except SchemaError as exc:
                log.error("re-drive: undecodable stored sample %d: %s", sid, exc)
                self._repo.dead_letter_raw(topic=t, raw=raw,
                                           reason=f"redrive_decode: {exc}", now_ms=_now_ms())
                self._repo.mark_built([sid])
                continue
            self._build_sample(sid, sample, patient, t, raw)
        return len(rows)


def _safe(payload: bytes) -> str:
    return payload.decode("utf-8", errors="replace")
