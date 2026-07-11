"""initial schema (samples, observations, dead_letters, bindings)

Revision ID: 0001_initial
Revises:
Create Date: 2026-06-09

The initial migration materialises the schema **as it was at this revision** with
explicit ``op.create_table`` calls. It deliberately does NOT use
``Base.metadata.create_all`` against the live models: the models evolve (e.g.
``samples.built`` was added in 0002), so reflecting them here would make this
migration recreate the *current* schema and collide with the later append-only
migrations — ``alembic upgrade head`` from empty would fail with
``duplicate column name: built``. Migrations are a frozen historical record:
explicit ``op.*`` only (CLAUDE.md §10.3 / §20).
"""
from __future__ import annotations

import sqlalchemy as sa
from alembic import op

revision = "0001_initial"
down_revision = None
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.create_table(
        "samples",
        sa.Column("id", sa.Integer(), primary_key=True, autoincrement=True),
        sa.Column("device_uuid", sa.String(), nullable=False),
        sa.Column("sensor", sa.String(), nullable=False),       # env|air|radar|light
        sa.Column("seq", sa.Integer(), nullable=False),
        sa.Column("ts_us", sa.Integer(), nullable=False),
        sa.Column("wall_ms", sa.Integer(), nullable=True),      # -1 sentinel → NULL
        sa.Column("quality", sa.Integer(), nullable=False),     # 0..3
        sa.Column("raw_json", sa.Text(), nullable=False),
        sa.Column("parsed_json", sa.Text(), nullable=False),
        sa.Column("created_at", sa.Integer(), nullable=False),  # receipt epoch ms
        sa.UniqueConstraint("device_uuid", "sensor", "seq",
                            name="uq_sample_dev_sensor_seq"),
    )
    op.create_index("idx_samples_device_sensor", "samples",
                    ["device_uuid", "sensor"])

    op.create_table(
        "observations",
        sa.Column("id", sa.Integer(), primary_key=True, autoincrement=True),
        sa.Column("sample_id", sa.Integer(),
                  sa.ForeignKey("samples.id"), nullable=False),
        sa.Column("fhir_identifier", sa.String(), nullable=False),
        sa.Column("fhir_json", sa.Text(), nullable=False),
        sa.Column("status", sa.String(), nullable=False),       # pending|posted|dead_letter
        sa.Column("attempts", sa.Integer(), nullable=False, server_default="0"),
        sa.Column("last_attempt", sa.Integer(), nullable=True),
        sa.Column("last_error", sa.Text(), nullable=True),
        sa.Column("server_id", sa.String(), nullable=True),
        sa.UniqueConstraint("fhir_identifier",
                            name="uq_observations_fhir_identifier"),
    )
    op.create_index("idx_obs_status", "observations", ["status"])

    op.create_table(
        "dead_letters",
        sa.Column("id", sa.Integer(), primary_key=True, autoincrement=True),
        sa.Column("topic", sa.String(), nullable=False),
        sa.Column("raw", sa.Text(), nullable=False),
        sa.Column("reason", sa.Text(), nullable=False),
        sa.Column("created_at", sa.Integer(), nullable=False),
    )

    op.create_table(
        "bindings",
        sa.Column("device_uuid", sa.String(), primary_key=True),
        sa.Column("patient_id", sa.String(), nullable=False),
        sa.Column("created_at", sa.Integer(), nullable=False),
    )


def downgrade() -> None:
    op.drop_table("bindings")
    op.drop_table("dead_letters")
    op.drop_index("idx_obs_status", table_name="observations")
    op.drop_table("observations")
    op.drop_index("idx_samples_device_sensor", table_name="samples")
    op.drop_table("samples")
