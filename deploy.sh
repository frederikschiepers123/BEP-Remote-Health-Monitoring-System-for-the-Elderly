#!/data/data/com.termux/files/usr/bin/bash
# ─────────────────────────────────────────────────────────────────────────────
# deploy.sh — one-command, on-tablet provisioning of the RMMS tablet tier.
#
# Run this ONCE, inside Termux, from a clone of this repo on the tablet:
#
#     pkg install -y git
#     git clone <repo-url> ~/BEP-Remote-Health-Monitoring-System-for-the-Elderly
#     cd ~/BEP-Remote-Health-Monitoring-System-for-the-Elderly
#     ./deploy.sh
#
# It installs and configures everything on the tablet that can be done
# on-device, so that after the one laptop-side cert step (below) and a reboot,
# the tablet self-heals into a full RMMS hub: MQTT broker + MagicMirror² + mDNS.
#
# WHAT IT DOES (all idempotent — safe to re-run):
#   • Termux packages: git, nodejs, mosquitto, openssh, python, termux-api
#     + Python deps (paho-mqtt, zeroconf) for the mDNS responder + presence bridge
#   • MagicMirror²: npm install + install-mm + the MMM-CustomMQTTBridge mqtt dep
#     (the repo clone IS a git checkout, so install-mm's git-clean postinstall and
#      MagicMirror's own fonts/vendor fetch both work)
#   • Wires the repo's tablet runtime scripts into ~/rmms and installs the
#     self-healing Termux:Boot autostart (scripts/tablet_boot.sh → ~/.termux/boot)
#   • Starts sshd so the laptop can push the mTLS certs (the ONE laptop step)
#   • Fully Kiosk config + the screen-control APK into ~/storage/downloads
#   • Prints the remaining laptop command + manual Android steps
#
# WHAT IT DELIBERATELY DOES NOT DO:
#   • Mint or hold any mTLS certificate. The project CA private key never touches
#     the tablet (root CLAUDE.md §10.2). The broker certs + mosquitto.conf + acl
#     + mirror bundle are minted on the laptop by scripts/provision_ca.sh and
#     pushed by scripts/deploy_home_kit.sh over the sshd this script starts:
#
#         # on the laptop, once:
#         scripts/setup_tablet_ssh.sh   <tablet-ip>
#         scripts/deploy_home_kit.sh tablet <home-id> <tablet-ip>
#
#     Until that runs, MagicMirror comes up but mosquitto stays down (no certs)
#     and there is no live sensor data — by design.
#
# Reconciliation note: this supersedes the older single-shot boot/start_magicmirror.sh
# as the boot entry point. The canonical autostart is scripts/tablet_boot.sh
# (wake-lock → Wi-Fi wait → sshd → mosquitto → mDNS → presence → MagicMirror,
# all pgrep-guarded with crash-restart). MagicMirror itself is launched + kept
# alive (and the Fully Kiosk viewer opened) by scripts/tablet_start_magicmirror.sh.
# ─────────────────────────────────────────────────────────────────────────────

set -e

REPO_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$REPO_DIR"
RMMS="$HOME/rmms"

echo "======================================="
echo " Remote Health Monitoring Deployment"
echo "======================================="
echo ""
echo "Repo:  $REPO_DIR"
echo "User:  $(whoami)"
echo ""

# Sanity: this is a Termux-only script (uses pkg, termux-* tools, Termux paths).
command -v pkg >/dev/null 2>&1 || {
    echo "ERROR: 'pkg' not found — run this inside Termux on the tablet, not on a PC." >&2
    exit 1
}

########################################
# Storage access
########################################

echo "Requesting storage access..."
echo "If Android asks for permission, press ALLOW."
echo ""

termux-setup-storage || echo "(storage setup skipped/failed — APK + Fully config copy may not work)"

sleep 5

########################################
# Install dependencies
########################################

echo "Updating Termux packages..."

pkg update -y
pkg upgrade -y

echo "Installing required packages..."

# git/nodejs  → MagicMirror;  mosquitto → broker;  openssh → laptop cert push;
# python      → mDNS responder + presence bridge + tablet_boot Wi-Fi wait;
# termux-api  → wake-lock + am/pm (Fully Kiosk launch).
pkg install -y \
    git \
    nodejs \
    mosquitto \
    openssh \
    python \
    termux-api

echo ""
echo "Installing Python deps (mDNS responder + presence bridge)..."
# zeroconf → tablet.local advertisement;  paho-mqtt → presence→screen bridge.
pip install --upgrade paho-mqtt zeroconf \
    || echo "WARNING: pip install failed — tablet.local mDNS + presence bridge will not run."

########################################
# Install MagicMirror
########################################

echo ""
echo "Installing MagicMirror dependencies...(might take several minutes)"

cd "$REPO_DIR/MagicMirror"

npm install

echo ""
echo "Running MagicMirror installer..."

node --run install-mm

########################################
# Install MQTT dependency
########################################

echo ""
echo "Installing MQTT dependency (MMM-CustomMQTTBridge)..."

cd "$REPO_DIR/MagicMirror/modules/MMM-CustomMQTTBridge"

npm install mqtt

cd "$REPO_DIR"

