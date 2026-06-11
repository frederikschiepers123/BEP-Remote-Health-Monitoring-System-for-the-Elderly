# SBC bring-up runbook

Turnkey steps to install the SBC aggregator, run its tests, and **prove its
output is correct HL7 FHIR R4 — without a hospital server.** Run everything from
the `sbc/` directory.

The scope here is the project goal: *get the data into the correct FHIR API
standard format.* No real hospital/EHR, no OAuth — a local reference FHIR server
(HAPI) stands in for format validation only.

```
cd sbc
```

---

## Prerequisites (Pine64 / Radxa, Ubuntu aarch64)

```bash
sudo apt update && sudo apt install -y python3 python3-venv python3-pip
# Docker is only needed for Part 2 (local HAPI):
#   sudo apt install -y docker.io docker-compose-plugin
python3 --version    # need >= 3.11
```

---

## Part 0 — Zero-install sanity check (works on any machine)

The pure decision logic (FHIR mapping, codes, identifiers, R4 shape) has **no
third-party dependencies**, so you can confirm it before installing anything:

```bash
PYTHONPATH=src python3 -m unittest discover -s tests/pure -t tests/pure
# expect: "OK" (27 tests) — includes the FHIR R4 structural-conformance suite
```

---

## Part 1 — Install + full test suite (proves strict FHIR validation)

This is the first time the real `fhir.resources` library runs — it validates
every Observation/Bundle against FHIR R4 at build time.

```bash
python3 -m venv .venv
. .venv/bin/activate
pip install -e ".[dev]"          # installs the service + pytest/respx

pytest                            # runs tests/pure + tests/unit
# expect: all pass. tests/unit exercises the real fhir.resources builder,
# the SQLAlchemy 24h queue, and the idempotency identifier scheme.
```

---

## Part 2 — Prove the FHIR format (no hospital, no broker, no sensor)

### 2a. Generate example resources from the real pipeline

```bash
PYTHONPATH=src python3 tools/gen_fhir_examples.py
ls examples/fhir/                 # observation_*.json + bundle_transaction.json
```

### 2b. Validate them against the FHIR R4 standard

**Option A — online (quickest):** upload each `examples/fhir/*.json` to
<https://validator.fhir.org> and select **FHIR R4 (4.0.1)**.

**Option B — offline (official HAPI validator JAR):**

```bash
# one-time: download the validator (needs internet once)
curl -L -o validator_cli.jar \
  https://github.com/hapifhir/org.hl7.fhir.core/releases/latest/download/validator_cli.jar

java -jar validator_cli.jar examples/fhir/*.json -version 4.0.1
# expect: "Success" with 0 errors (the resources include narrative + coded UCUM
# units, so the run is also warning-clean).
```

### 2c. Round-trip against a local reference FHIR server (format + idempotency)

This shows a real FHIR server **accepts** the resources and that re-sending does
**not** create duplicates — the whole idempotency story — still no hospital.

```bash
# start a local HAPI FHIR R4 server on :8080
docker compose -f docker-compose.dev.yml up -d hapi
# wait ~30s for it to boot, then:
curl -sf http://localhost:8080/fhir/metadata >/dev/null && echo "HAPI up"

# POST the transaction Bundle (each entry is a conditional update)
curl -sS -X POST http://localhost:8080/fhir \
  -H 'Content-Type: application/fhir+json' \
  --data @examples/fhir/bundle_transaction.json | python3 -m json.tool | head -20

# how many Observations exist now?
curl -sS 'http://localhost:8080/fhir/Observation?_summary=count'

# POST the SAME Bundle again …
curl -sS -X POST http://localhost:8080/fhir \
  -H 'Content-Type: application/fhir+json' \
  --data @examples/fhir/bundle_transaction.json >/dev/null

# … and confirm the count did NOT increase (idempotent: update-or-ignore)
curl -sS 'http://localhost:8080/fhir/Observation?_summary=count'
```

Expected: the two counts are **equal** (e.g. 11 and 11). Inspect a stored
resource: `curl -sS http://localhost:8080/fhir/Observation | python3 -m json.tool`.

That is the deliverable proven end-to-end: sensor-shaped data → standard FHIR R4,
accepted by a FHIR server, no duplicates.

---

## Part 3 — Full live pipeline (when the hardware is ready)

Runs the actual service: tablet broker → SBC → local HAPI. Needs the tablet
Mosquitto broker reachable and the SBC's **PEM** client cert in place.

```bash
cp .env.example .env
# edit .env:
#   RMMS_BROKER_HOST=<tablet host/ip>            RMMS_BROKER_PORT=8883
#   RMMS_BROKER_CA_PATH / CERT_PATH / KEY_PATH   → the SBC's PEM cert + project CA
#   RMMS_FHIR_ENDPOINT=http://localhost:8080/fhir RMMS_FHIR_OAUTH_TOKEN_URL=   (empty = no auth)
#   RMMS_DB_PATH=./aggregator.db                 (a writable dev path)

# create the SQLite schema
RMMS_DB_PATH=./aggregator.db alembic upgrade head

# one-time provisioning so readings get a Patient subject:
rmms-provision device  --uuid <device-uuid>
rmms-provision patient --identifier "urn:rmms:demo|001" --name "Demo Patient"
rmms-provision bind    --device <device-uuid> --patient <fhir-patient-id-from-HAPI>

# run the service (Ctrl-C to stop)
rmms-aggregator
# health: curl -s http://localhost:9100/health | python3 -m json.tool
```

Feed it data either from the **real sensor** (publishing `rmms/<uuid>/...`) or,
for a broker-only test, with `mosquitto_pub` of a canned §9.2 JSON sample to
`rmms/<uuid>/env`. Then confirm the Observations appear in HAPI as in Part 2c.

> Broker note: the SBC's cert must be allowed to **publish** `rmms/+/time/set`
> (so the sensor learns the clock), in addition to its subscribe grant — set in
> the tablet broker ACL.

---

## Part 4 — Production install (later, not for the demo)

`deployment/install.sh` sets the service up under systemd + Docker on the Radxa.
It is the eventual deployment path; not needed for the format demo above.

---

## Troubleshooting

- **`ensurepip` / venv error:** `sudo apt install -y python3-venv`.
- **`pip install` can't reach PyPI:** the device needs internet for this one step
  (or use a wheel cache / `pip download` on another machine).
- **Service exits at startup with a cert error:** the broker cert paths in `.env`
  must point at real **PEM** files (Python `ssl` needs PEM; the firmware uses DER,
  so the provisioning workstation must emit a PEM copy for the SBC).
- **HAPI rejects a resource:** re-run Part 2b to see the exact validator error.
- **Health endpoint not reachable remotely:** it binds to `127.0.0.1` by design;
  curl it from on the device.
```
