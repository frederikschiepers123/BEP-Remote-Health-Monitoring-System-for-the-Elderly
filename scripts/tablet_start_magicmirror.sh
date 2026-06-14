#!/data/data/com.termux/files/usr/bin/env bash
# Tablet-side launcher for MagicMirror² in server-only mode + Chrome viewer.
#
# Place this in Termux at ~/start_magicmirror.sh (or symlink from
# ~/.termux/boot/ — see Termux:Boot in the install crib) to run MM² on boot.
#
# What it does:
#   1. Starts `npm run server` inside ~/MagicMirror, which runs node ./serveronly
#      and exposes the mirror at http://localhost:8080. Output is appended to
#      ~/MagicMirror/server.log.
#   2. Waits up to 30 s for port 8080 to accept connections.
#   3. Launches Chrome via Android's `am start` intent, pointing at
#      http://localhost:8080. (Chrome has no true kiosk flag on Android; install
#      a kiosk-mode app like "Fully Kiosk Browser" if you need URL-bar / OS-bar
#      hiding for production.)
#   4. Loops with backoff if MM² dies, so a transient crash auto-restarts.
#
# Pre-reqs (Termux, one-time):
#   pkg update && pkg upgrade
#   pkg install nodejs git termux-api
#   git clone <your tree>/MagicMirror ~/MagicMirror
#   cd ~/MagicMirror && npm run install-mm
#   cd ~/MagicMirror/modules/MMM-CustomMQTTBridge && npm install   # mTLS MQTT bridge (not MMM-MQTT)
#   mkdir -p ~/MagicMirror/certs
#   # ... copy mirror bundle: ca.crt / cert.pem / key.pem ...
#   sed -i "s/REPLACE_WITH_MIRROR_CN/<actual mirror CN>/" ~/MagicMirror/config/config.js
#
# Auto-start (Termux:Boot — install from F-Droid):
#   mkdir -p ~/.termux/boot
#   ln -sf ~/start_magicmirror.sh ~/.termux/boot/start_magicmirror
#   chmod +x ~/start_magicmirror.sh

set -u

MM_DIR="${MM_DIR:-$HOME/MagicMirror}"
MM_PORT="${MM_PORT:-8080}"
LOG="$MM_DIR/server.log"
URL="http://localhost:$MM_PORT"

[ -d "$MM_DIR" ] || { echo "MagicMirror dir not found at $MM_DIR" >&2; exit 1; }

wait_for_port () {
    local n=0
    while [ "$n" -lt 30 ]; do
        if (echo > "/dev/tcp/127.0.0.1/$MM_PORT") 2>/dev/null; then
            return 0
        fi
        sleep 1
        n=$((n+1))
    done
    return 1
}

launch_viewer () {
    if ! command -v am >/dev/null 2>&1; then
        echo "(no 'am' on PATH — install termux-api; on a desktop just open $URL)" >&2
        return
    fi
    # Prefer Fully Kiosk Browser (de.ozerov.fully) — it has a real kiosk mode
    # and a configured Start URL (set it to http://localhost:8080). Fall back
    # to Chrome (no true kiosk on Android) if Fully isn't installed.
    if pm list packages 2>/dev/null | grep -q "de.ozerov.fully" \
       || [ "${VIEWER:-}" = "fully" ]; then
        am start -n de.ozerov.fully/.MainActivity >/dev/null 2>&1 \
            || echo "am start (Fully) failed" >&2
    else
        am start -n com.android.chrome/com.google.android.apps.chrome.Main \
            -a android.intent.action.VIEW -d "$URL" \
            --activity-clear-task >/dev/null 2>&1 \
            || echo "am start (Chrome) failed (Chrome/termux-api missing?)" >&2
    fi
}

run_once () {
    cd "$MM_DIR" || return 1
    echo "[$(date -Iseconds)] starting MagicMirror² on :$MM_PORT" >> "$LOG"
    # `npm run server` runs `node ./serveronly` per MagicMirror's package.json.
    npm run server >> "$LOG" 2>&1
}

# Main loop — restart with a small backoff if the server dies.
backoff=2
while true; do
    run_once &
    server_pid=$!

    if wait_for_port; then
        echo "[$(date -Iseconds)] MM² listening, opening viewer" >> "$LOG"
        launch_viewer
        backoff=2
    else
        echo "[$(date -Iseconds)] MM² did not open :$MM_PORT in 30s" >> "$LOG"
    fi

    wait "$server_pid"
    echo "[$(date -Iseconds)] MM² exited, restarting in ${backoff}s" >> "$LOG"
    sleep "$backoff"
    backoff=$(( backoff < 60 ? backoff * 2 : 60 ))
done
