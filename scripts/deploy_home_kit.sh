#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# deploy_home_kit.sh — one-command, idempotent provisioning of a full RMMS home
# kit (Pico sensor module + Termux/Mosquitto tablet + MagicMirror² + Radxa SBC),
# and a `fleet` mode that loops the whole flow over a manifest for scale.
#
# This is an ORCHESTRATOR. It does not reimplement the per-tier tools — it calls
# the ones already in this repo and only fills the cross-tier gaps:
#   - scripts/provision_ca.sh        → CA + broker + mirror + operator + device certs
#   - scripts/provision_device.py    → write device bundle into the Pico over USB
#   - scripts/tablet_boot.sh         → tablet self-heal autostart (Termux:Boot)
#   - scripts/tablet_*.py/.sh        → mDNS responder, presence bridge, MM launcher
#   - sbc/deployment/install.sh      → Radxa Docker + systemd aggregator install
#
# Gaps this script closes:
#   1. Issues a dedicated Radxa/SBC client identity (CN=radxa-<home>) + ACL rule,
#      and emits the PEM trio install.sh expects (ca.pem / radxa.pem / radxa.key).
#   2. Renders a working MagicMirror² config.js from the home's mirror CN + certs.
#   3. One-time tablet bootstrap (Termux packages + boot symlink).
#   4. A per-home manifest so a kit is reproducible and a fleet is a for-loop.
#
# Security posture (matches CLAUDE.md §10): the CA private key never leaves
# $CA_DIR. Only public certs + leaf bundles move to devices. Nothing secret is
# echoed or committed. FHIR endpoint/OAuth secrets are passed by env, never args.
#
# Usage:
#   scripts/deploy_home_kit.sh certs   <home-id> [device-uuid ...]
#   scripts/deploy_home_kit.sh tablet  <home-id> <tablet-ip> [--bootstrap]
#   scripts/deploy_home_kit.sh pico    <home-id> <device-uuid> <serial-port>
#   scripts/deploy_home_kit.sh sbc     <home-id> <host (user@ip)> [--poc]   # RockPro64/Radxa
#   scripts/deploy_home_kit.sh all     <home-id> <tablet-ip> <sbc-host> <serial-port> [device-uuid]
#   scripts/deploy_home_kit.sh fleet   <manifest.tsv>
#
# PoC (RockPro64, local HAPI FHIR — no real EHR): add --poc to 'sbc', or set
# DEPLOY_PROFILE=poc for 'all'/'fleet'. The SBC then runs a local FHIR R4 API on
# :8080 so you can verify data representation/handling against it. Example:
#   DEPLOY_PROFILE=poc scripts/deploy_home_kit.sh all home-001 192.168.2.21 rock@10.0.0.50 /dev/ttyACM0
#
# Manifest (TSV, '#'-comments allowed):
#   home_id   tablet_ip      sbc_host        serial_port   device_uuid(optional)
#   home-001  192.168.2.21   rock@10.0.0.50  /dev/ttyACM0  52295a51-...-1685f0
#
# Env overrides:
#   RMMS_CA_DIR (~/rmms-ca)  OUT_DIR (./out)
#   BROKER_HOST (tablet.local)  LEAF_DAYS (730, §16-Q8)
#   TERMUX_USER (u0_a76)  TERMUX_PORT (8022)  RMMS_DIR (Termux ~/rmms)
#   TABLET_MM_DIR (Termux ~/MagicMirror)
#   RADXA_SUDO (sudo)  RADXA_TMP (/tmp/rmms-deploy)
#   RMMS_FHIR_ENDPOINT / RMMS_FHIR_OAUTH_*  (seeded into the Radxa env file)
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_DIR"

CA_DIR="${RMMS_CA_DIR:-$HOME/rmms-ca}"
OUT_DIR="${OUT_DIR:-$REPO_DIR/out}"
BROKER_HOST="${BROKER_HOST:-tablet.local}"
LEAF_DAYS="${LEAF_DAYS:-730}"

TERMUX_USER="${TERMUX_USER:-u0_a76}"
TERMUX_PORT="${TERMUX_PORT:-8022}"
RMMS_DIR="${RMMS_DIR:-/data/data/com.termux/files/home/rmms}"
TABLET_MM_DIR="${TABLET_MM_DIR:-/data/data/com.termux/files/home/MagicMirror}"

