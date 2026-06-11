"""Alembic environment. The SQLite URL comes from RMMS_DB_PATH (no path baked in)."""
from __future__ import annotations

import os

from alembic import context
from sqlalchemy import engine_from_config, pool

from rmms_aggregator.storage.models import Base

config = context.config

_db_path = os.environ.get("RMMS_DB_PATH", "/var/lib/rmms/aggregator.db")
config.set_main_option("sqlalchemy.url", f"sqlite:///{_db_path}")

target_metadata = Base.metadata


def run_migrations_offline() -> None:
    context.configure(
        url=config.get_main_option("sqlalchemy.url"),
        target_metadata=target_metadata,
        literal_binds=True,
        render_as_batch=True,   # SQLite-friendly ALTERs for future migrations
    )
    with context.begin_transaction():
        context.run_migrations()


def run_migrations_online() -> None:
    connectable = engine_from_config(
        config.get_section(config.config_ini_section, {}),
        prefix="sqlalchemy.", poolclass=pool.NullPool,
    )
    with connectable.connect() as connection:
        context.configure(connection=connection, target_metadata=target_metadata,
                          render_as_batch=True)
        with context.begin_transaction():
            context.run_migrations()


if context.is_offline_mode():
    run_migrations_offline()
else:
    run_migrations_online()
