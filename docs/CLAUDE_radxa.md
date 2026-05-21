# CLAUDE.md — RMMS Aggregator (Radxa Service)

> Read this file completely before writing or modifying any code in this repository.
> It is the single source of truth for the Radxa service: data flow, FHIR mapping,
> deployment, and operational concerns.

---

## 1. Project context

This repository contains the **aggregation service** for the Remote Medical
Monitoring System (RMMS) — a TU Delft BSc Applied Project (BAP). It runs on a
**Radxa Dragon Q6A** SBC (Qualcomm QCS6490, 8 GB RAM, Ubuntu 24.04 aarch64)
deployed in a patient's home alongside the tablet/firmware stack.

Three-tier system:

```
Sensor Module ──► Tablet ──────────────► Radxa Dragon Q6A ──► Hospital FHIR endpoint
   (firmware     (broker + UI)            (THIS REPO)         (HAPI for dev,
   repo)                                                       real EHR for prod)
```

This service:
1. **Subscribes** to raw sensor JSON topics (`rmms/+/env`, `rmms/+/radar`,
   `rmms/+/light`, `rmms/+/status`) on the tablet's Mosquitto broker over mTLS.
2. **Aggregates and validates** samples, filtering on the quality flag.
3. **Translates** sensor JSON samples into FHIR R4 `Observation` resources
   per the contract in the firmware repo's `CLAUDE.md §9.6`.
4. **Posts** validated Observations (as `Bundle` transactions) to a configured
   FHIR endpoint, authenticating via OAuth 2.0 client credentials
   (SMART-on-FHIR backend services profile).
5. **Republishes** UI-safe presentation topics to `rmms/ui/<uuid>/...` for
   MagicMirror² consumption on the tablet.
6. **Buffers** locally in SQLite for resilience to FHIR-endpoint outages.

This service implements two of the three BAP deliverables:
- "API interfaces for seamless integration with existing medical infrastructure"
  → §8, §9 below.
- "Update deployment script"
  → §13 below.

Note that the original BAP assignment referenced a Pine RockPro64. This was
replaced with the Radxa Dragon Q6A for the 12 TOPS Hexagon NPU (relevant for
future ML workloads; not used in v1 — see §18 non-goals) and mainline Ubuntu
support. The platform substitution does not change this service's behaviour.

The **previous BAP group's SBC software** is in scope as a cautionary tale,
not a starting point. See `docs/technical-audit.md` §D.5 and §D.6 — they had
the MQTT subscriber path working but never persisted, never posted anywhere,
and littered the code with `# persist to database here` and `# post to FHIR
here` comments. This service exists specifically to fulfil what they did not.

---

## 2. Architecture

### 2.1 Data flow

```
                                Tablet (Mosquitto :8883)
                                          │
                                          │ mTLS subscribe
                                          │ rmms/+/env, rmms/+/radar,
                                          │ rmms/+/light, rmms/+/status
                                          ▼
                          ┌──────────────────────────────┐
                          │  MQTT subscriber             │
                          │  paho-mqtt + ECDSA P-256     │
                          └──────────────┬───────────────┘
                                         │ raw JSON bytes (per firmware §9.2)
                                         ▼
                          ┌──────────────────────────────┐
                          │  JSON decoder + domain types │
                          │  json (stdlib) → typed Sample│
                          └──────────────┬───────────────┘
                                         │
                          ┌──────────────┴───────────────┐
                          ▼                              ▼
            ┌──────────────────────────┐   ┌──────────────────────────┐
            │  FHIR builder            │   │  UI publisher            │
            │  Sample → Observation    │   │  Sample → UI topic JSON  │
            │  + LOINC + UCUM          │   │  (qualitative only)      │
            └──────────────┬───────────┘   └──────────────┬───────────┘
                           │                              │ mTLS publish
                           ▼                              ▼
            ┌──────────────────────────┐         rmms/ui/<uuid>/presence
            │  Local validation        │         rmms/ui/<uuid>/wellness
            │  fhir.resources Pydantic │         rmms/ui/<uuid>/ambient
            │  + optional HAPI JAR     │              ▲
            └──────────────┬───────────┘              │
                           │                          │ MagicMirror² subscribes
                  ┌────────┴─────────┐                │
                  ▼                  ▼                │
            valid                invalid              │
              │                    │                  │
              ▼                    ▼                  │
      ┌─────────────────┐   ┌─────────────────┐       │
      │ SQLite buffer   │   │ Dead-letter     │       │
      │ samples table   │   │ store (raw JSON │       │
      │ status: pending │   │ + error)        │       │
      └────────┬────────┘   └─────────────────┘       │
               │                                      │
               ▼                                      │
      ┌─────────────────────────────────┐             │
      │  FHIR client                    │             │
      │  OAuth 2.0 client_credentials   │             │
      │  POST /fhir Bundle (transaction)│             │
      └────────┬────────────────────────┘             │
               │                                      │
        ┌──────┴────────┐                             │
        ▼               ▼                             │
     success         failure                          │
        │               │                             │
        ▼               ▼                             │
    mark sent      retry queue                        │
                   (exp. backoff)                     │
                                                      │
                                                      │
              Tablet Mosquitto ◄────────────── (republished UI topics)
```

### 2.2 Service boundaries

This service **owns**:
- Subscribing to raw sensor topics on the tablet broker.
- JSON decoding and domain validation.
- FHIR R4 Observation construction with proper LOINC/UCUM coding.
- OAuth 2.0 authentication to the hospital FHIR endpoint.
- Local persistence (SQLite) and dead-letter handling.
- Clinical-threshold logic (e.g., "is heart rate in normal range?") — this is
  where it lives, not on the MCU and not in MagicMirror².
