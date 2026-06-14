#!/usr/bin/env bash
# RMMS aggregator installer (CLAUDE.md §13.3) — the deployment deliverable, and
# the "API interfaces for seamless integration with existing medical
# infrastructure" deliverable (the FHIR R4 REST API is that interface).
#
# Idempotent: re-running updates configuration without clobbering existing values.
# It CONSUMES certs from the project provisioning workstation; it never generates
# them (§13.4). Run as root on the SBC (Ubuntu/Armbian aarch64).
#
# Supported SBCs: Pine64 RockPro64 (RK3399 — the PoC board) and Radxa Dragon Q6A
# (QCS6490). Both are aarch64; the installer is board-agnostic.
#
# Deployment profiles:
#   production (default) — POST FHIR to a real endpoint (RMMS_FHIR_ENDPOINT) with
#                          OAuth (RMMS_FHIR_OAUTH_*). Prompts for missing vars.
#   poc  (--poc | RMMS_DEPLOY_PROFILE=poc) — bring up a LOCAL HAPI FHIR R4 server
#                          (docker-compose.dev.yml) as the "existing medical
#                          infrastructure" the aggregator integrates with: no real
#                          hospital endpoint, no OAuth, non-interactive. Lets you
#                          verify the data is REPRESENTED and HANDLED correctly
#                          against a standards-compliant FHIR API before any real
#                          EHR is involved. This is the PoC path for the RockPro64.
#
# Usage:
#   sudo ./deployment/install.sh            # production
#   sudo ./deployment/install.sh --poc      # PoC: local HAPI FHIR R4 on :8080
set -euo pipefail

PROFILE=production
case "${1:-}" in
    --poc)               PROFILE=poc ;;
    --production|"")     : ;;
    *) echo "unknown arg: $1 (use --poc or --production)" >&2; exit 2 ;;
esac
[ "${RMMS_DEPLOY_PROFILE:-}" = poc ] && PROFILE=poc

RMMS_ETC=/etc/rmms
RMMS_VAR=/var/lib/rmms
RMMS_OPT=/opt/rmms-aggregator
ENV_FILE="$RMMS_ETC/aggregator.env"
SRC_DIR="$(cd "$(dirname "$0")/.." && pwd)"

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
fail() { printf '\033[31mFAILED at: %s\033[0m\n' "$1" >&2; exit 1; }

[ "$(id -u)" = 0 ] || fail "must run as root"
echo "Deployment profile: $PROFILE"

step "1/9 verify platform"
. /etc/os-release 2>/dev/null || true
arch=$(uname -m)
# RockPro64 (RK3399) and Radxa Q6A are both aarch64; the service is board-agnostic.
# Only a non-aarch64 arch is a real concern — the aggregator image and the HAPI
# FHIR image are built/pulled for arm64.
if [ "$arch" != "aarch64" ]; then
    echo "WARNING: expected aarch64 (RockPro64 / Radxa Q6A); got '$arch' on ${ID:-?}."
    echo "Continuing (override) — verify this is intentional."
else
    echo "  aarch64 OK (${PRETTY_NAME:-${ID:-unknown} $arch})"
fi

step "2/9 ensure Docker + compose plugin"
if ! command -v docker >/dev/null 2>&1; then
    curl -fsSL https://get.docker.com | sh || fail "docker install"
fi
docker compose version >/dev/null 2>&1 || fail "docker compose plugin missing"

step "3/9 create $RMMS_ETC"
install -d -m 0700 "$RMMS_ETC" "$RMMS_ETC/certs"
step "4/9 create $RMMS_VAR"
id rmms >/dev/null 2>&1 || useradd --system --home "$RMMS_VAR" --shell /usr/sbin/nologin rmms
install -d -m 0700 -o rmms -g rmms "$RMMS_VAR"

step "5/9 configure environment ($ENV_FILE)"
# In PoC the FHIR "endpoint" is the local HAPI server brought up alongside the
# aggregator by the dev overlay (step 7) — no real hospital server, no OAuth.
fhir_default="https://fhir.example.org/fhir"
[ "$PROFILE" = poc ] && fhir_default="http://localhost:8080/fhir"

# Preserve existing values; only set missing ones (idempotent).
declare -A VARS=(
    [RMMS_BROKER_HOST]="tablet.local"
    [RMMS_BROKER_PORT]="8883"
    [RMMS_BROKER_CA_PATH]="$RMMS_ETC/certs/ca.pem"
    [RMMS_BROKER_CERT_PATH]="$RMMS_ETC/certs/radxa.pem"
    [RMMS_BROKER_KEY_PATH]="$RMMS_ETC/certs/radxa.key"
    [RMMS_FHIR_ENDPOINT]="$fhir_default"
    [RMMS_FHIR_OAUTH_TOKEN_URL]=""
    [RMMS_FHIR_OAUTH_CLIENT_ID]=""
    [RMMS_FHIR_OAUTH_CLIENT_SECRET]=""
    [RMMS_FHIR_OAUTH_SCOPES]="system/Observation.write"
    [RMMS_DB_PATH]="$RMMS_VAR/aggregator.db"
    [RMMS_LOG_LEVEL]="INFO"
    [RMMS_HEALTH_PORT]="9100"
)
touch "$ENV_FILE"; chmod 0600 "$ENV_FILE"
for k in "${!VARS[@]}"; do
    cur=$(grep -E "^${k}=" "$ENV_FILE" | head -1 | cut -d= -f2- || true)
    if [ -z "$cur" ]; then
        if [ "$PROFILE" = poc ]; then
            val="${VARS[$k]}"                 # non-interactive: accept PoC defaults
        else
            read -r -p "  $k [${VARS[$k]}]: " val || true
            val="${val:-${VARS[$k]}}"
        fi
        grep -qE "^${k}=" "$ENV_FILE" && sed -i "s|^${k}=.*|${k}=${val}|" "$ENV_FILE" \
            || echo "${k}=${val}" >> "$ENV_FILE"
    fi
