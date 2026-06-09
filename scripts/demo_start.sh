#!/usr/bin/env bash
# One-shot demo bring-up on the laptop side.
#
# Usage:
#   scripts/demo_start.sh <tablet-ip> [device-uuid]
#
# What it does (≈20 seconds + one MM² boot):
#   1. Refresh the broker cert + restart Mosquitto for today's tablet IP.
#   2. Start the tablet's mDNS responder so `tablet.local` resolves on the LAN
#      (this is what lets the Pico find the broker by name — see §8.2 / ADR).
#   3. Point the MagicMirror² bridge at the broker IP (the bridge runs in WSL,
#      which has no mDNS resolver, so it still needs the literal IP).
#   4. Refresh the demo device bundle on the Windows side (host-only broker.json)
#      for the *one-time* provisioning; see note below.
#   5. (Re)start MagicMirror², logging to /tmp/mm2.log.
#
# THE PAYOFF (mDNS): broker.json is now host-only (`tablet.local`, empty ip).
# Once the demo Pico has been provisioned with it ONCE, the IP can change every
# session and the Pico still finds the broker by name — NO re-provisioning, NO
# reflash. Each session you just: run this script, power the Pico, open the
# browser. The Windows bundle copy below exists only for the first-ever provision
# (or to recover a wiped device).
#
# NOTE: refresh_broker.sh (step 1) calls provision_ca.sh, which mints a NEW
# random-UUID device bundle every run. We must NOT pick "newest device dir"
# here — that's always the throwaway. We target a fixed demo device (DEMO_UUID,
# overridable as $2); its certs are signed by the same stable CA as the broker
# cert, so mTLS still validates after a broker refresh.

set -euo pipefail

# Fixed demo device — the one provisioned to the bench Pico.
DEMO_UUID_DEFAULT="52295a51-1a2d-4b2f-bedd-dacbbc1685f0"

TIP="${1:-}"
DEMO_UUID="${2:-$DEMO_UUID_DEFAULT}"
if [ -z "$TIP" ]; then
    echo "usage: $0 <tablet-ip> [device-uuid]" >&2
    exit 2
fi

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_DIR"

WIN_HOME="${WIN_HOME:-/mnt/c/Users/frede}"
MM_DIR="$REPO_DIR/MagicMirror"
MM_CONFIG="$MM_DIR/config/config.js"
MM_LOG="${MM_LOG:-/tmp/mm2.log}"
TERMUX_USER="${TERMUX_USER:-u0_a76}"
TERMUX_PORT="${TERMUX_PORT:-8022}"
RMMS_DIR="${RMMS_DIR:-/data/data/com.termux/files/home/rmms}"

step() { printf "\n\033[1;36m[demo] %s\033[0m\n" "$*"; }
ssh_t() { ssh -p "$TERMUX_PORT" -o StrictHostKeyChecking=accept-new -o ConnectTimeout=10 "${TERMUX_USER}@${TIP}" "$@"; }

# ── 1. Broker cert + Mosquitto ───────────────────────────────────────────────
step "refreshing broker cert + restarting Mosquitto for $TIP"
./scripts/refresh_broker.sh "$TIP"

# ── 2. Tablet mDNS responder (so tablet.local resolves on the LAN) ───────────
step "starting tablet mDNS responder (tablet.local)"
scp -P "$TERMUX_PORT" -o StrictHostKeyChecking=accept-new \
    scripts/tablet_mdns_responder.py "${TERMUX_USER}@${TIP}:${RMMS_DIR}/" >/dev/null
# setsid + nohup + python3 -u keeps it alive after this SSH session closes and
# flushes its log immediately. Android suppresses multicast RX with the screen
# off, so keep the tablet awake during the demo.
ssh_t "
  pkill -f tablet_mdns_responder.py 2>/dev/null || true
  sleep 0.5
  cd '$RMMS_DIR'
  setsid nohup python3 -u tablet_mdns_responder.py tablet </dev/null > mdns.log 2>&1 &
  sleep 2
  cat mdns.log
" || echo "[demo] WARNING: responder start failed — Pico will fall back to a literal IP only if broker.json has one"

# ── 2b. Radar-presence → tablet-screen bridge ───────────────────────────────
# Reads rmms/+/radar (mTLS, mirror cert) and drives the HealthMonitorWakeTest
# app's `display` topic (plain :1883): present -> ON, absent -> OFF. Needs the
# mirror cert bundle (provision_ca.sh) + a plain localhost :1883 listener on the
# tablet (the app's IPC channel) + paho-mqtt. All guarded — never aborts the demo.
step "starting radar-presence → screen bridge"
MIR_DIR="$(ls -d out/mirror-* 2>/dev/null | head -1)"
if [ -z "$MIR_DIR" ]; then
    echo "[demo] WARNING: no out/mirror-* cert bundle (run provision_ca.sh) — skipping presence bridge"