- UI-safe topic publication for MagicMirror².

This service **does not own**:
- Sensor sampling, frame parsing, or MQTT publishing of raw data
  (→ firmware repo).
- The MQTT broker itself (→ tablet repo).
- The MagicMirror² front-end (→ tablet repo).
- The hospital's FHIR server itself (HAPI for dev, real EHR for production).

### 2.3 Relationship to firmware repo

The sensor JSON schema in the firmware repo (`CLAUDE.md §9.2`) is the
**authoritative input contract** for this service. The sensor-JSON → FHIR
mapping in the firmware repo (`CLAUDE.md §9.6`) is the **authoritative
output contract**. This document specifies the implementation; the firmware
repo specifies the shape.

If you find a discrepancy between the firmware repo's `§9.2`/`§9.6` and this
service's behaviour, the firmware repo is correct. File an issue, do not
"fix" silently.

---

## 3. Tech stack

| Concern | Choice | Why |
|---|---|---|
| Language | **Python 3.11+** | FHIR libraries are best-in-class in Python; readability matters for a service that has to be auditable |
| MQTT | **paho-mqtt 2.x** | Mature, supports TLS callbacks, well-tested with Mosquitto |
| JSON | **stdlib `json`** | RFC 8259, no dependency. Wire format per firmware §9.2 |
| FHIR | **fhir.resources 7.x** (R4 release) | Pydantic-backed, validates on construction, type-safe |
| HTTP | **httpx 0.27+** | Async-capable, modern, OAuth-friendly |
| OAuth | **authlib 1.x** | Implements RFC 6749 + SMART backend services |
| DB | **SQLAlchemy 2.x** + **SQLite (WAL mode)** | Synchronous mode; service is single-instance per Radxa |
| Migrations | **Alembic** | Standard, no ad-hoc `CREATE IF NOT EXISTS` |
| Config | **pydantic-settings** | Env-var driven, typed, no hand-rolled parsing |
| Logging | **structlog** | Structured JSON to stdout, journald-friendly |
| Metrics | **prometheus-client** | Optional; exposes `/metrics` for monitoring |
| Tests | **pytest** + **pytest-asyncio** + **respx** | Async-aware HTTP mocking |
| Container | **Docker** + **Docker Compose** | Deployment story (§13) |

**Dependency closure rule:** anything beyond this list requires an ADR under
`docs/adr/`. Python's package ecosystem is sprawling; staying disciplined
about what's pulled in matters for auditability and image size.

---

## 4. Repository layout

```
.
├── CLAUDE.md                       # this file
├── README.md                       # human-facing intro
├── pyproject.toml                  # poetry/pdm; no requirements.txt
├── docker-compose.yml              # production stack
├── docker-compose.dev.yml          # adds HAPI FHIR + dev tooling
├── Dockerfile
├── .gitignore                      # see §19 — must block certs/keys/.env
├── .env.example                    # documents required env vars; not real values
│
├── deployment/
│   ├── rmms-aggregator.service     # systemd unit for the host
│   ├── install.sh                  # production installer (the BAP deliverable)
│   ├── uninstall.sh
│   └── mosquitto-client.conf       # template for client connection
│
├── docs/
│   ├── adr/                        # architecture decision records
│   ├── technical-audit.md          # copy of previous BAP audit
│   └── fhir-mapping.md             # human-readable LOINC/UCUM table
│
├── src/rmms_aggregator/
│   ├── __init__.py
│   ├── __main__.py                 # entrypoint: `python -m rmms_aggregator`
│   ├── config.py                   # pydantic-settings env model
│   │
│   ├── mqtt/
│   │   ├── client.py               # paho-mqtt wrapper, mTLS context
│   │   ├── subscriber.py           # topic dispatch, JSON decode
│   │   └── publisher.py            # for UI republishing (§11)
│   │
│   ├── domain/
│   │   ├── sample.py               # typed sensor-sample dataclasses
│   │   ├── quality.py              # q-flag → status mapping
│   │   └── device_patient.py       # device_uuid → patient_id table
│   │
│   ├── fhir/
│   │   ├── codes.py                # LOINC + UCUM lookup tables (§8.2)
│   │   ├── builder.py              # Sample → Observation
│   │   ├── identifiers.py          # idempotency identifier scheme
│   │   ├── validator.py            # local + optional HAPI validation
│   │   ├── client.py               # POST + OAuth (§9)
│   │   └── oauth.py                # token caching, refresh
│   │
│   ├── storage/
│   │   ├── models.py               # SQLAlchemy models (§10)
│   │   ├── repository.py           # queries
│   │   └── migrations/             # Alembic
│   │
│   ├── ui/
│   │   └── publisher.py            # raw vitals → qualitative UI topics
│   │
│   ├── retry/
│   │   └── deadletter.py           # retry queue, exponential backoff
│   │
│   ├── health/
│   │   ├── server.py               # /health and /metrics endpoint
│   │   └── checks.py
│   │
│   └── cli/
│       ├── provision.py            # add Patient/Device FHIR resources
│       ├── redrive.py              # re-post dead-letter entries
│       ├── rotate_cert.py
│       └── inspect.py              # query local SQLite for ops debugging
│
└── tests/
    ├── unit/                       # pure-Python, no I/O
    ├── integration/                # against local Mosquitto + HAPI in docker
    └── fixtures/                   # canned JSON samples, FHIR responses
```

