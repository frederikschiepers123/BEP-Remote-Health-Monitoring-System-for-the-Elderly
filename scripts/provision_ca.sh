#!/usr/bin/env bash
# Generates the RMMS project CA (once), a broker server cert+key for Mosquitto,
# a mirror cert (for MagicMirror² to subscribe), an operator cert (for the PoC
# laptop / Radxa relay to publish info+screen), and one device cert+key for a
# single Pico. All ECDSA P-256 per CLAUDE.md §10.2 / §10.2 extended (mirror,
# operator are non-device identities issued by the same CA, see §9.5).
#
# The CA private key NEVER leaves $CA_DIR (default ~/rmms-ca/). CA_DIR is created
# with mode 700 and is .gitignored. Outputs that move off this host (broker cert,
# device cert, mirror bundle, operator bundle, CA *public* cert) land in $OUT_DIR.
#
# Mirror and operator CNs are randomly generated on first run and persisted in
# $CA_DIR/mirror.cn / $CA_DIR/operator.cn. Subsequent runs reuse those CNs.
# Override by deleting the .cn file (then a new random CN is issued on next run).
#
# Per-run usage:
#
#   # First time only (creates CA + broker + mirror + operator + first device):
#   BROKER_IP=192.168.2.21 ./scripts/provision_ca.sh
#
#   # Provision another device (reuses CA + broker + mirror + operator):
#   ./scripts/provision_ca.sh
#
# Outputs:
#   $OUT_DIR/broker/ca.crt | broker.crt | broker.key | mosquitto.conf | acl
#   $OUT_DIR/mirror-<id>/ca.crt | cert.pem | key.pem        (PEM, for the mirror's MMM-CustomMQTTBridge)
#   $OUT_DIR/operator-<id>/ca.crt | cert.pem | key.pem      (PEM, for mosquitto_pub)
#   $OUT_DIR/device-<uuid>/ca.der | dev.crt | dev.key       (DER, for Pico littlefs)
#   $OUT_DIR/device-<uuid>/dev.crt.pem | dev.key.pem        (PEM, for bake_certs.py)
#   $OUT_DIR/device-<uuid>/device.json                      (UUID record)
#
# Override knobs (env vars):
#   RMMS_CA_DIR   default ~/rmms-ca
#   OUT_DIR       default ./out
#   BROKER_HOST   default tablet.local            — added to broker cert SAN
#   BROKER_IP     default 192.168.2.21            — added to broker cert SAN
#   DEVICE_UUID   default $(uuidgen)              — set to re-issue a specific device
#   MIRROR_CN     default mirror-<rand>           — persisted; reused if already set
#   OPERATOR_CN   default operator-<rand>         — persisted; reused if already set
#   CA_DAYS       default 3650 (10 years)         — CA lifetime
#   LEAF_DAYS     default 730  (2 years, §16 Q8)  — leaf cert lifetime

set -euo pipefail

CA_DIR="${RMMS_CA_DIR:-$HOME/rmms-ca}"
OUT_DIR="${OUT_DIR:-./out}"
BROKER_HOST="${BROKER_HOST:-tablet.local}"
BROKER_IP="${BROKER_IP:-192.168.2.21}"
DEVICE_UUID="${DEVICE_UUID:-$(uuidgen | tr 'A-Z' 'a-z')}"
CA_DAYS="${CA_DAYS:-3650}"
LEAF_DAYS="${LEAF_DAYS:-730}"

command -v openssl >/dev/null || { echo "openssl not found"; exit 1; }
command -v uuidgen >/dev/null || { echo "uuidgen not found (apt install uuid-runtime)"; exit 1; }
command -v xxd     >/dev/null || { echo "xxd not found (apt install xxd or vim-common)"; exit 1; }

# ── CA (one-time) ────────────────────────────────────────────────────────────
mkdir -p "$CA_DIR" && chmod 700 "$CA_DIR"
if [ ! -f "$CA_DIR/ca.key" ]; then
    echo "[ca] generating new project CA in $CA_DIR (one-time, ECDSA P-256, $CA_DAYS days)"
    openssl ecparam -name prime256v1 -genkey -noout -out "$CA_DIR/ca.key"
    chmod 600 "$CA_DIR/ca.key"
    openssl req -new -x509 -key "$CA_DIR/ca.key" -days "$CA_DAYS" \
        -subj "/CN=RMMS-Project-CA" \
        -addext "basicConstraints=critical,CA:TRUE,pathlen:0" \
        -addext "keyUsage=critical,keyCertSign,cRLSign" \
        -out "$CA_DIR/ca.crt"
