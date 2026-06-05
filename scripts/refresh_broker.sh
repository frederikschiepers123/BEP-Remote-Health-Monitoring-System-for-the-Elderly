#!/usr/bin/env bash
# One-shot broker refresh — re-issue the broker cert for today's tablet IP,
# push it to Termux, restart Mosquitto, verify it answers.
#
# Usage:
#   scripts/refresh_broker.sh <tablet-ip>
#
#   # If you ran `make-tablet-known` once (see bottom of script), passwordless
#   # SSH means the whole flow runs in ~15 seconds with no manual prompts.
#
# Env overrides:
#   TERMUX_USER   default u0_a76
#   TERMUX_PORT   default 8022
#   RMMS_DIR      default /data/data/com.termux/files/home/rmms
#   REPO_DIR      default $(pwd)
#
# What it does (all from the laptop, no tablet typing):
#   1. BROKER_IP=<tip> ./scripts/provision_ca.sh   (re-issues SAN)
#   2. scp out/broker/* tablet:~/rmms/
#   3. ssh tablet 'mv certs → ~/rmms/certs/, pkill mosquitto, mosquitto -d'
#   4. nc -zv <tip> 8883   (verify TLS port is up)

set -euo pipefail

TIP="${1:-}"
if [ -z "$TIP" ]; then
    echo "usage: $0 <tablet-ip>" >&2
    exit 2
fi

TERMUX_USER="${TERMUX_USER:-u0_a76}"
TERMUX_PORT="${TERMUX_PORT:-8022}"
RMMS_DIR="${RMMS_DIR:-/data/data/com.termux/files/home/rmms}"
REPO_DIR="${REPO_DIR:-$(pwd)}"

cd "$REPO_DIR"

echo "[refresh] re-issuing broker cert for IP $TIP"
BROKER_IP="$TIP" ./scripts/provision_ca.sh > /tmp/refresh_broker_provision.log 2>&1
tail -5 /tmp/refresh_broker_provision.log

echo "[refresh] pushing broker bundle to $TERMUX_USER@$TIP:$RMMS_DIR/"
scp -P "$TERMUX_PORT" -o StrictHostKeyChecking=accept-new \
    out/broker/* "${TERMUX_USER}@${TIP}:${RMMS_DIR}/"

echo "[refresh] restarting mosquitto on tablet"
ssh -p "$TERMUX_PORT" -o StrictHostKeyChecking=accept-new \
    "${TERMUX_USER}@${TIP}" "
set -e
mkdir -p ~/rmms/certs
mv -f ~/rmms/ca.crt ~/rmms/broker.crt ~/rmms/broker.key ~/rmms/certs/
chmod 600 ~/rmms/certs/broker.key
pkill mosquitto 2>/dev/null || true
sleep 0.2
nohup mosquitto -c ~/rmms/mosquitto.conf -v > ~/rmms/mosquitto.log 2>&1 &
sleep 0.5
echo '[tablet] mosquitto PID:' \$(pgrep mosquitto || echo none)
"

echo "[refresh] verifying $TIP:8883 …"
if nc -zv "$TIP" 8883 2>&1 | grep -q succeeded; then
    echo "[refresh] OK — broker reachable on $TIP:8883"
else
    echo "[refresh] FAILED — port not answering" >&2
    exit 1
fi