**One module per concern.** Do not create a `utils/` or `helpers/` package.

---

## 5. Configuration

**All configuration via environment variables.** No config files in the repo,
no defaults that point at lab-specific IPs (the audit catches the previous
group hardcoding `192.168.0.101` and `192.168.0.255`). pydantic-settings
loads from environment plus an optional `/etc/rmms/aggregator.env` file in
production.

Required environment variables:

| Variable | Example | Purpose |
|---|---|---|
| `RMMS_BROKER_HOST` | `tablet.local` | Tablet hostname or IP |
| `RMMS_BROKER_PORT` | `8883` | mTLS listener port |
| `RMMS_BROKER_CA_PATH` | `/etc/rmms/certs/ca.der` | Project CA |
| `RMMS_BROKER_CERT_PATH` | `/etc/rmms/certs/radxa.crt` | This service's client cert |
| `RMMS_BROKER_KEY_PATH` | `/etc/rmms/certs/radxa.key` | This service's private key |
| `RMMS_FHIR_ENDPOINT` | `https://fhir.example.org/fhir` | Hospital FHIR base URL |
| `RMMS_FHIR_OAUTH_TOKEN_URL` | `https://auth.example.org/token` | OAuth token endpoint |
| `RMMS_FHIR_OAUTH_CLIENT_ID` | `rmms-aggregator-001` | OAuth client id |
| `RMMS_FHIR_OAUTH_CLIENT_SECRET` | (secret) | OAuth client secret |
| `RMMS_FHIR_OAUTH_SCOPES` | `system/Observation.write` | SMART backend services scopes |
| `RMMS_DB_PATH` | `/var/lib/rmms/aggregator.db` | SQLite location |
| `RMMS_LOG_LEVEL` | `INFO` | One of DEBUG/INFO/WARNING/ERROR |
| `RMMS_HEALTH_PORT` | `9100` | Health/metrics endpoint port |

**Never `print()` an env var.** Logging redacts known-secret keys (`*_SECRET`,
`*_KEY`, `*_TOKEN`) at the structlog processor level.

For the SMART backend services flow specifically, RFC 7521 / SMART app
launch IG `client_credentials` grant with `scope=system/Observation.write
system/Patient.read system/Device.write` is the canonical setup. For HAPI
FHIR in dev, OAuth is optional — `RMMS_FHIR_OAUTH_TOKEN_URL` empty falls back
to no-auth.

---

## 6. MQTT subscriber

### 6.1 Connection
- TLS context built from `RMMS_BROKER_CA_PATH`, `RMMS_BROKER_CERT_PATH`,
  `RMMS_BROKER_KEY_PATH`. Same cert chain as the firmware uses (§10 of
  firmware doc).
- Client ID = `radxa-<hostname>` for log readability. CN in the cert is the
  identity broker validates; client ID is cosmetic.
- Clean session = false; QoS 1 subscriptions; persistent session so missed
  messages while the service was down are queued by the broker (within
  broker limits — Mosquitto default is 100 queued msgs/client, raise to
  10000 in tablet config).
- Reconnect: exponential backoff 1 s → 60 s. **Do not** flood the broker on
  outage (the previous group did; the audit catches it).

### 6.2 Subscriptions

```
rmms/+/env       QoS 1
rmms/+/radar     QoS 1
rmms/+/light     QoS 1
rmms/+/status    QoS 1   (retained — captures device state on (re)connect)
```

The `+` wildcard captures device UUID. Topic-level dispatch in
`subscriber.py` extracts UUID and sensor type into a typed event.

### 6.3 Decoder behaviour

- JSON parse errors → log + dead-letter, do not crash.
- Missing required field → dead-letter with reason `schema_violation`.
- Unknown sensor type (topic) → log warning, dead-letter. Do not silently
  ignore — the firmware should never publish a topic this service does not
  recognise.
- `wall_ms == -1` → treat as "no RTC sync"; resolve via fallback per §8.5,
  force `status: preliminary`. **Do not** treat `-1` as a literal timestamp.
- Per-sample monotonic check: drop samples with `seq` ≤ last seen for that
  `(uuid, sensor)` pair, log as duplicate (likely a retransmission after
  broker reconnect — this is normal, not an error, but the dedup is
  required for FHIR idempotency).

---

## 7. Domain types

Use `dataclasses` (or `pydantic.BaseModel`) with explicit types. No raw
dicts past the JSON decode boundary.

```python
@dataclass(frozen=True, slots=True)
class EnvSample:
    device_uuid: str
    ts_us: int                      # monotonic μs since device boot
    wall_ms: int | None             # may be null until RTC sync
    seq: int
    quality: Quality                # enum: OK, STALE, DEGRADED, INVALID
    temp_c: float
    hum_pct: float
    pres_hpa: float

@dataclass(frozen=True, slots=True)
class RadarSample:
    device_uuid: str
    ts_us: int
    wall_ms: int | None
    seq: int
    quality: Quality
    presence: bool
    distance_mm: int | None
    breath_bpm: float | None
    heart_bpm: float | None

@dataclass(frozen=True, slots=True)
class LightSample:
    device_uuid: str
    ts_us: int
    wall_ms: int | None
    seq: int
    quality: Quality
    lux: float
```

**Never `Dict[str, Any]` past the decode boundary.** Every field has a
documented type. The audit catches the previous group passing raw dicts
through layers and crashing at runtime when keys were missing — typed
domain objects make that a parse-time error.

---

