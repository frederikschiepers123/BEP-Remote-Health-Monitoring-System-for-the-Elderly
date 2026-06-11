"""Add samples.built — re-drive flag for samples stored before a patient binding.

A sample is built=0 until its Observations are enqueued (or it is dead-lettered /
has nothing to build). Samples that arrive before the device→patient binding stay
built=0 and are re-driven once bound, so they are never silently lost.

Revision ID: 0002_sample_built
Revises: 0001_initial
"""
from __future__ import annotations

import sqlalchemy as sa
from alembic import op

revision = "0002_sample_built"
down_revision = "0001_initial"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.add_column(
        "samples",
        sa.Column("built", sa.Integer(), nullable=False, server_default="0"),
    )


def downgrade() -> None:
    op.drop_column("samples", "built")
