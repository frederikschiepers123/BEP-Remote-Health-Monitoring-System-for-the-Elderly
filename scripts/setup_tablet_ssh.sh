#!/usr/bin/env bash
# One-time setup: copy the laptop's SSH public key onto the tablet so
# scripts/refresh_broker.sh can run without prompting for the Termux
# password each time.
#
# Usage:
#   scripts/setup_tablet_ssh.sh <tablet-ip>
#
# After this runs once, every subsequent `./scripts/refresh_broker.sh <ip>`
# is fully unattended.
#
# Pre-reqs on the tablet (Termux):
#   pkg install openssh         # already done if scp has been working
#   passwd                       # must be set, since this run authenticates with it
#   sshd                         # listening on port 8022
#
# Env overrides: TERMUX_USER (default u0_a76), TERMUX_PORT (default 8022).

set -euo pipefail

TIP="${1:-}"
if [ -z "$TIP" ]; then
    echo "usage: $0 <tablet-ip>" >&2
    exit 2
fi

TERMUX_USER="${TERMUX_USER:-u0_a76}"
TERMUX_PORT="${TERMUX_PORT:-8022}"

# 1. Generate a laptop SSH key if one doesn't already exist.
if [ ! -f ~/.ssh/id_ed25519 ]; then
    echo "[setup] no ed25519 key found — generating one"
    ssh-keygen -t ed25519 -N "" -f ~/.ssh/id_ed25519
fi

# 2. Push it to the tablet. ssh-copy-id handles authorized_keys + perms.
echo "[setup] copying public key to $TERMUX_USER@$TIP (will prompt for Termux password)"
ssh-copy-id -p "$TERMUX_PORT" -o StrictHostKeyChecking=accept-new \
    "${TERMUX_USER}@${TIP}"

# 3. Smoke-test.
echo "[setup] verifying passwordless ssh works"
ssh -p "$TERMUX_PORT" -o StrictHostKeyChecking=accept-new -o BatchMode=yes \
    "${TERMUX_USER}@${TIP}" 'echo "[tablet] reachable as $(whoami)"'

echo "[setup] done. Next time you need the broker:"
echo "  ./scripts/refresh_broker.sh $TIP"