## 8. FHIR mapping

### 8.1 Resource types built

Per sample: one `Observation`. Multiple Observations from one sample (e.g.,
radar produces heart rate + breath rate + presence simultaneously) are
**separate Observations**, not panel members, because the FHIR querying
patterns at hospitals expect this.

Resources this service creates **at runtime**:
- `Observation` — one per measurement, posted as part of a `Bundle`
  transaction.

Resources provisioned **once at deployment** (see §12):
- `Patient` — created by the operator during install.
- `Device` — created by `cli/provision.py` with `identifier.system=urn:rmms:device`,
  `identifier.value=<device_uuid>`.

### 8.2 LOINC and UCUM lookup tables

These live in `fhir/codes.py` as Python dicts, **not** scattered through the
codebase. New sensor fields require updating this file and adding a unit
test.

| Sensor field | LOINC code | LOINC display | UCUM unit | Notes |
|---|---|---|---|---|
| `temp_c` | `8310-5` | Body temperature | `Cel` | Ambient, not body temp — discuss in thesis whether to use environmental code `8328-7` instead |
| `hum_pct` | `19736-7` | Relative humidity in environment | `%` | |
| `pres_hpa` | `3140-1` | Barometric pressure | `kPa` | Convert hPa to kPa (divide by 10) |
| `heart_bpm` | `8867-4` | Heart rate | `/min` | |
| `breath_bpm` | `9279-1` | Respiratory rate | `/min` | |
| `presence` | `76689-9` | Patient presence | (boolean) | Use `valueBoolean`, not `valueQuantity` |
| `distance_mm` | (custom) | RMMS device-to-subject distance | `mm` | No standard LOINC; use `system=urn:rmms:loinc` with custom code |
| `lux` | (custom) | Ambient light level | `lx` | No standard LOINC for ambient illumination |

**LOINC vs SNOMED CT note:** LOINC is correct for *what is measured*; SNOMED
CT codes might be needed for *interpretation* (e.g., "tachycardia"). v1 does
not produce interpretations — the hospital's clinical system does that. Do
not enrich Observations with interpretation codes.

### 8.3 Observation construction

```python
def build_observation(sample: RadarSample, patient_id: str) -> Observation:
    return Observation(
        identifier=[Identifier(
            system="urn:rmms:seq",
            value=f"{sample.device_uuid}-heart-{sample.seq}",
        )],
        status=quality_to_status(sample.quality),    # see §8.4
        category=[CodeableConcept(coding=[Coding(
            system="http://terminology.hl7.org/CodeSystem/observation-category",
            code="vital-signs",
        )])],
        code=CodeableConcept(coding=[Coding(
            system="http://loinc.org",
            code="8867-4",
            display="Heart rate",
        )]),
        subject=Reference(reference=f"Patient/{patient_id}"),
        device=Reference(reference=f"Device/{sample.device_uuid}"),
        effectiveDateTime=resolve_effective_datetime(sample),  # see §8.5
        valueQuantity=Quantity(
            value=sample.heart_bpm,
            unit="/min",
            system="http://unitsofmeasure.org",
            code="/min",
        ),
    )
```

### 8.4 Quality → status mapping

| `quality` enum | FHIR `Observation.status` |
|---|---|
| `OK` (q=0) | `final` |
| `STALE` (q=1) | `preliminary` |
| `DEGRADED` (q=2) | `preliminary` |
| `INVALID` (q=3) | **do not build, dead-letter directly** |

If `wall_ms is None` (no RTC sync yet), force `status=preliminary`
regardless of quality, because the timestamp itself is uncertain. This is a
clinical-safety call — never tell the hospital "this is a final, reliable
reading at time X" when X is a best-guess.

### 8.5 Timestamp resolution

```python
def resolve_effective_datetime(sample) -> datetime:
    if sample.wall_ms is not None:
        return datetime.fromtimestamp(sample.wall_ms / 1000, tz=UTC)
    # Fallback: assume sample arrived "now", subtract estimated transport
    # latency. Mark as preliminary in caller.
    return datetime.now(UTC) - ESTIMATED_TRANSPORT_LATENCY
```

`ESTIMATED_TRANSPORT_LATENCY` is a config value, default 500 ms, tunable
based on observed RTT. Document any change in an ADR.

### 8.6 Identifier scheme (idempotency)

Every Observation carries an `Identifier` with `system=urn:rmms:seq` and
`value=<uuid>-<sensor>-<seq>`. When POSTing, use the FHIR `If-None-Exist`
header with `?identifier=urn:rmms:seq|<uuid>-<sensor>-<seq>` — the server
returns 200 (existing) instead of creating a duplicate.

This handles:
- Network retries after partial failure.
- Service restart re-processing un-acked samples from SQLite.
- Broker redelivery on reconnect.

**Without this, every restart double-posts.** The previous group had no
persistence at all; we have persistence and must handle the re-drive cleanly.

---

## 9. FHIR client

### 9.1 OAuth 2.0 — SMART backend services

The client-credentials grant per RFC 6749 §4.4, with the SMART backend
services profile (`grant_type=client_credentials`, `client_assertion_type`,
JWT-based client assertions for production):

```
POST /token
  grant_type=client_credentials
  client_id=<from env>
  client_secret=<from env>          ← simple client_secret_post for dev/BAP
  scope=system/Observation.write system/Patient.read system/Device.write
```