else
    echo "[ca] reusing existing CA at $CA_DIR/ca.crt"
fi

# ── Broker server cert ───────────────────────────────────────────────────────
BROKER_OUT="$OUT_DIR/broker"
mkdir -p "$BROKER_OUT"
echo "[broker] issuing server cert for CN=$BROKER_HOST, IP=$BROKER_IP"
openssl ecparam -name prime256v1 -genkey -noout -out "$BROKER_OUT/broker.key"
chmod 600 "$BROKER_OUT/broker.key"

# SAN must cover the hostname AND the loopback/localhost so every client
# validates the leaf against its connection target:
#   - DNS:tablet.local  — the firmware (dials by mDNS name; IP-independent)
#   - DNS:localhost, IP:127.0.0.1 — the tablet's OWN MagicMirror bridge, which
#     connects to the broker over loopback. These are STABLE, so the bridge
#     keeps validating across DHCP-lease changes (the prior cert only had the
#     then-current LAN IP, so the loopback bridge failed hostname check).
#   - IP:$BROKER_IP — convenience for a laptop connecting by the current LAN IP.
cat > "$BROKER_OUT/broker.ext" <<EOF
basicConstraints=CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:$BROKER_HOST,DNS:localhost,IP:127.0.0.1,IP:$BROKER_IP
EOF

openssl req -new -key "$BROKER_OUT/broker.key" \
    -subj "/CN=$BROKER_HOST" -out "$BROKER_OUT/broker.csr"
openssl x509 -req -in "$BROKER_OUT/broker.csr" \
    -CA "$CA_DIR/ca.crt" -CAkey "$CA_DIR/ca.key" -CAcreateserial \
    -days "$LEAF_DAYS" -extfile "$BROKER_OUT/broker.ext" \
    -out "$BROKER_OUT/broker.crt"
cp "$CA_DIR/ca.crt" "$BROKER_OUT/ca.crt"
rm -f "$BROKER_OUT/broker.csr" "$BROKER_OUT/broker.ext"

# ── Mirror cert (idempotent — generate CN once, reuse the bundle) ────────────
MIRROR_CN_FILE="$CA_DIR/mirror.cn"
if [ -n "${MIRROR_CN:-}" ]; then
    echo "$MIRROR_CN" > "$MIRROR_CN_FILE"
elif [ -f "$MIRROR_CN_FILE" ]; then
    MIRROR_CN=$(cat "$MIRROR_CN_FILE")
else
    MIRROR_CN="mirror-$(head -c 4 /dev/urandom | xxd -p)"
    echo "$MIRROR_CN" > "$MIRROR_CN_FILE"
fi
MIRROR_OUT="$OUT_DIR/$MIRROR_CN"
mkdir -p "$MIRROR_OUT"
if [ ! -f "$MIRROR_OUT/key.pem" ]; then
    echo "[mirror] issuing client cert for CN=$MIRROR_CN"
    openssl ecparam -name prime256v1 -genkey -noout -out "$MIRROR_OUT/key.pem"
    chmod 600 "$MIRROR_OUT/key.pem"
    cat > "$MIRROR_OUT/mirror.ext" <<EOF
basicConstraints=CA:FALSE
keyUsage=critical,digitalSignature
extendedKeyUsage=clientAuth
EOF
    openssl req -new -key "$MIRROR_OUT/key.pem" \
        -subj "/CN=$MIRROR_CN" -out "$MIRROR_OUT/mirror.csr"
    openssl x509 -req -in "$MIRROR_OUT/mirror.csr" \
        -CA "$CA_DIR/ca.crt" -CAkey "$CA_DIR/ca.key" -CAcreateserial \
        -days "$LEAF_DAYS" -extfile "$MIRROR_OUT/mirror.ext" \
        -out "$MIRROR_OUT/cert.pem"
    rm -f "$MIRROR_OUT/mirror.csr" "$MIRROR_OUT/mirror.ext"
    cp "$CA_DIR/ca.crt" "$MIRROR_OUT/ca.crt"
