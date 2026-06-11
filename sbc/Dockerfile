# RMMS aggregator image (CLAUDE.md §13). Multi-arch (Radxa Q6A = aarch64).
FROM python:3.11-slim

WORKDIR /opt/rmms-aggregator

# curl is used by the compose healthcheck; tini for clean signal handling.
RUN apt-get update && apt-get install -y --no-install-recommends curl tini \
    && rm -rf /var/lib/apt/lists/*

COPY pyproject.toml alembic.ini ./
COPY src ./src
RUN pip install --no-cache-dir .

# Run DB migrations, then the service. (alembic reads RMMS_DB_PATH via env.py.)
ENTRYPOINT ["/usr/bin/tini", "--"]
CMD ["sh", "-c", "alembic upgrade head && rmms-aggregator"]