For production-grade SMART backend services, replace `client_secret_post`
with `client_assertion_type=urn:ietf:params:oauth:client-assertion-type:jwt-bearer`
and a JWT signed by a private key registered with the EHR. **This is the
production path.** For BAP demo against HAPI, basic-auth or `client_secret`
is fine; document the JWT path in the thesis as the production-ready
extension.

**Token caching:** cache the access token until 30 s before its `exp`. Do
not request a fresh token per request. Refresh asynchronously; in-flight
requests use the cached token until rotation.

### 9.2 Posting

- Batch Observations into a **`Bundle` of type `transaction`** with one
  entry per Observation, each using `PUT` (conditional create via
  `If-None-Exist`) for idempotency.
- Batch size: 50 Observations per Bundle, or 5 seconds elapsed, whichever
  comes first.
- Timeout: 30 s per Bundle.
- Retry on `5xx` and connection errors with exponential backoff (1s, 2s,
  4s, 8s, 16s, 32s) and up to 6 attempts; then move to dead-letter.
- Retry on `429` honouring `Retry-After` header.
- **Do not retry** on `4xx` (except `429`) — that's a contract violation
  that won't fix itself; dead-letter directly with the
  `OperationOutcome` returned by the server.

### 9.3 Validation before posting

Three layers, in order:
1. **Construction-time** — `fhir.resources` Pydantic models validate
   cardinality and basic typing automatically when the Observation is
   instantiated.
2. **Structural** — `fhir.resources` `dict()` round-trip catches
   serialization issues.
3. **(Optional) Profile** — run the HAPI FHIR validator JAR against
   `nl-core-Observation` for Dutch hospital integration. This is a build-time
   CI step against canned samples, not a runtime hot-path step.

If validation fails at any layer, the Observation is dead-lettered with the
validation error message. **Never silently drop**. The audit shows what
that pattern produces.

---

## 10. Storage layer

### 10.1 SQLite schema

```sql
-- samples: every received JSON sample, parsed
CREATE TABLE samples (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    device_uuid     TEXT NOT NULL,
    sensor          TEXT NOT NULL,        -- 'env', 'radar', 'light'
    seq             INTEGER NOT NULL,
    ts_us           INTEGER NOT NULL,
    wall_ms         INTEGER,              -- nullable in DB (firmware -1 sentinel becomes NULL on insert)
    quality         INTEGER NOT NULL,     -- 0..3
    raw_json        TEXT NOT NULL,        -- always retained for audit/replay
    parsed_json     TEXT NOT NULL,        -- typed domain object as JSON
    created_at      INTEGER NOT NULL,     -- service-side receipt time
    UNIQUE(device_uuid, sensor, seq)
);

-- observations: built FHIR resources, with sync state
CREATE TABLE observations (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    sample_id       INTEGER NOT NULL REFERENCES samples(id),
    fhir_identifier TEXT NOT NULL UNIQUE,
    fhir_json       TEXT NOT NULL,
    status          TEXT NOT NULL,        -- 'pending', 'posted', 'dead_letter'
    attempts        INTEGER NOT NULL DEFAULT 0,
    last_attempt    INTEGER,
    last_error      TEXT,
    server_id       TEXT                  -- FHIR Observation.id from server, if posted
);

CREATE INDEX idx_obs_status ON observations(status);
CREATE INDEX idx_samples_device_sensor ON samples(device_uuid, sensor);
```

### 10.2 SQLite mode

