# RMMS Aggregator (SBC / Radxa service)

The home-gateway service for the Remote Medical Monitoring System. It runs on the
**Radxa Dragon Q6A** (a **Pine64 ROCKPro64** stands in for the demo), subscribes
to the sensor module's raw MQTT topics on the tablet broker, turns each sample
into a **FHIR R4 `Observation`**, buffers them in SQLite, and POSTs them to the
hospital FHIR endpoint — losing nothing during a cloud outage and never creating
duplicates.

> **This is one project in a monorepo.** It is a standalone Python service; the
> Pico firmware (repo root) never imports it. The firmware rule "no FHIR on the
> MCU" (firmware `CLAUDE.md §9.6`) is upheld — all FHIR/medical code lives here.
> The authoritative spec is [`CLAUDE.md`](CLAUDE.md); the cross-tier contract is
> the firmware repo's `docs/sbc-failover-and-idempotency.md`.

## What it guarantees (the requirements)

- **Cloud-outage failover:** built Observations sit in a SQLite queue
  (`status='pending'`) holding **≥24 h** of outbound data; they POST when the
  endpoint returns. Nothing is silently dropped — failures are retried then
  dead-lettered.
- **Idempotency:** every Observation carries
  `identifier = urn:rmms:seq | <uuid>-<obs_key>-<seq>`; the SBC POSTs with a
  **conditional update** (`PUT ?identifier=…`) so the server **updates-or-ignores**
  rather than duplicating. The firmware's stable, gap-monotonic `seq` (firmware
  ADR-0003) makes this correct across device reboots.
- **Coding:** medical/vital fields use **LOINC**; ambient fields use **LOINC or
  SNOMED CT** (placeholders for not-yet-assigned codes are flagged for clinical
  sign-off). All in `src/rmms_aggregator/fhir/codes.py`.
- **FHIR R4:** every value becomes a validated R4 `Observation`.

## Layout

```
src/rmms_aggregator/
  domain/   sample types, quality, dedup, JSON decode      (pure)
  fhir/     codes, identifiers, mapping, resource_json      (pure)
            builder, client, oauth                          (fhir.resources / httpx)
  storage/  models, repository, migrations (alembic)        (SQLAlchemy)
  mqtt/     client, subscriber (ingest), publisher (time/set)
  health/   /health endpoint (stdlib)
  retry/    backoff + dead-letter policy                    (pure)
  cli/      rmms-provision (device / patient / bind / healthcheck)
  __main__  wiring + post loop
tests/pure/ stdlib unittest — runs with no third-party deps
tests/unit/ pytest — needs the runtime deps (CI / on-device)
```

## Run / test

> **Bring-up & FHIR-format demo:** [`RUNBOOK.md`](RUNBOOK.md) has the full
> copy-paste sequence — install, run the tests, generate example resources, and
> **validate them as standard FHIR R4 (and round-trip through a local HAPI
> server) with no hospital**. Start there.

Quick reference:

```bash
# Pure-logic tests (no dependencies needed):
PYTHONPATH=src python3 -m unittest discover -s tests/pure -t tests/pure

# Full install + tests (needs network for deps):
pip install -e ".[dev]"
RMMS_DB_PATH=./aggregator.db alembic upgrade head   # create the SQLite schema
pytest                                               # pure + dep-bound tests

# Generate + (externally) validate example FHIR R4 resources:
PYTHONPATH=src python3 tools/gen_fhir_examples.py    # → examples/fhir/

# Dev end-to-end with a local HAPI FHIR R4 server:
docker compose -f docker-compose.dev.yml up -d hapi

# Production deploy on the SBC:
sudo deployment/install.sh
```

## Notes / scope

- **FHIR R4 version:** `fhir.resources` is pinned to **7.x** (pydantic v2, to
  match the rest of the stack); its R4-family resources are imported from the
  `fhir.resources.R4B` subpackage (R4B = FHIR 4.3). The emitted JSON validates
  as FHIR R4 (4.0.1) — see `RUNBOOK.md` Part 2. (The 6.x line is 4.0.1 but
  pydantic v1, incompatible here; a strict-4.0.1 build would be a separate ADR.)
- **UI republishing (`CLAUDE.md §11`) is intentionally not implemented.** The
  firmware architecture changed: the mirror now subscribes to the raw
  `rmms/<uuid>/…` topics directly and does its own thresholding (firmware
  `CLAUDE.md §9.5`); the `rmms/ui/…` namespace is retired. Building a UI
  republisher here would contradict that and is out of scope for the
  failover/idempotency work.
- Logging uses the stdlib `logging` module; structlog (spec §3) is the intended
  production JSON logger and can replace it without touching call sites.
- Prometheus metrics (`CLAUDE.md §14.3`) are optional and not built.
