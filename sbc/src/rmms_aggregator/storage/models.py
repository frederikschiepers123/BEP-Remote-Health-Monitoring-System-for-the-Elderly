"""SQLAlchemy models (CLAUDE.md §10.1).

Two tables:
  - ``samples``       — every received, parsed sensor sample (raw JSON retained).
  - ``observations``  — built FHIR resources with their sync state. This is the
    store-and-forward queue that gives the SBC its ≥24 h cloud-outage tolerance
    (rows sit at status='pending' until POSTed; never silently dropped).
"""
from __future__ import annotations

from sqlalchemy import ForeignKey, Index, Integer, String, Text, UniqueConstraint
from sqlalchemy.orm import DeclarativeBase, Mapped, mapped_column, relationship


class Base(DeclarativeBase):
    pass


class Sample(Base):
    __tablename__ = "samples"

    id: Mapped[int] = mapped_column(primary_key=True, autoincrement=True)
    device_uuid: Mapped[str] = mapped_column(String, nullable=False)
    sensor: Mapped[str] = mapped_column(String, nullable=False)   # env|air|radar|light
    seq: Mapped[int] = mapped_column(Integer, nullable=False)
    ts_us: Mapped[int] = mapped_column(Integer, nullable=False)
    wall_ms: Mapped[int | None] = mapped_column(Integer, nullable=True)  # -1 sentinel → NULL
    quality: Mapped[int] = mapped_column(Integer, nullable=False)        # 0..3
    raw_json: Mapped[str] = mapped_column(Text, nullable=False)
    parsed_json: Mapped[str] = mapped_column(Text, nullable=False)
    created_at: Mapped[int] = mapped_column(Integer, nullable=False)     # receipt epoch ms
    # 0 until the sample's Observations have been built (or it was dead-lettered /
    # had nothing to build). Samples that arrive before a device→patient binding
    # stay built=0 and are re-driven once bound, so they are never silently lost.
    built: Mapped[int] = mapped_column(Integer, nullable=False, default=0)

    observations: Mapped[list["Observation"]] = relationship(back_populates="sample")

    __table_args__ = (
        # The DB-level guard that, together with the in-memory dedup, enforces
        # idempotent ingest: a re-sent (uuid, sensor, seq) is rejected here.
        UniqueConstraint("device_uuid", "sensor", "seq", name="uq_sample_dev_sensor_seq"),
        Index("idx_samples_device_sensor", "device_uuid", "sensor"),
    )


class Observation(Base):
    __tablename__ = "observations"

    id: Mapped[int] = mapped_column(primary_key=True, autoincrement=True)
    sample_id: Mapped[int] = mapped_column(ForeignKey("samples.id"), nullable=False)
    # urn:rmms:seq|<uuid>-<obs_key>-<seq> — the idempotency key (§8.6). UNIQUE so a
    # re-built Observation can never be double-queued.
    fhir_identifier: Mapped[str] = mapped_column(String, nullable=False, unique=True)
    fhir_json: Mapped[str] = mapped_column(Text, nullable=False)
    status: Mapped[str] = mapped_column(String, nullable=False)   # pending|posted|dead_letter
    attempts: Mapped[int] = mapped_column(Integer, nullable=False, default=0)
    last_attempt: Mapped[int | None] = mapped_column(Integer, nullable=True)
    last_error: Mapped[str | None] = mapped_column(Text, nullable=True)
    server_id: Mapped[str | None] = mapped_column(String, nullable=True)

    sample: Mapped["Sample"] = relationship(back_populates="observations")

    __table_args__ = (Index("idx_obs_status", "status"),)


class DeadLetter(Base):
    """A payload that could not be decoded or built into a valid Observation.
    Retained with the raw bytes + reason so it is never silently dropped
    (CLAUDE.md §6.3/§9.3) and can be inspected / re-driven."""
    __tablename__ = "dead_letters"

    id: Mapped[int] = mapped_column(primary_key=True, autoincrement=True)
    topic: Mapped[str] = mapped_column(String, nullable=False)
    raw: Mapped[str] = mapped_column(Text, nullable=False)
    reason: Mapped[str] = mapped_column(Text, nullable=False)
    created_at: Mapped[int] = mapped_column(Integer, nullable=False)


class Binding(Base):
    """device_uuid → hospital FHIR Patient id, established once at install
    (CLAUDE.md §12). Held only here — the patient identity never reaches the MCU."""
    __tablename__ = "bindings"

    device_uuid: Mapped[str] = mapped_column(String, primary_key=True)
    patient_id: Mapped[str] = mapped_column(String, nullable=False)
    created_at: Mapped[int] = mapped_column(Integer, nullable=False)


# Status constants (avoid stringly-typed call sites).
STATUS_PENDING = "pending"
STATUS_POSTED = "posted"
STATUS_DEAD_LETTER = "dead_letter"