done

step "6/9 install certs"
echo "  Place ca.pem / radxa.pem / radxa.key in $RMMS_ETC/certs (mode 0600)."
chmod -f 0600 "$RMMS_ETC"/certs/* 2>/dev/null || true

step "7/9 install service files"
install -d "$RMMS_OPT"
cp "$SRC_DIR/docker-compose.yml" "$RMMS_OPT/"
cp "$SRC_DIR/Dockerfile" "$RMMS_OPT/" 2>/dev/null || true
cp -r "$SRC_DIR/src" "$SRC_DIR/pyproject.toml" "$SRC_DIR/alembic.ini" "$RMMS_OPT/" 2>/dev/null || true

if [ "$PROFILE" = poc ]; then
    # Stack the dev overlay so EVERY compose command in $RMMS_OPT (the systemd
    # unit's `docker compose up`, ExecStop's `down`, and uninstall) brings up the
    # local HAPI FHIR server with the aggregator. COMPOSE_FILE in the project-dir
    # .env is how `docker compose` discovers both files with no flag changes.
    cp "$SRC_DIR/docker-compose.dev.yml" "$RMMS_OPT/"
    echo "COMPOSE_FILE=docker-compose.yml:docker-compose.dev.yml" > "$RMMS_OPT/.env"
    echo "  PoC: local HAPI FHIR R4 will run on :8080 (the medical-infrastructure API)"
else
    rm -f "$RMMS_OPT/.env" "$RMMS_OPT/docker-compose.dev.yml"
fi

(cd "$RMMS_OPT" && docker compose build) || fail "image build"
cp "$SRC_DIR/deployment/rmms-aggregator.service" /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now rmms-aggregator.service || fail "service start"

step "8/9 health check"
if [ "$PROFILE" = poc ]; then
    echo "  waiting for local HAPI FHIR (JVM start on aarch64 can take 1-3 min)…"
    for _ in $(seq 1 60); do
        curl -fsS "http://localhost:8080/fhir/metadata" >/dev/null 2>&1 && { echo "  HAPI FHIR R4 ready (http://localhost:8080/fhir)"; break; }
        sleep 5
    done
    curl -fsS "http://localhost:8080/fhir/metadata" >/dev/null 2>&1 \
        || echo "  HAPI not ready yet — check: docker logs \$(docker ps -qf name=hapi)"
fi
sleep 5
hport=$(grep -E '^RMMS_HEALTH_PORT=' "$ENV_FILE" | cut -d= -f2)
if curl -fsS "http://localhost:${hport}/health" >/dev/null; then
    echo "  aggregator health OK (http://localhost:${hport}/health)"
else
    echo "  aggregator health not ready yet — check: journalctl -u rmms-aggregator -f"
fi

step "9/9 done"
echo "Deployment successful (profile: $PROFILE)."
echo
echo "Provision a (demo) patient + device, then bind — durable in local SQLite:"
echo "  cd $RMMS_OPT && docker compose run --rm aggregator rmms-provision patient --name 'Demo Patient' --identifier 'urn:rmms:demo|001'"
echo "  cd $RMMS_OPT && docker compose run --rm aggregator rmms-provision device  --uuid <uuid> --model 'RMMS-Sensor-v1'"
echo "  cd $RMMS_OPT && docker compose run --rm aggregator rmms-provision bind    --device <uuid> --patient <fhir-patient-id>"
if [ "$PROFILE" = poc ]; then
cat <<'EOF'

── Verify data representation against the FHIR API (PoC) ─────────────────────
Once a device is publishing, the aggregator translates sensor JSON -> FHIR R4
and POSTs to the local HAPI server. Inspect what reached the medical-infra API
(install jq first: apt-get install -y jq):

  # Latest Observations — code, value+unit, identifier, subject, device:
  curl -s 'http://localhost:8080/fhir/Observation?_sort=-_lastUpdated&_count=10' \
    | jq '.entry[].resource | {code:.code.coding[0], value:.valueQuantity, id:.identifier[0].value, status, subject:.subject.reference, device:.device.reference}'

  # Heart-rate only (LOINC 8867-4) — proves correct coding:
  curl -s 'http://localhost:8080/fhir/Observation?code=http://loinc.org|8867-4' | jq '.total'

  # Provisioned Device + Patient resources:
  curl -s 'http://localhost:8080/fhir/Device'  | jq '.entry[].resource.identifier'
  curl -s 'http://localhost:8080/fhir/Patient' | jq '.entry[].resource.identifier'

  # Idempotency — a resent sample must NOT create a duplicate (expect total: 1):
  curl -s 'http://localhost:8080/fhir/Observation?identifier=urn:rmms:seq|<uuid>-heart-<seq>' | jq '.total'
EOF
fi
