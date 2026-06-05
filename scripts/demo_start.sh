#!/usr/bin/env bash
# One-shot demo bring-up on the laptop side.
#
# Usage:
#   scripts/demo_start.sh <tablet-ip>
#
# What it does (≈15 seconds + one MM² boot):
#   1. Refresh broker cert + restart Mosquitto for today's tablet IP.
#   2. Update MagicMirror² bridge config to point at today's IP.
#   3. Update broker.json in the most-recent device bundle to today's IP
#      and copy that bundle (plus the provision driver) to /mnt/c/Users/frede/
#      so PowerShell can immediately re-push to the Pico.
#   4. Kill any process holding :8080 and start MagicMirror² in background,
#      logging to /tmp/mm2.log.
#
# Manual steps that remain:
#   - Flash bringup_provision.uf2 to the Pico (BOOTSEL).
#   - In PowerShell:
#       py -3.13 provision_device.py COM<N> device-<uuid>
#   - Flash bringup_sensors.uf2.
#   - Open http://localhost:8080 in a browser.

set -euo pipefail

TIP="${1:-}"
if [ -z "$TIP" ]; then
    echo "usage: $0 <tablet-ip>" >&2
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
DEVDIR="$(ls -dt out/device-* 2>/dev/null | head -1 || true)"
if [ -z "$DEVDIR" ]; then
    echo "[demo] WARNING: no out/device-* bundle found. Run scripts/provision_ca.sh first."
else
    step "updating $DEVDIR/broker.json -> $TIP"
    cat > "$DEVDIR/broker.json" <<EOF
{"_v":1,"host":"tablet.local","ip":"$TIP","port":8883}
EOF
    cp -r "$DEVDIR" "$WIN_HOME/"
    cp scripts/provision_device.py "$WIN_HOME/"
    echo "[demo] copied bundle + provision_device.py to $WIN_HOME/"
fi

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
    # one-time per session: push today's broker.json to the Pico
    # (flash bringup_provision.uf2 first via BOOTSEL drag-and-drop)
    py -3.13 provision_device.py COM<N> ${DEVDIR##*/}

Then flash bringup_sensors.uf2 (already at C:\\Users\\frede\\) and open
http://localhost:8080 in a browser. Tail \`tail -f $MM_LOG\` to watch the
bridge connect.

EOM