else
    ssh_t "mkdir -p '$RMMS_DIR/mirror'" || true
    scp -P "$TERMUX_PORT" -o StrictHostKeyChecking=accept-new \
        "$MIR_DIR/ca.crt" "$MIR_DIR/cert.pem" "$MIR_DIR/key.pem" \
        "${TERMUX_USER}@${TIP}:${RMMS_DIR}/mirror/" >/dev/null 2>&1 || \
        echo "[demo] WARNING: mirror cert scp failed"
    scp -P "$TERMUX_PORT" -o StrictHostKeyChecking=accept-new \
        scripts/tablet_presence_screen.py "${TERMUX_USER}@${TIP}:${RMMS_DIR}/" >/dev/null 2>&1 || true
    ssh_t "
      cd '$RMMS_DIR'
      python3 -c 'import paho.mqtt.client' 2>/dev/null || pip install paho-mqtt >/dev/null 2>&1 || true
      pkill -f tablet_presence_screen.py 2>/dev/null || true
      sleep 0.5
      setsid nohup python3 -u tablet_presence_screen.py --insecure </dev/null > presence.log 2>&1 &
      sleep 2
      cat presence.log
    " || echo "[demo] WARNING: presence bridge start failed (paho-mqtt? mirror cert? :1883 listener?)"
fi

# ── 3. Mirror bridge config (WSL has no mDNS resolver → literal IP) ───────────
step "pointing MagicMirror bridge at mqtts://$TIP:8883"
sed -i -E "s|mqtts://[0-9.]+:8883|mqtts://$TIP:8883|" "$MM_CONFIG"
grep "broker.*mqtts" "$MM_CONFIG"

# ── 4. Demo device bundle — host-only broker.json (one-time provision) ───────
DEVDIR="out/device-$DEMO_UUID"
if [ ! -d "$DEVDIR" ]; then
    echo "[demo] ERROR: demo device bundle not found: $DEVDIR" >&2
    echo "[demo] Provision it once with: DEVICE_UUID=$DEMO_UUID ./scripts/provision_ca.sh <label>" >&2
    exit 1
fi
step "writing host-only broker.json (tablet.local) into $DEVDIR"
cat > "$DEVDIR/broker.json" <<EOF
{"_v":1,"host":"tablet.local","ip":"","port":8883}
EOF
cp -r "$DEVDIR" "$WIN_HOME/"
cp scripts/provision_device.py "$WIN_HOME/"
echo "[demo] copied $DEVDIR + provision_device.py to $WIN_HOME/ (only needed for first-time provision)"

# ── 5. MagicMirror² ──────────────────────────────────────────────────────────
step "(re)starting MagicMirror²"
if PID=$(ss -tlnp 2>/dev/null | awk '/:8080/ {match($0, /pid=([0-9]+)/, m); print m[1]; exit}'); then
    [ -n "$PID" ] && { echo "[demo] killing PID $PID on :8080"; kill "$PID" 2>/dev/null || kill -9 "$PID" 2>/dev/null || true; sleep 1; }
fi
( cd "$MM_DIR" && nohup npm run server > "$MM_LOG" 2>&1 & disown )
sleep 5
echo "[demo] MagicMirror log (first lines):"
grep -E "Ready to go|MQTT|Subscribed|ERROR" "$MM_LOG" | tail -10 || tail -10 "$MM_LOG"

# ── 6. What's left ───────────────────────────────────────────────────────────
cat <<EOM

[demo] laptop side done.

If the demo Pico is ALREADY provisioned with the host-only broker.json
(tablet.local), you are done — just power the Pico and open
http://localhost:8080. It resolves the broker by name; no reflash, no
re-provision, even though the IP changed.

FIRST-TIME ONLY (or after a device wipe), from Windows PowerShell:
    cd C:\\Users\\frede
    # flash bringup_provision.uf2 via BOOTSEL, then:
    py -3.13 provision_device.py COM<N> device-$DEMO_UUID
    # then flash bringup_sensors_slow.uf2

Keep the tablet screen ON (Android suppresses multicast with the screen off).
Watch the bridge connect: tail -f $MM_LOG

EOM
