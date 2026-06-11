"""initial schema (samples, observations, dead_letters, bindings)

Revision ID: 0001_initial
Revises:
Create Date: 2026-06-09

The initial migration materialises the schema directly from the SQLAlchemy
models, so it can never drift from them. Subsequent migrations are append-only
and use explicit ``op.*`` operations (CLAUDE.md §10.3 / §20).
"""
from alembic import op

from rmms_aggregator.storage.models import Base

revision = "0001_initial"
down_revision = None
branch_labels = None
depends_on = None


def upgrade() -> None:
    Base.metadata.create_all(bind=op.get_bind())


def downgrade() -> None:
    Base.metadata.drop_all(bind=op.get_bind())