else
    echo "[mirror] reusing existing $MIRROR_CN bundle at $MIRROR_OUT"
fi

# ── Operator cert (idempotent — same pattern as mirror) ──────────────────────
OPERATOR_CN_FILE="$CA_DIR/operator.cn"
if [ -n "${OPERATOR_CN:-}" ]; then
    echo "$OPERATOR_CN" > "$OPERATOR_CN_FILE"
elif [ -f "$OPERATOR_CN_FILE" ]; then
    OPERATOR_CN=$(cat "$OPERATOR_CN_FILE")
else
    OPERATOR_CN="operator-$(head -c 4 /dev/urandom | xxd -p)"
    echo "$OPERATOR_CN" > "$OPERATOR_CN_FILE"
fi
OPERATOR_OUT="$OUT_DIR/$OPERATOR_CN"
mkdir -p "$OPERATOR_OUT"
if [ ! -f "$OPERATOR_OUT/key.pem" ]; then
    echo "[operator] issuing client cert for CN=$OPERATOR_CN"
    openssl ecparam -name prime256v1 -genkey -noout -out "$OPERATOR_OUT/key.pem"
    chmod 600 "$OPERATOR_OUT/key.pem"
    cat > "$OPERATOR_OUT/operator.ext" <<EOF
basicConstraints=CA:FALSE
keyUsage=critical,digitalSignature
extendedKeyUsage=clientAuth
EOF
    openssl req -new -key "$OPERATOR_OUT/key.pem" \
        -subj "/CN=$OPERATOR_CN" -out "$OPERATOR_OUT/operator.csr"
    openssl x509 -req -in "$OPERATOR_OUT/operator.csr" \
        -CA "$CA_DIR/ca.crt" -CAkey "$CA_DIR/ca.key" -CAcreateserial \
        -days "$LEAF_DAYS" -extfile "$OPERATOR_OUT/operator.ext" \
        -out "$OPERATOR_OUT/cert.pem"
    rm -f "$OPERATOR_OUT/operator.csr" "$OPERATOR_OUT/operator.ext"
    cp "$CA_DIR/ca.crt" "$OPERATOR_OUT/ca.crt"
else
    echo "[operator] reusing existing $OPERATOR_CN bundle at $OPERATOR_OUT"
fi

# ── Mosquitto config + ACL ───────────────────────────────────────────────────
# Paths assume the Termux user copies the broker/ directory to ~/rmms/ on the
# tablet (see the install crib).
cat > "$BROKER_OUT/mosquitto.conf" <<'EOF'
# Per-listener auth: each listener below carries its own security settings.
per_listener_settings true

# ── Network listener — the firmware's broker contract (CLAUDE.md §19.1) ───────
# Mutual TLS, cert CN as username, ACL-enforced. The ONLY network-facing
# listener. Firmware, mirror, operator, and the presence bridge's READ side.
listener 8883 0.0.0.0
protocol mqtt
allow_anonymous false
require_certificate true
use_identity_as_username true
cafile   /data/data/com.termux/files/home/rmms/certs/ca.crt
certfile /data/data/com.termux/files/home/rmms/certs/broker.crt
keyfile  /data/data/com.termux/files/home/rmms/certs/broker.key
acl_file /data/data/com.termux/files/home/rmms/acl

# ── Localhost-only plain listener — on-device app IPC ONLY (ADR-0004) ─────────
# Bound to 127.0.0.1, so NOT network-reachable: no firmware traffic ever touches
# it. Carries the HealthMonitorWakeTest `display` topic + the presence bridge's
# WRITE side (see docs/presence_screen_coupling.md). This is a tablet-app IPC
# channel, distinct from — and not a weakening of — the mTLS contract above.
# A *network*-facing plain listener would be a §19.1 violation; this is not one.
listener 1883 127.0.0.1
protocol mqtt
allow_anonymous true

persistence false
log_dest stdout
log_type error
log_type warning
log_type notice
EOF