# The tablet runtime scripts expect MagicMirror at ~/MagicMirror; point that at
# this clone so a fixed boot path works regardless of the clone directory name.
ln -sfn "$REPO_DIR/MagicMirror" "$HOME/MagicMirror"

########################################
# Wire tablet runtime scripts + self-healing autostart
########################################

echo ""
echo "Installing tablet runtime scripts into $RMMS ..."

mkdir -p "$RMMS/certs" "$RMMS/mirror" "$HOME/.termux/boot"

# Copy the runtime scripts from the clone so tablet_boot.sh finds them locally.
# (mosquitto.conf / acl / certs are NOT here — the laptop delivers those.)
cp scripts/tablet_boot.sh \
   scripts/tablet_start_magicmirror.sh \
   scripts/tablet_mdns_responder.py \
   scripts/tablet_presence_screen.py \
   "$RMMS/"
chmod +x "$RMMS/tablet_boot.sh" "$RMMS/tablet_start_magicmirror.sh"

# Self-healing Termux:Boot hook (supersedes boot/start_magicmirror.sh).
ln -sf "$RMMS/tablet_boot.sh" "$HOME/.termux/boot/10-rmms"
# Remove the older single-shot hook if a previous run installed it (avoids a
# double MagicMirror launch).
rm -f "$HOME/.termux/boot/start_magicmirror.sh"
echo "Termux:Boot hook installed: ~/.termux/boot/10-rmms -> tablet_boot.sh"

########################################
# sshd (so the laptop can push the mTLS certs)
########################################

echo ""
echo "Starting sshd (port 8022) for the laptop cert push..."
pgrep -x sshd >/dev/null 2>&1 || sshd

if [ ! -s "$HOME/.ssh/authorized_keys" ]; then
    echo ""
    echo "Set a Termux login password now so the laptop can copy its SSH key"
    echo "(press Ctrl-C to skip and set it later with: passwd):"
    passwd || echo "(password not set — run 'passwd' before the laptop cert step)"
fi

########################################
# Copy Fully Kiosk configuration
########################################

echo ""
echo "Installing Fully Kiosk configuration..."

mkdir -p "$HOME/storage/downloads"

cp fully/fully-settings.json \
   "$HOME/storage/downloads/fully-settings.json" \
   || echo "(could not copy Fully config — is storage access granted?)"

########################################
# Copy APK
########################################

if [ -f apk/appScreenControl.apk ]; then

    echo ""
    echo "Copying APK..."

    cp apk/appScreenControl.apk \
       "$HOME/storage/downloads/appScreenControl.apk" \
       || echo "(could not copy APK — is storage access granted?)"

    echo ""
    echo "Launching APK installer..."

    termux-open \
       "$HOME/storage/downloads/appScreenControl.apk" \
       || echo "(could not open APK installer — install it manually from Downloads)"

else

    echo ""
    echo "APK not found:"
    echo "apk/appScreenControl.apk"

fi

########################################
# Bring services up now (idempotent)
########################################

echo ""
echo "Bringing services up now (tablet_boot.sh)..."
# Starts sshd + mDNS + MagicMirror immediately. Mosquitto waits for the laptop
# cert step (no mosquitto.conf yet); presence bridge waits for the mirror cert.
sh "$RMMS/tablet_boot.sh" || echo "(tablet_boot reported a non-zero exit — check $RMMS/boot.log)"

# Best-effort IP discovery for the laptop handoff line below.
TABLET_IP="$(python3 -c "import socket; s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.connect(('8.8.8.8',80)); print(s.getsockname()[0])" 2>/dev/null || echo '<tablet-ip>')"

########################################
# Finished
########################################

echo ""
echo "======================================="
echo " TABLET DEPLOYMENT COMPLETED"
echo "======================================="
echo ""
echo "This tablet:  $(whoami)@${TABLET_IP}  (ssh port 8022)"
echo ""
echo ">>> ONE laptop step (delivers the mTLS certs + broker config) <<<"
echo ""
echo "    scripts/setup_tablet_ssh.sh   ${TABLET_IP}"
echo "    scripts/deploy_home_kit.sh tablet <home-id> ${TABLET_IP}"
echo ""
echo "    Until this runs, the broker (mosquitto) stays down and there is no"
echo "    live sensor data — MagicMirror will still load."
echo ""
echo "Remaining manual steps (Android won't let a script do these):"
echo ""
echo "1. Install the APK if Android prompts."
echo "2. Open Fully Kiosk Browser, then Import: Downloads/fully-settings.json"
echo "   (set its Start URL to http://localhost:8080 if not already)."
echo "3. Disable battery optimization for Termux (Android Settings) — else Doze"
echo "   kills the broker/mirror within minutes."
echo "4. Install Termux:Boot from F-Droid (NOT Play Store) and open it once."
echo "5. Reboot the tablet, then unlock it once (Termux:Boot fires post-unlock)."
echo ""
echo "6. Verify that:"
echo "   - MagicMirror starts        (http://localhost:8080)"
echo "   - Fully Kiosk launches and shows the mirror page"
echo "   - mosquitto is up           (pgrep -f mosquitto)   [after the laptop step]"
echo "   - tablet.local resolves on the LAN                 [mDNS responder]"
echo ""