WAL mode (`PRAGMA journal_mode=WAL`), synchronous=NORMAL (not FULL —
acceptable durability tradeoff for vital-sign data given we also have the
broker's retained delivery on reconnect).

Path: `/var/lib/rmms/aggregator.db`. Permissions 0600, owned by service user.

### 10.3 Migrations

**Alembic, no exceptions.** No `CREATE TABLE IF NOT EXISTS` in application
code. Schema changes go through migration scripts checked into
`storage/migrations/`. CI runs migrations from empty against the test DB
on every PR.

### 10.4 Retention

`samples` table retained 30 days. `observations` table retained indefinitely
(it's the audit trail of what was sent to the hospital). A scheduled vacuum
runs nightly. **Do not implement deletion of `observations` records.** If
storage becomes an issue, that's a future ADR.

---

## 11. UI publisher

### 11.1 Topic schema

This service publishes to `rmms/ui/<uuid>/...` for MagicMirror² consumption.
The schema is JSON, consistent with the firmware → broker leg. MagicMirror²
modules expect JSON; the Radxa is the format-stable layer for both upstream
and downstream consumers.

| Topic | Payload | When published |
|---|---|---|
| `rmms/ui/<uuid>/presence` | `{"present": true, "confidence": "high"\|"medium"\|"low"}` | On radar presence change |
| `rmms/ui/<uuid>/wellness` | `{"status": "ok"\|"check"\|"alert", "since_ms": 12345}` | On clinical-threshold cross |
| `rmms/ui/<uuid>/ambient` | `{"temp": "comfortable"\|"warm"\|"cold", "air": "good"\|"poor"}` | On env change |
| `rmms/ui/<uuid>/connection` | `{"online": true, "last_seen_ms": 12345}` | On status topic change |

**Qualitative, not quantitative.** Per the firmware repo's §9.5: no raw
heart-rate numbers, no exact temperatures, no battery percentages. The
mirror shows ambient signals — "Anna is home and well" — not clinical
dashboards. Numeric vitals go to the hospital, qualitative summaries go to
the mirror.

### 11.2 Threshold logic

Clinical thresholds (resting heart rate range, respiratory rate ranges,
ambient temp comfort zone) live in `ui/publisher.py` as named constants.
Document their source in inline comments — these are clinical decisions,
not arbitrary numbers.

For BAP scope, use thresholds from a published guideline (NHG-Standaard
Hartfalen for cardiac, Dutch comfort temperature standards) and cite them
in the thesis. **Do not invent thresholds.**

---

## 12. Provisioning workflow

### 12.1 What gets provisioned

A new home deployment requires three one-time setup steps:

1. **Create the `Patient` FHIR resource** on the hospital server (or look up
   an existing one). Operator runs `rmms-provision patient --name "Anna de
   Vries" --dob 1942-03-15 --identifier "BSN:123456789"`.
2. **Create the `Device` FHIR resource** for each sensor module, keyed by
   its UUID. Operator runs `rmms-provision device --uuid <uuid> --model
   "RMMS-Sensor-v1"`.
3. **Bind device to patient** in the local SQLite. `rmms-provision bind
   --device <uuid> --patient <fhir-patient-id>`.

These steps are interactive and performed at install time. The output is
durable: subsequent restarts of the service load the binding from SQLite.

### 12.2 The Patient identifier dilemma

For real Dutch hospital integration, the Patient identifier is the **BSN**
(Burgerservicenummer). For BAP demo, use a synthetic identifier
(`urn:rmms:demo|<seq>`) to avoid handling real PII. **Never log a real BSN.**
The logging-redaction processor (§14) treats anything matching the BSN
pattern as a secret.

### 12.3 Device resource shape

```json
{
  "resourceType": "Device",
  "identifier": [{
    "system": "urn:rmms:device",
    "value": "550e8400-e29b-41d4-a716-446655440000"
  }],
  "status": "active",
  "manufacturer": "TU Delft RMMS BAP",
  "deviceName": [{"name": "RMMS Sensor Module v1", "type": "user-friendly-name"}],
  "type": {"coding": [{
    "system": "http://snomed.info/sct",
    "code": "468063009",
    "display": "Vital signs monitor"
  }]}
}
```

---

## 13. Deployment (the BAP deliverable)

### 13.1 Approach

**Docker Compose, not bare shell scripts.** The previous group's Ubuntu
24.04 shell scripts (per the audit) were brittle and required two
Mosquitto configs the README didn't mention. We replace them with a
declarative compose file that brings up the whole stack.

```yaml
# docker-compose.yml (production)
services:
  aggregator:
    image: rmms-aggregator:${VERSION:-latest}
    restart: unless-stopped
    env_file:
      - /etc/rmms/aggregator.env
    volumes:
      - /etc/rmms/certs:/etc/rmms/certs:ro
      - /var/lib/rmms:/var/lib/rmms
    network_mode: host    # to reach tablet.local on the LAN
    healthcheck:
      test: ["CMD", "curl", "-f", "http://localhost:9100/health"]
      interval: 30s
      timeout: 5s
      retries: 3
```

```yaml
# docker-compose.dev.yml (overlay for development)
services:
  hapi:
    image: hapiproject/hapi:v7.4.0
    ports:
      - "8080:8080"
    environment:
      - hapi.fhir.fhir_version=R4
      - hapi.fhir.validation.requests_enabled=true
```

Bring up dev with `docker compose -f docker-compose.yml -f
docker-compose.dev.yml up`.

### 13.2 systemd integration

```ini
# deployment/rmms-aggregator.service
[Unit]
Description=RMMS Aggregator Service
After=docker.service network-online.target
Requires=docker.service
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=/opt/rmms-aggregator
ExecStart=/usr/bin/docker compose up
ExecStop=/usr/bin/docker compose down
Restart=on-failure
RestartSec=10s

[Install]
WantedBy=multi-user.target
```

### 13.3 The deployment script itself

`deployment/install.sh` is the actual BAP deliverable. It must:

1. Verify Ubuntu 24.04 on aarch64 (Radxa Q6A) — exit with clear error on
   anything else.
2. Install Docker + Docker Compose plugin if not present.
3. Create `/etc/rmms/` (mode 0700, owner `rmms`).
4. Create `/var/lib/rmms/` (mode 0700, owner `rmms`).
5. Prompt for the `RMMS_*` env vars and write `/etc/rmms/aggregator.env`
   (mode 0600).
6. Prompt for cert paths and copy certs into `/etc/rmms/certs/` (mode 0600).
7. Install the systemd unit, enable, start.
8. Run `rmms-provision health-check` to verify the service reaches the
   broker, the FHIR endpoint, and the OAuth server.
9. Print a clear "deployment successful" or "deployment failed at step X"
   summary. **Not "if you didn't see any errors, you're probably fine"
   — the previous group's style.**

The script is idempotent: re-running it updates configuration without
re-prompting for values already set.

### 13.4 What `install.sh` does not do

- **Generate certs.** Certs come from the project provisioning workstation
  (firmware repo §10.2). The install script consumes, never produces.
- **Modify the firewall.** The Radxa's network policy is operator-owned.
- **Set up the tablet broker.** That's the tablet repo's deliverable.
- **Touch any hospital FHIR server.** Server-side resources are created
  via the `rmms-provision` CLI, not the install script.

---

## 14. Operational concerns

### 14.1 Logging

structlog → stdout in JSON. journald collects when run under systemd.
**Never log:**
- Secrets (env-var redactor handles known patterns).
- Real BSN-shaped values (regex redactor).
- Raw FHIR Patient resource bodies.
- OAuth tokens (even in DEBUG).

Log levels: DEBUG for development only. INFO for steady state. WARNING for
recoverable issues (e.g., one Bundle failed but retry queued). ERROR for
operator-visible issues (e.g., cannot reach FHIR endpoint for 5 minutes).
**No log levels above ERROR** — there is no CRITICAL or FATAL; severe
issues use ERROR plus a metric.

### 14.2 Health endpoint

`GET /health` returns:
```json
{
  "status": "ok"|"degraded"|"unhealthy",
  "broker": {"connected": true, "last_msg_ms": 12345},
  "fhir": {"reachable": true, "last_post_ms": 12345},
  "oauth": {"token_valid_until_ms": 12345},
  "queue_depth": {"pending": 12, "dead_letter": 0},
  "version": "0.1.0"
}
```

- `ok` — all subsystems green.
- `degraded` — FHIR unreachable but broker fine; we're buffering locally
  (recoverable).
- `unhealthy` — broker disconnected for >2 min OR dead-letter queue
  growing without bound.

systemd's `Restart=on-failure` only fires on process exit. The healthcheck
on the Docker level decides whether the service is restarted on
`unhealthy`. Set `start_period` to 30s to allow startup.

### 14.3 Metrics (optional)

`GET /metrics` exposes Prometheus-format metrics:
- `rmms_samples_received_total{sensor="env|radar|light"}`
- `rmms_observations_built_total{quality="final|preliminary"}`
- `rmms_observations_posted_total{result="success|failure"}`
- `rmms_dead_letter_total{reason="..."}`
- `rmms_fhir_post_duration_seconds` (histogram)
- `rmms_broker_reconnects_total`

Not deployed by default. Useful in a monitoring lab; out of BAP scope
for production.

### 14.4 Graceful shutdown

On SIGTERM:
1. Stop accepting new MQTT messages.
2. Drain in-flight Observations to SQLite (mark as `pending`).
3. Close OAuth session.
4. Close DB connection cleanly.
5. Exit within 30 s (systemd's default `TimeoutStopSec`).

If shutdown exceeds 30 s, the previous group's `kill -9` pattern is what
SQLite WAL mode protects against — but don't rely on it. Profile and fix
slow drain paths.

---

## 15. Testing

### 15.1 Unit
- Pure-Python, no I/O. Cover: JSON decoding, schema validation, FHIR
  builders, identifier generation, quality mapping, timestamp resolution.
- Each LOINC/UCUM mapping has a positive test (correct code emitted) and
  a negative test (no Observation built for unmapped fields).
- Run on every push.

### 15.2 Integration
- Docker-compose-up of Mosquitto + HAPI + this service.
- Mock firmware: a small `pytest` fixture that publishes canned JSON
  samples to the broker.
- Assert: Observations land in HAPI with the expected codes, values, and
  identifiers. Idempotency: publish the same sample twice, verify HAPI
  returns the existing resource.

### 15.3 Soak
- 24 hours sustained, 1 sample/sec across 5 mock devices.
- Memory growth must be < 10 MB over the run. SQLite size growth matches
  expected: ~700 KB/device-hour at 1 Hz env + radar.

### 15.4 Failure injection
- Kill HAPI mid-run, verify samples buffer and dead-letter as designed.
- Kill broker mid-run, verify reconnect with backoff.
- Drop OAuth token, verify automatic refresh.
- Inject malformed JSON, verify dead-letter without crash.

---

## 16. Bring-up order

1. **Local Python env** — `pdm install`, `python -m rmms_aggregator
   --version`.
2. **Config loading** — write minimal `.env`, confirm `pydantic-settings`
   loads without error.
3. **SQLite + migrations** — `alembic upgrade head`, verify schema.
4. **Mock broker test** — local Mosquitto, publish a canned env sample
   manually with `mosquitto_pub`, verify decoded and stored.
5. **Observation builder** — unit test against canned samples, compare
   against golden JSON files.
6. **HAPI FHIR in Docker** — `docker compose -f docker-compose.dev.yml up
   hapi`, post a hand-built Observation, verify creation.
7. **End-to-end mock** — full pipeline: mock-publish JSON → broker →
   subscriber → builder → HAPI. Verify resource in HAPI matches expected
   FHIR JSON.
8. **OAuth integration** — point at SMART sandbox (or a local Keycloak),
   verify token caching and refresh.
9. **Dead-letter** — kill HAPI mid-run, observe queue grow; restart HAPI,
   trigger re-drive, verify backlog clears.
10. **UI publisher** — verify `rmms/ui/<uuid>/*` topics receive expected
    qualitative JSON, MagicMirror² (or `mosquitto_sub`) sees them.
11. **systemd unit** — install the service, verify `systemctl status`,
    `journalctl -u rmms-aggregator -f` shows clean logs.
12. **Provisioning CLI** — exercise `rmms-provision patient`, `device`,
    `bind`, verify FHIR resources created and SQLite binding stored.
13. **48-hour soak** on the Radxa hardware.

---

## 17. Open questions (BLOCKING — resolve before claiming v1)

1. **Hospital FHIR endpoint identity.** Which FHIR server is the BAP
   demonstrating against? HAPI Docker local is fine for code completion;
   for the thesis you want at least one run against a SMART sandbox to
   prove OAuth works end-to-end.
2. **Dutch profile conformance.** Do we apply `nl-core-Observation`
   profile constraints? If yes, run HAPI with the NL package loaded and
   validate. If no, document that as a v1 limitation.
3. **Clinical thresholds source.** Which guideline drives the
   `wellness=ok|check|alert` mapping? NHG-Standaard? An expert from the TU
   Delft medical faculty? **Do not invent thresholds.**
4. **MagicMirror² module choice.** MMM-MQTT direct, or a custom MMM-RMMS
   module wrapping it? See tablet repo's CLAUDE.md for the answer (this
   service must match whatever schema is chosen there).
5. **RTC sync mechanism.** See firmware repo §16 question 6. This service
   must implement the corresponding `rmms/<uuid>/time/set` publisher if
   that path is chosen.
6. **BSN handling for non-demo.** If the BAP demo escalates to a real
   patient pilot, BSN handling requires legal/ethics review. Out of scope
   for v1, but document the boundary in the thesis.

---

## 18. Non-goals (v1)

- ML / NPU usage. The Hexagon NPU is available but no v1 workload uses it.
- Real-time vital sign analysis (rhythm classification, fall detection,
  etc.). The Radxa forwards qualified observations; clinical analysis is
  the hospital's job.
- Multi-tenant deployment. One service instance per home; one home per
  patient.
- Web admin UI. Operations via CLI and journalctl.
- Data export beyond FHIR. No CSV dumps, no Excel reports, no email
  summaries.
- High-availability (clustering, failover). One Radxa per home; if it dies
  the buffered samples sync when it comes back.
- End-user-facing app (this is what the tablet repo + MagicMirror² are
  for).

---

## 19. References

- Firmware repo `CLAUDE.md §9.2` — sensor JSON sample schema (authoritative
  input contract).
- Firmware repo `CLAUDE.md §9.6` — sensor JSON → FHIR mapping (authoritative
  output contract).
- Tablet repo `CLAUDE.md` — Mosquitto config, ACL pattern, MagicMirror².
- `docs/technical-audit.md` — previous BAP audit, especially §D.5 (silent
  failure) and §D.6 (deployment / no persistence).
- FHIR R4: <https://hl7.org/fhir/R4/>.
- SMART Backend Services: <https://hl7.org/fhir/smart-app-launch/backend-services.html>.
- HAPI FHIR: <https://hapifhir.io/>.
- `fhir.resources` library: <https://github.com/nazrulworld/fhir.resources>.
- Nictiz `nl-core` profiles: <https://simplifier.net/packages/nictiz.fhir.nl.r4.nl-core>.
- LOINC: <https://loinc.org>.
- UCUM: <https://ucum.org>.

---

## 20. Instructions for Claude Code specifically

When working in this repository:

- **Always read this file before proposing a change.** Cite the section if
  a proposal conflicts.
- **Do not add dependencies.** The list in §3 is closed; new deps need an
  ADR.
- **Do not invent LOINC codes or UCUM units.** The table in §8.2 is
  authoritative. New sensor fields require updating the table and the
  firmware contract in tandem.
- **Do not silently drop a sample, ever.** Every failure path leads to
  either a logged-and-retried Observation or a dead-letter row. The audit's
  §D.5 catalogues what silent drops produced last time.
- **Do not commit certs, keys, `.env`, or any value matching
  `*_SECRET|*_KEY|*_TOKEN|*_PASSWORD`.** `.gitignore` blocks the obvious
  patterns; pre-commit hook scans for ASN.1/PEM/JWT shapes.
- **Do not modify the wire schema from this side.** If the JSON schema in
  firmware §9.2 needs to change, it changes there first; this service
  follows. Backward-incompatible schema changes coordinate with the
  firmware team via ADR.
- **Do not write FHIR resources by hand-stringing JSON.** Use
  `fhir.resources` constructors so validation happens at build time.
- **Do not bypass OAuth.** Even in dev, a missing token URL should produce
  a clear "running without auth (DEV ONLY)" warning, not a silent skip.
- **Do not invent clinical thresholds.** Cite a guideline.
- **Prefer Bundle transactions over per-resource POSTs.** Network
  round-trips are the main cost.
- **Treat the database migration tree as append-only.** No editing past
  migrations. New issues → new migration.
- When asked to "make it work", first answer: does it match the contract
  in firmware §9.6? If not, the answer is no.

### 20.1 Anti-patterns from the audit (do not reintroduce)

| Previous failure | This repo's rule |
|---|---|
| `# persist to database here` comment with no implementation | Code is the spec, comments are commentary. No `# TODO` comments survive merge. |
| In-memory state only (no DB) | All ingested samples written to SQLite before any further processing. |
| Hardcoded LAN IPs and shared bootstrap creds | All config via env vars. `.env.example` documents shape only. |
| Bare `except Exception: pass` silencing sensor errors | Every exception caught is logged with full context AND either retried, dead-lettered, or re-raised. Never silenced. |
| Unencrypted CA + device keys committed to repo | `.gitignore` blocks `*.key`, `*.pem`, `*.crt`, `*.der`, `*.env`, `creds/`, `certs/`. Pre-commit hook scans content. |
| Two Mosquitto configs required, only one documented | Single `mosquitto-client.conf` template. `install.sh` materialises it from env. |
| `# TODO: real signing` left in production code | Code does what it says. Stubs are explicitly named `*_stub` and never reachable in production builds. |
| Lab-specific subnet (`192.168.0.0/24`) baked in | No IP literals anywhere outside test fixtures. |
| TLS SNI hardcoded to wrong value | SNI from `RMMS_BROKER_HOST` env var. No string literal anywhere. |
| Backdated cert "for testing purposes" left in prod path | Dev and prod use different CAs entirely. Never the same CA on a dev machine. |

Anything matching these is rejected at review, regardless of how convenient
it would be in the short term.