# ACL uses %u (username via use_identity_as_username = cert CN). More robust
# than %c (which depends on the client setting client_id correctly).
cat > "$BROKER_OUT/acl" <<EOF
# Auto-generated by scripts/provision_ca.sh — do not hand-edit.
# Each cert's CN becomes its MQTT username via use_identity_as_username true.

# ─── Per-device pattern ──────────────────────────────────────────────────────
# Devices (CN = UUID) publish to their own subtree, subscribe to their own /cmd.
# Pattern applies to all users; mirror/operator additionally could write to
# rmms/<their-cn>/* but nobody subscribes there, so it's harmless.
pattern write rmms/%u/#
pattern read  rmms/%u/cmd

# ─── Mirror role ─────────────────────────────────────────────────────────────
# Subscribes to every device's raw topics (env / air / radar / light / status)
# and the downlink info / screen topics, to render the MagicMirror² UI.
# Read-only across the entire device tree.
user $MIRROR_CN
topic read rmms/#

# ─── Operator role ───────────────────────────────────────────────────────────
# Publishes downlink info and screen for any device. PoC: laptop. Production:
# Radxa relay from the hospital. Write-only on these two topic families.
user $OPERATOR_CN
topic write rmms/+/info
topic write rmms/+/screen
EOF

# ── Device cert + DER conversion + device.json ───────────────────────────────
DEV_OUT="$OUT_DIR/device-$DEVICE_UUID"
mkdir -p "$DEV_OUT"
echo "[dev] issuing device cert for UUID=$DEVICE_UUID"

openssl ecparam -name prime256v1 -genkey -noout -out "$DEV_OUT/dev.key.pem"
chmod 600 "$DEV_OUT/dev.key.pem"

cat > "$DEV_OUT/dev.ext" <<EOF
basicConstraints=CA:FALSE
keyUsage=critical,digitalSignature
extendedKeyUsage=clientAuth
subjectAltName=URI:urn:rmms:device:$DEVICE_UUID
EOF

openssl req -new -key "$DEV_OUT/dev.key.pem" \
    -subj "/CN=$DEVICE_UUID" -out "$DEV_OUT/dev.csr"
openssl x509 -req -in "$DEV_OUT/dev.csr" \
    -CA "$CA_DIR/ca.crt" -CAkey "$CA_DIR/ca.key" -CAcreateserial \
    -days "$LEAF_DAYS" -extfile "$DEV_OUT/dev.ext" \
    -out "$DEV_OUT/dev.crt.pem"
rm -f "$DEV_OUT/dev.csr" "$DEV_OUT/dev.ext"

# Pico stores DER (compact, no parser needed for PEM headers in firmware).
openssl x509 -in "$DEV_OUT/dev.crt.pem"   -outform DER -out "$DEV_OUT/dev.crt"
openssl ec   -in "$DEV_OUT/dev.key.pem"   -outform DER -out "$DEV_OUT/dev.key" 2>/dev/null
openssl x509 -in "$CA_DIR/ca.crt"         -outform DER -out "$DEV_OUT/ca.der"
chmod 600 "$DEV_OUT/dev.key"

printf '{"_v":1,"uuid":"%s"}\n' "$DEVICE_UUID" > "$DEV_OUT/device.json"

# ── Summary ──────────────────────────────────────────────────────────────────
echo
echo "Done."
echo
echo "Broker (copy to tablet ~/rmms/ — see install crib):"
ls -la "$BROKER_OUT"
echo
echo "Mirror (copy to MagicMirror²'s MMM-CustomMQTTBridge config):"
ls -la "$MIRROR_OUT"
echo
echo "Operator (PoC: keep on laptop for mosquitto_pub / Radxa publish):"
ls -la "$OPERATOR_OUT"
echo
echo "Device (upload to Pico /certs and /cfg via bringup_provision, or bake into bringup_mqtt):"
ls -la "$DEV_OUT"
echo
echo "Identities:"
echo "  Device UUID:  $DEVICE_UUID"
echo "  Mirror CN:    $MIRROR_CN"
echo "  Operator CN:  $OPERATOR_CN"
echo "  Broker SAN:   DNS:$BROKER_HOST, DNS:localhost, IP:127.0.0.1, IP:$BROKER_IP"
