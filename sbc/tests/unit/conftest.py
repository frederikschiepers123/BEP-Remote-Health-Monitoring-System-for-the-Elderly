"""pytest fixtures for the dependency-bound unit tests.

These require the runtime deps (SQLAlchemy, fhir.resources, httpx, respx) and run
on CI / the Radxa, not in the pip-less dev sandbox. The pure-logic tests live in
tests/pure and need nothing.
"""
import pytest

from rmms_aggregator.storage.models import Base
from rmms_aggregator.storage.repository import Repository


@pytest.fixture()
def repo(tmp_path):
    r = Repository(str(tmp_path / "test.db"))
    Base.metadata.create_all(r.engine)   # test-only; prod uses alembic (§10.3)
    return r
