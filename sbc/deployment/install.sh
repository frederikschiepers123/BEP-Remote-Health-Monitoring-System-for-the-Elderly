#!/usr/bin/env bash
# RMMS aggregator installer (CLAUDE.md §13.3) — the deployment deliverable.
#
# Idempotent: re-running updates configuration without clobbering existing values.
# It CONSUMES certs from the project provisioning workstation; it never generates
# them (§13.4). Run as root on the Radxa (Ubuntu aarch64).
set -euo pipefail

RMMS_ETC=/etc/rmms
RMMS_VAR=/var/lib/rmms
RMMS_OPT=/opt/rmms-aggregator
ENV_FILE="$RMMS_ETC/aggregator.env"
SRC_DIR="$(cd "$(dirname "$0")/.." && pwd)"

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
fail() { printf '\033[31mFAILED at: %s\033[0m\n' "$1" >&2; exit 1; }

[ "$(id -u)" = 0 ] || fail "must run as root"

step "1/9 verify platform"
. /etc/os-release 2>/dev/null || true
arch=$(uname -m)
if [ "${ID:-}" != "ubuntu" ] || [ "$arch" != "aarch64" ]; then
    echo "WARNING: expected Ubuntu aarch64 (Radxa Q6A); got ${ID:-?} $arch."
    echo "Continuing (override) — verify this is intentional."
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
# Preserve existing values; only prompt for missing ones (idempotent).
declare -A VARS=(
    [RMMS_BROKER_HOST]="tablet.local"
    [RMMS_BROKER_PORT]="8883"
    [RMMS_BROKER_CA_PATH]="$RMMS_ETC/certs/ca.pem"
    [RMMS_BROKER_CERT_PATH]="$RMMS_ETC/certs/radxa.pem"
    [RMMS_BROKER_KEY_PATH]="$RMMS_ETC/certs/radxa.key"
    [RMMS_FHIR_ENDPOINT]="https://fhir.example.org/fhir"
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
        read -r -p "  $k [${VARS[$k]}]: " val || true
        val="${val:-${VARS[$k]}}"
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
(cd "$RMMS_OPT" && docker compose build) || fail "image build"
cp "$SRC_DIR/deployment/rmms-aggregator.service" /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now rmms-aggregator.service || fail "service start"

step "8/9 health check"
sleep 5
if curl -fsS "http://localhost:$(grep -E '^RMMS_HEALTH_PORT=' "$ENV_FILE" | cut -d= -f2)/health" >/dev/null; then
    echo "  health endpoint OK"
else
    echo "  health endpoint not ready yet — check: journalctl -u rmms-aggregator -f"
fi

step "9/9 done"
echo "Deployment successful. Next: bind devices with"
echo "  docker compose -f $RMMS_OPT/docker-compose.yml run --rm aggregator rmms-provision bind --device <uuid> --patient <id>"
