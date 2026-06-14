#!/data/data/com.termux/files/usr/bin/sh
# RMMS tablet autostart — Termux:Boot entry point.
#
# Symlinked from ~/.termux/boot/ so an Android boot brings the whole tablet
# tier up with NO laptop and NO demo_start.sh: sshd, the Mosquitto mTLS broker,
# the tablet.local mDNS responder, the presence->screen bridge (if its certs
# are present), and MagicMirror² (server + Chrome viewer).
#
# Everything here is IP-INDEPENDENT, so a new DHCP lease needs no
# reconfiguration:
#   - the broker cert SANs `tablet.local`, and the device connects by that name,
#   - the mDNS responder advertises the CURRENT IP it detects at runtime,
#   - MagicMirror² and the broker talk over localhost.
# That is the whole point: after this is installed, a reboot is self-healing.
#
# Idempotent: every service is guarded by pgrep, so re-running (or Termux:Boot
# firing twice) never double-launches.
#
# Install (run once in Termux, with this file at ~/rmms/tablet_boot.sh):
#   mkdir -p ~/.termux/boot
#   ln -sf ~/rmms/tablet_boot.sh ~/.termux/boot/10-rmms
#   chmod +x ~/rmms/tablet_boot.sh
#
# Manual prerequisites Android does not allow scripting (do these once):
#   - install the Termux:Boot add-on from F-Droid (NOT Play Store),
#   - disable battery optimization for Termux (Android Settings),
#   - unlock the device once after each boot (Termux:Boot fires post-unlock).

RMMS="$HOME/rmms"
mkdir -p "$RMMS"
LOG="$RMMS/boot.log"
echo "=== tablet_boot $(date 2>/dev/null || echo) ===" >> "$LOG"

# Hold a wake lock so Doze does not suspend Termux once the screen sleeps.
# Released only by termux-wake-unlock. Needs the termux-api package; ignore if
# absent.
termux-wake-lock 2>/dev/null || true

# 1. sshd — lets the laptop reach the tablet without a manual start.
pgrep -x sshd >/dev/null 2>&1 || sshd

# 1b. WAIT FOR Wi-Fi. Termux:Boot can fire before the Wi-Fi associates; if the
#     mDNS responder starts then, its route-probe falls back to 127.0.0.1 and
#     it advertises loopback — the device would never find the broker. Block
#     (up to ~60 s) until we have a real (non-loopback) routable IP. Uses
#     python3 (already required by the responder), so no extra dependency.
i=0
while [ "$i" -lt 30 ]; do
    if python3 -c "import socket; s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.connect(('8.8.8.8',80)); ip=s.getsockname()[0]; exit(0 if not ip.startswith('127.') else 1)" 2>/dev/null; then
        break
    fi
    sleep 2
    i=$((i + 1))
done
echo "tablet_boot: network ready after ${i} checks" >> "$LOG"

# 2. Mosquitto (mTLS broker). Uses the certs already in ~/rmms (SAN
#    tablet.local), so IP changes do not invalidate them.
if [ -f "$RMMS/mosquitto.conf" ] && \
   ! pgrep -f "mosquitto .*mosquitto.conf" >/dev/null 2>&1; then
    setsid mosquitto -c "$RMMS/mosquitto.conf" -v >> "$RMMS/mosquitto.log" 2>&1 &
fi

# 3. mDNS responder — advertises tablet.local -> the current IP it detects.
if [ -f "$RMMS/tablet_mdns_responder.py" ] && \
   ! pgrep -f "tablet_mdns_responder" >/dev/null 2>&1; then
    ( cd "$RMMS" && \
      setsid python3 -u tablet_mdns_responder.py tablet >> "$RMMS/mdns.log" 2>&1 & )
fi

# 4. Presence -> screen bridge (optional; only if its mirror cert bundle and
#    script are present).
if [ -f "$RMMS/tablet_presence_screen.py" ] && [ -f "$RMMS/mirror/cert.pem" ] && \
   ! pgrep -f "tablet_presence_screen" >/dev/null 2>&1; then
    ( cd "$RMMS" && \
      setsid python3 -u tablet_presence_screen.py --insecure >> "$RMMS/presence.log" 2>&1 & )
fi

# 5. MagicMirror² (server + Chrome). The launcher self-restarts MM² on crash,
#    so guard on the launcher itself, not the node process.
if [ -x "$RMMS/tablet_start_magicmirror.sh" ] && \
   ! pgrep -f "tablet_start_magicmirror" >/dev/null 2>&1; then
    setsid "$RMMS/tablet_start_magicmirror.sh" >> "$RMMS/mm-boot.log" 2>&1 &
fi

echo "tablet_boot: launch dispatched" >> "$LOG"