RADXA_SUDO="${RADXA_SUDO:-sudo}"
RADXA_TMP="${RADXA_TMP:-/tmp/rmms-deploy}"

c() { printf "\n\033[1;36m== %s\033[0m\n" "$*"; }     # phase banner
ok() { printf "  \033[32m✓ %s\033[0m\n" "$*"; }
warn() { printf "  \033[33m! %s\033[0m\n" "$*" >&2; }
die() { printf "\033[31mFAILED: %s\033[0m\n" "$*" >&2; exit 1; }

ssh_t()  { ssh -p "$TERMUX_PORT" -o StrictHostKeyChecking=accept-new -o ConnectTimeout=10 "${TERMUX_USER}@${1}" "${@:2}"; }
scp_t()  { scp -P "$TERMUX_PORT" -o StrictHostKeyChecking=accept-new "${@}"; }

HOME_MANIFEST() { echo "$OUT_DIR/home-$1.env"; }

# ── certs ────────────────────────────────────────────────────────────────────
# Mint everything the home needs from the single project CA. Idempotent: the CA,
# broker, mirror, and operator are reused across runs; only new device UUIDs add
# bundles. Records identities to out/home-<id>.env so later phases are scriptable.
cmd_certs() {
    local home="$1"; shift || true
    [ -n "$home" ] || die "certs: need <home-id>"
    command -v openssl >/dev/null || die "openssl not found"

    c "certs: project CA + broker + mirror + operator ($home)"
    BROKER_HOST="$BROKER_HOST" OUT_DIR="$OUT_DIR" RMMS_CA_DIR="$CA_DIR" LEAF_DAYS="$LEAF_DAYS" \
        ./scripts/provision_ca.sh >/tmp/deploy_ca_$home.log 2>&1 || { cat /tmp/deploy_ca_$home.log; die "provision_ca.sh"; }
    local mirror_cn operator_cn
    mirror_cn="$(cat "$CA_DIR/mirror.cn")"
    operator_cn="$(cat "$CA_DIR/operator.cn")"
    ok "mirror=$mirror_cn  operator=$operator_cn"

    # Device certs: take given UUIDs, else mint one fresh device.
    local uuids=("$@")
    if [ ${#uuids[@]} -eq 0 ]; then
        uuids=("$(uuidgen | tr 'A-Z' 'a-z')")
    fi
    for u in "${uuids[@]}"; do
        if [ ! -f "$OUT_DIR/device-$u/dev.key" ]; then
            c "certs: device $u"
            DEVICE_UUID="$u" BROKER_HOST="$BROKER_HOST" OUT_DIR="$OUT_DIR" \
                RMMS_CA_DIR="$CA_DIR" LEAF_DAYS="$LEAF_DAYS" \
                ./scripts/provision_ca.sh >/tmp/deploy_dev_$u.log 2>&1 || { cat /tmp/deploy_dev_$u.log; die "device cert $u"; }
        fi
        ok "device bundle: $OUT_DIR/device-$u"
    done

    # Radxa/SBC client identity — provision_ca.sh does not issue this. The SBC
    # SUBSCRIBES to the raw device tree and PUBLISHES time/set, so it gets its
    # own CN + a read-tree / write-time/set ACL. PEM trio matches install.sh.
    local radxa_cn="radxa-$home"
    local rx="$OUT_DIR/$radxa_cn"
    if [ ! -f "$rx/radxa.key" ]; then
        c "certs: Radxa client identity ($radxa_cn)"
        mkdir -p "$rx"
        openssl ecparam -name prime256v1 -genkey -noout -out "$rx/radxa.key"
        chmod 600 "$rx/radxa.key"
        cat > "$rx/radxa.ext" <<EOF
basicConstraints=CA:FALSE
keyUsage=critical,digitalSignature
extendedKeyUsage=clientAuth
EOF
        openssl req -new -key "$rx/radxa.key" -subj "/CN=$radxa_cn" -out "$rx/radxa.csr"
        openssl x509 -req -in "$rx/radxa.csr" \
            -CA "$CA_DIR/ca.crt" -CAkey "$CA_DIR/ca.key" -CAcreateserial \
            -days "$LEAF_DAYS" -extfile "$rx/radxa.ext" -out "$rx/radxa.pem"
        rm -f "$rx/radxa.csr" "$rx/radxa.ext"
        cp "$CA_DIR/ca.crt" "$rx/ca.pem"          # PEM CA for Python ssl (§5)
        ok "radxa bundle: $rx (ca.pem / radxa.pem / radxa.key)"
    fi

    # Persist the home manifest (sourced by tablet/radxa/pico phases).
    mkdir -p "$OUT_DIR"
    {
        echo "# RMMS home kit $home — generated identities (no secrets here)"
        echo "HOME_ID=$home"
        echo "MIRROR_CN=$mirror_cn"
        echo "OPERATOR_CN=$operator_cn"
        echo "RADXA_CN=$radxa_cn"
        echo "DEVICE_UUIDS=\"${uuids[*]}\""
    } > "$(HOME_MANIFEST "$home")"
    ok "manifest: $(HOME_MANIFEST "$home")"
}

# Append the radxa role to a broker ACL file (idempotent). Done at push time so
# a later provision_ca.sh / refresh_broker.sh (which overwrites acl) can't drop it.
acl_add_radxa() {
    local acl="$1" radxa_cn="$2"
    grep -q "^user $radxa_cn$" "$acl" 2>/dev/null && return 0
    cat >> "$acl" <<EOF

# ─── Radxa aggregator role (added by deploy_home_kit.sh) ─────────────────────
# Subscribes to the whole device tree for FHIR translation; publishes time/set.
user $radxa_cn
topic read rmms/#
topic write rmms/+/time/set
EOF
}

# ── tablet ───────────────────────────────────────────────────────────────────
cmd_tablet() {
    local home="$1" tip="$2"; local bootstrap="${3:-}"
    [ -n "$home" ] && [ -n "$tip" ] || die "tablet: need <home-id> <tablet-ip>"
    # shellcheck source=/dev/null
    [ -f "$(HOME_MANIFEST "$home")" ] || die "no manifest — run 'certs $home' first"
    . "$(HOME_MANIFEST "$home")"
    local mir="$OUT_DIR/$MIRROR_CN"

    [ "$bootstrap" = "--bootstrap" ] && tablet_bootstrap "$tip"

    c "tablet: pushing broker config + scripts to $TERMUX_USER@$tip:$RMMS_DIR"
    ssh_t "$tip" "mkdir -p '$RMMS_DIR/certs' '$RMMS_DIR/mirror'"

    # Broker bundle. Add the radxa ACL rule before push.
    acl_add_radxa "$OUT_DIR/broker/acl" "$RADXA_CN"
    scp_t "$OUT_DIR/broker/mosquitto.conf" "$OUT_DIR/broker/acl" "${TERMUX_USER}@${tip}:${RMMS_DIR}/"
    scp_t "$OUT_DIR/broker/ca.crt" "$OUT_DIR/broker/broker.crt" "$OUT_DIR/broker/broker.key" \
        "${TERMUX_USER}@${tip}:${RMMS_DIR}/certs/"
    ssh_t "$tip" "chmod 600 '$RMMS_DIR/certs/broker.key'"
    ok "broker config + certs in place"

    # Mirror cert bundle (for MMM-CustomMQTTBridge mTLS) + presence bridge certs.
    scp_t "$mir/ca.crt" "$mir/cert.pem" "$mir/key.pem" "${TERMUX_USER}@${tip}:${RMMS_DIR}/mirror/"
    ok "mirror cert bundle in place"

    # Tablet-side scripts (self-heal autostart + helpers).
    scp_t scripts/tablet_boot.sh scripts/tablet_start_magicmirror.sh \
          scripts/tablet_mdns_responder.py scripts/tablet_presence_screen.py \
          "${TERMUX_USER}@${tip}:${RMMS_DIR}/"
    ssh_t "$tip" "chmod +x '$RMMS_DIR/tablet_boot.sh' '$RMMS_DIR/tablet_start_magicmirror.sh'
                  mkdir -p ~/.termux/boot && ln -sf '$RMMS_DIR/tablet_boot.sh' ~/.termux/boot/10-rmms"
    ok "Termux:Boot symlink (~/.termux/boot/10-rmms) installed"

    # MagicMirror² config — render for this home's mirror CN + local broker.
    render_mm_config "$MIRROR_CN" > "/tmp/config-$home.js"
    ssh_t "$tip" "mkdir -p '$TABLET_MM_DIR/config'"
    scp_t "/tmp/config-$home.js" "${TERMUX_USER}@${tip}:${TABLET_MM_DIR}/config/config.js"
    ok "MagicMirror config.js rendered (clientId=$MIRROR_CN, broker=mqtts://localhost:8883)"

    # Kick the self-heal launcher (idempotent; every service is pgrep-guarded).
    c "tablet: launching services (tablet_boot.sh)"
    ssh_t "$tip" "sh '$RMMS_DIR/tablet_boot.sh'" || warn "tablet_boot returned non-zero (check $RMMS_DIR/boot.log)"
    ssh_t "$tip" "sleep 1; pgrep -f mosquitto >/dev/null && echo 'mosquitto up' || echo 'mosquitto DOWN'"
    ok "tablet tier dispatched — mirror at http://$tip:8080"
}

# One-time Termux package + sshd setup. Pushes a bootstrap snippet and runs it.
tablet_bootstrap() {
    local tip="$1"
    c "tablet: one-time bootstrap (Termux packages) on $tip"
    cat > /tmp/rmms_termux_bootstrap.sh <<'EOF'
#!/data/data/com.termux/files/usr/bin/sh
set -e
yes | pkg update  >/dev/null 2>&1 || true
yes | pkg install -y openssh mosquitto python nodejs-lts termux-api >/dev/null 2>&1 || true
python -c 'import paho.mqtt.client' 2>/dev/null || pip install paho-mqtt zeroconf >/dev/null 2>&1 || true
mkdir -p ~/rmms ~/.termux/boot
echo "bootstrap done: $(command -v mosquitto sshd node python | tr '\n' ' ')"
EOF
    scp_t /tmp/rmms_termux_bootstrap.sh "${TERMUX_USER}@${tip}:${RMMS_DIR%/rmms}/rmms_termux_bootstrap.sh" 2>/dev/null \
        || scp_t /tmp/rmms_termux_bootstrap.sh "${TERMUX_USER}@${tip}:rmms_termux_bootstrap.sh"
    ssh_t "$tip" "sh ~/rmms_termux_bootstrap.sh" || warn "bootstrap had non-fatal errors"
    warn "Manual steps Android can't script (once): install Termux:Boot from F-Droid,"
    warn "disable battery optimization for Termux, install Termux:Boot add-on, unlock once after boot."
}

# Emit a complete MagicMirror² config for a given mirror CN. Broker is reached
# over loopback (tablet runs both broker and mirror); certs live in ~/rmms/mirror.
render_mm_config() {
    local mirror_cn="$1"
    local certdir="$RMMS_DIR/mirror"
    cat <<EOF
/* Generated by deploy_home_kit.sh for $mirror_cn — do not hand-edit; re-render. */
let config = {
  address: "0.0.0.0",
  port: 8080,
  ipWhitelist: [],
  language: "en",
  timeFormat: 24,
  units: "metric",
  modules: [
    { module: "clock", position: "top_left" },
    {
      module: "MMM-CustomMQTTBridge",
      config: {
        broker: "mqtts://localhost:8883",
        clientId: "$mirror_cn",
        caFile:   "$certdir/ca.crt",
        certFile: "$certdir/cert.pem",
        keyFile:  "$certdir/key.pem",
        rejectUnauthorized: true,
        topics: ["rmms/+/+"]
      }
    },
    { module: "MMM-SensorUI", position: "middle_center" },
    {
      module: "MMM-pages",
      config: { modules: [["clock","MMM-SensorUI"]], rotationTime: 0 }
    }
  ]
};
if (typeof module !== "undefined") { module.exports = config; }
EOF
}

# ── pico ─────────────────────────────────────────────────────────────────────
# Provision a Pico over USB-serial. Flash bringup_provision.uf2 FIRST (BOOTSEL),
# then run this, then flash sensor_module.uf2. broker.json is host-only so the
# device finds the broker by mDNS name and never needs re-provisioning on a new
# LAN (see §8.2 / demo_start.sh payoff note).
cmd_pico() {
    local home="$1" uuid="$2" port="$3"
    [ -n "$home" ] && [ -n "$uuid" ] && [ -n "$port" ] || die "pico: need <home-id> <device-uuid> <serial-port>"
    local dev="$OUT_DIR/device-$uuid"
    [ -d "$dev" ] || die "no bundle $dev — run 'certs $home $uuid' first"

    c "pico: writing host-only broker.json into $dev"
    cat > "$dev/broker.json" <<EOF
{"_v":1,"host":"$BROKER_HOST","ip":"","port":8883}
EOF
    # Default sensor config — adjust per board variant (§3.2) if needed.
    [ -f "$dev/sensors.json" ] || \
        echo '{"_v":1,"env":"bme280","light":"bh1750","radar":"bha2"}' > "$dev/sensors.json"

    c "pico: provisioning over $port (verify bringup_provision.uf2 is flashed)"
    python3 scripts/provision_device.py "$port" "$dev" \
        || die "provision_device.py failed (is bringup_provision.uf2 flashed and the port right?)"
    ok "device $uuid provisioned — now flash sensor_module.uf2 (BOOTSEL) to run the app"
}

# ── sbc (RockPro64 / Radxa — board-agnostic) ─────────────────────────────────
# Installs the aggregator on the SBC. Two profiles:
#   default     — production: POST FHIR to a real endpoint (RMMS_FHIR_ENDPOINT).
#   --poc       — bring up a LOCAL HAPI FHIR R4 server on :8080 as the "existing
#                 medical infrastructure" stand-in; no real EHR, no OAuth. This is
#                 the PoC path (RockPro64): validate data representation/handling
#                 against a real FHIR API before any hospital server is involved.
cmd_sbc() {
    local home="$1" host="$2" arg3="${3:-}"
    [ -n "$home" ] && [ -n "$host" ] || die "sbc: need <home-id> <host (user@ip)> [--poc]"
    [ -f "$(HOME_MANIFEST "$home")" ] || die "no manifest — run 'certs $home' first"
    . "$(HOME_MANIFEST "$home")"
    local rx="$OUT_DIR/$RADXA_CN"

    local profile=production
    { [ "$arg3" = "--poc" ] || [ "${DEPLOY_PROFILE:-}" = poc ]; } && profile=poc

    c "sbc: staging certs + sbc/ tree on $host:$RADXA_TMP (profile=$profile)"
    ssh -o StrictHostKeyChecking=accept-new "$host" "mkdir -p '$RADXA_TMP/certs'"
    scp -o StrictHostKeyChecking=accept-new "$rx/ca.pem" "$rx/radxa.pem" "$rx/radxa.key" \
        "$host:$RADXA_TMP/certs/"
    tar -C "$REPO_DIR" -czf /tmp/sbc-$home.tgz sbc
    scp -o StrictHostKeyChecking=accept-new /tmp/sbc-$home.tgz "$host:$RADXA_TMP/sbc.tgz"
    ok "staged"

    # Production seeds a real FHIR endpoint from THIS host's env (never args/manifest).
    # PoC leaves FHIR unset so install.sh --poc wires the local HAPI server itself.
    local fhir_seed="" install_flag=""
    if [ "$profile" = poc ]; then
        install_flag="--poc"
    else
        local fhir_ep="${RMMS_FHIR_ENDPOINT:-https://fhir.example.org/fhir}"
        fhir_seed="
seed RMMS_FHIR_ENDPOINT     \"$fhir_ep\"
seed RMMS_FHIR_OAUTH_TOKEN_URL     \"${RMMS_FHIR_OAUTH_TOKEN_URL:-}\"
seed RMMS_FHIR_OAUTH_CLIENT_ID     \"${RMMS_FHIR_OAUTH_CLIENT_ID:-}\"
seed RMMS_FHIR_OAUTH_CLIENT_SECRET \"${RMMS_FHIR_OAUTH_CLIENT_SECRET:-}\""
    fi

    c "sbc: install (Docker + systemd) on RockPro64/Radxa"
    ssh -o StrictHostKeyChecking=accept-new "$host" "$RADXA_SUDO bash -s" <<EOF
set -e
install -d -m 0700 /etc/rmms /etc/rmms/certs
install -m 0600 '$RADXA_TMP/certs/ca.pem'    /etc/rmms/certs/ca.pem
install -m 0600 '$RADXA_TMP/certs/radxa.pem' /etc/rmms/certs/radxa.pem
install -m 0600 '$RADXA_TMP/certs/radxa.key' /etc/rmms/certs/radxa.key
touch /etc/rmms/aggregator.env; chmod 600 /etc/rmms/aggregator.env
seed() { grep -qE "^\$1=" /etc/rmms/aggregator.env || echo "\$1=\$2" >> /etc/rmms/aggregator.env; }
seed RMMS_BROKER_HOST       "$BROKER_HOST"
seed RMMS_BROKER_PORT       "8883"
seed RMMS_BROKER_CA_PATH    "/etc/rmms/certs/ca.pem"
seed RMMS_BROKER_CERT_PATH  "/etc/rmms/certs/radxa.pem"
seed RMMS_BROKER_KEY_PATH   "/etc/rmms/certs/radxa.key"
seed RMMS_DB_PATH           "/var/lib/rmms/aggregator.db"
seed RMMS_HEALTH_PORT       "9100"$fhir_seed
rm -rf '$RADXA_TMP/sbc' && mkdir -p '$RADXA_TMP/sbc'
tar -C '$RADXA_TMP/sbc' -xzf '$RADXA_TMP/sbc.tgz'
cd '$RADXA_TMP/sbc/sbc'
# install.sh only prompts for MISSING env keys → non-interactive either way.
bash deployment/install.sh $install_flag < /dev/null
EOF
    ok "SBC aggregator installed — health: curl http://<sbc-ip>:9100/health"
    [ "$profile" = poc ] && ok "local HAPI FHIR R4 API at http://<sbc-ip>:8080/fhir (verify data there)"
    warn "Provision + bind each device to a (demo) patient (once):"
    for u in $DEVICE_UUIDS; do
        echo "    ssh $host '$RADXA_SUDO sh -c \"cd /opt/rmms-aggregator && docker compose run --rm aggregator rmms-provision bind --device $u --patient <fhir-patient-id>\"'"
    done
}

# ── all / fleet ──────────────────────────────────────────────────────────────
cmd_all() {
    local home="$1" tip="$2" radxa="$3" port="${4:-}" uuid="${5:-}"
    [ -n "$home" ] && [ -n "$tip" ] && [ -n "$radxa" ] || die "all: need <home-id> <tablet-ip> <radxa-host> [serial-port] [device-uuid]"
    if [ -n "$uuid" ]; then cmd_certs "$home" "$uuid"; else cmd_certs "$home"; fi
    . "$(HOME_MANIFEST "$home")"
    [ -z "$uuid" ] && uuid="$(echo "$DEVICE_UUIDS" | awk '{print $1}')"
    cmd_tablet "$home" "$tip"
    if [ "${DEPLOY_PROFILE:-}" = poc ]; then cmd_sbc "$home" "$radxa" --poc; else cmd_sbc "$home" "$radxa"; fi
    if [ -n "$port" ]; then cmd_pico "$home" "$uuid" "$port"; else
        warn "no serial-port given — skipping Pico provisioning (run 'pico $home $uuid <port>' later)"
    fi
    c "home kit '$home' complete"
}

cmd_fleet() {
    local manifest="$1"
    [ -f "$manifest" ] || die "fleet: manifest not found: $manifest"
    while IFS=$'\t ' read -r home tip radxa port uuid _rest; do
        [ -z "${home:-}" ] && continue
        case "$home" in \#*) continue;; esac
        c "fleet: deploying $home"
        cmd_all "$home" "$tip" "$radxa" "${port:-}" "${uuid:-}" || warn "home $home failed — continuing"
    done < "$manifest"
    ok "fleet run complete"
}

# ── dispatch ─────────────────────────────────────────────────────────────────
sub="${1:-}"; shift || true
case "$sub" in
    certs)        cmd_certs  "$@";;
    tablet)       cmd_tablet "$@";;
    pico)         cmd_pico   "$@";;
    sbc|radxa)    cmd_sbc    "$@";;   # 'radxa' kept as an alias for cmd_sbc
    all)          cmd_all    "$@";;
    fleet)        cmd_fleet  "$@";;
    *) sed -n '2,52p' "$0"; exit 2;;
esac
