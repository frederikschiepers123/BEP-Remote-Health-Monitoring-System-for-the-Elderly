"""Migrations-from-empty test (CLAUDE.md §10.3: "CI runs migrations from empty
against the test DB on every PR").

Runs the *exact* command the Dockerfile runs at deploy time — ``alembic upgrade
head`` — against a throwaway SQLite file, then asserts the resulting schema
matches the SQLAlchemy models. This is the regression guard for migration drift:
it would have caught the bug where ``0001_initial`` used ``create_all`` against
the live models (recreating the *current* schema, ``built`` included) so that
``0002`` then collided with ``duplicate column name: built`` on a fresh deploy.
"""
from __future__ import annotations

import sqlite3
import subprocess
import sys
from pathlib import Path

import pytest

from rmms_aggregator.storage.models import Base

SBC_ROOT = Path(__file__).resolve().parents[2]   # tests/integration/ → tests/ → sbc/


def _alembic(db_path: Path, *args: str) -> subprocess.CompletedProcess:
    """Invoke alembic the same way production does (cwd = repo root, so the
    relative ``script_location`` in alembic.ini resolves; DB via RMMS_DB_PATH)."""
    return subprocess.run(
        [sys.executable, "-m", "alembic", *args],
        cwd=SBC_ROOT,
        env={"RMMS_DB_PATH": str(db_path), "PATH": "/usr/bin:/bin"},
        capture_output=True, text=True,
    )


def _columns(db_path: Path, table: str) -> set[str]:
    con = sqlite3.connect(db_path)
    try:
        return {r[1] for r in con.execute(f"PRAGMA table_info({table})")}
    finally:
        con.close()


def _tables(db_path: Path) -> set[str]:
    con = sqlite3.connect(db_path)
    try:
        return {r[0] for r in con.execute(
            "SELECT name FROM sqlite_master WHERE type='table'")}
    finally:
        con.close()


@pytest.fixture()
def db(tmp_path) -> Path:
    return tmp_path / "migrate.db"


def test_upgrade_head_from_empty_succeeds(db):
    r = _alembic(db, "upgrade", "head")
    assert r.returncode == 0, f"alembic upgrade head failed:\n{r.stdout}\n{r.stderr}"
    assert db.exists()


def test_migrated_schema_matches_models(db):
    assert _alembic(db, "upgrade", "head").returncode == 0
    tables = _tables(db)
    # Every model table is materialised by the migrations…
    model_tables = {t.name for t in Base.metadata.sorted_tables}
    assert model_tables <= tables, f"missing tables: {model_tables - tables}"
    # …with exactly the columns the models declare (drift guard, incl. samples.built).
    for table in Base.metadata.sorted_tables:
        model_cols = {c.name for c in table.columns}
        assert _columns(db, table.name) == model_cols, table.name


def test_full_downgrade_then_upgrade_roundtrip(db):
    assert _alembic(db, "upgrade", "head").returncode == 0
    r_down = _alembic(db, "downgrade", "base")
    assert r_down.returncode == 0, f"downgrade base failed:\n{r_down.stderr}"
    # only alembic's own bookkeeping table remains after a full downgrade
    assert _tables(db) <= {"alembic_version"}
    # and the chain re-applies cleanly (idempotent from empty)
    assert _alembic(db, "upgrade", "head").returncode == 0
    assert "samples" in _tables(db)
