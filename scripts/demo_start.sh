#!/usr/bin/env bash
# One-shot demo bring-up on the laptop side.
#
# Usage:
#   scripts/demo_start.sh <tablet-ip> [device-uuid]
#
# What it does (≈15 seconds + one MM² boot):
#   1. Refresh broker cert + restart Mosquitto for today's tablet IP.
#   2. Update MagicMirror² bridge config to point at today's IP.
#   3. Update broker.json in the DEMO DEVICE bundle to today's IP and copy
#      that bundle (plus the provision driver) to /mnt/c/Users/frede/ so
#      PowerShell can immediately re-push to the Pico.
#   4. Kill any process holding :8080 and start MagicMirror² in background,
#      logging to /tmp/mm2.log.
#
# NOTE: refresh_broker.sh (step 1) calls provision_ca.sh, which mints a NEW
# random-UUID device bundle every run. We must NOT pick "newest device dir"
# here — that's always the throwaway. Instead we target a fixed demo device
# (DEMO_UUID below, overridable as $2). Its certs are signed by the same
# stable CA as the broker cert, so mTLS still validates after a broker refresh.
#
# Manual steps that remain:
#   - Flash bringup_provision.uf2 to the Pico (BOOTSEL).
#   - In PowerShell:
#       py -3.13 provision_device.py COM<N> device-<uuid>
#   - Flash bringup_sensors_slow.uf2.
#   - Open http://localhost:8080 in a browser.

set -euo pipefail

# Fixed demo device — the one we actually provision to the bench Pico.
# Override by passing a second arg if the demo board ever changes identity.
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

step() { printf "\n\033[1;36m[demo] %s\033[0m\n" "$*"; }

# ── 1. Broker ────────────────────────────────────────────────────────────────
step "refreshing broker cert for $TIP"
./scripts/refresh_broker.sh "$TIP"

# ── 2. Mirror config ─────────────────────────────────────────────────────────
step "pointing MagicMirror bridge at mqtts://$TIP:8883"
sed -i -E "s|mqtts://[0-9.]+:8883|mqtts://$TIP:8883|" "$MM_CONFIG"
grep "broker.*mqtts" "$MM_CONFIG"

# ── 3. Device bundle ────────────────────────────────────────────────────────
# Target the FIXED demo device, not the newest dir (which is the throwaway
# provision_ca.sh just minted inside refresh_broker.sh — see header note).
DEVDIR="out/device-$DEMO_UUID"
if [ ! -d "$DEVDIR" ]; then
    echo "[demo] ERROR: demo device bundle not found: $DEVDIR" >&2
    echo "[demo] Provision it once with: DEVICE_UUID=$DEMO_UUID ./scripts/provision_ca.sh <label>" >&2
    exit 1
fi
step "updating $DEVDIR/broker.json -> $TIP"
cat > "$DEVDIR/broker.json" <<EOF
{"_v":1,"host":"tablet.local","ip":"$TIP","port":8883}
EOF
cp -r "$DEVDIR" "$WIN_HOME/"
cp scripts/provision_device.py "$WIN_HOME/"
echo "[demo] copied $DEVDIR + provision_device.py to $WIN_HOME/"

# ── 4. MagicMirror² ─────────────────────────────────────────────────────────
step "(re)starting MagicMirror²"
# Free port 8080 if held
if PID=$(ss -tlnp 2>/dev/null | awk '/:8080/ {match($0, /pid=([0-9]+)/, m); print m[1]; exit}'); then
    [ -n "$PID" ] && { echo "[demo] killing PID $PID on :8080"; kill "$PID" 2>/dev/null || kill -9 "$PID" 2>/dev/null || true; sleep 1; }
fi

( cd "$MM_DIR" && nohup npm run server > "$MM_LOG" 2>&1 & disown )
sleep 5
echo "[demo] MagicMirror log (first lines):"
grep -E "Ready to go|MQTT|Subscribed|ERROR" "$MM_LOG" | tail -10 || tail -10 "$MM_LOG"

# ── 5. Manual checklist ─────────────────────────────────────────────────────
cat <<EOM

[demo] laptop side done. Now from Windows PowerShell:

    cd C:\\Users\\frede
    # push today's broker.json to the Pico's littlefs
    # (flash bringup_provision.uf2 first via BOOTSEL drag-and-drop)
    py -3.13 provision_device.py COM<N> device-$DEMO_UUID

Then flash bringup_sensors_slow.uf2 (already at C:\\Users\\frede\\) and open
http://localhost:8080 in a browser. Tail \`tail -f $MM_LOG\` to watch the
bridge connect.

EOM
