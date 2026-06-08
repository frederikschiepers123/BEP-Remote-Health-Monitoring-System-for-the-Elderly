#!/usr/bin/env bash
# Broker mTLS + ACL enforcement harness (CLAUDE.md §16 Q9, bring-up step 16).
#
# Proves the broker actually ENFORCES the per-identity ACL, not just that it's
# configured. mosquitto silently drops ACL-denied publishes (no error to the
# publisher), so we can't assert from the publisher side — instead a privileged
# MIRROR observer (ACL: read rmms/#) watches whether each publish actually lands.
#
# Usage:
#   scripts/test_broker_acl.sh <broker-ip> [device-uuid]
#
# Assertions:
#   A device -> rmms/<own>/...           ALLOWED   (observer sees it)
#   B device -> rmms/<other>/...         DENIED    (observer never sees it)
#   C operator -> rmms/<own>/info        ALLOWED   (observer sees it)
#   D operator -> rmms/<own>/env         DENIED    (operator may only write info/screen)
#   E device pub+sub rmms/<own>/cmd      ALLOWED   (device can read its own /cmd)
#   F device -> sub rmms/<other>/#       DENIED    (no cross-device read; operator pub there, device blind)
#   G anonymous (no cert) connect        REFUSED   (require_certificate true)
#
# Exit 0 if every assertion holds, 1 otherwise.

set -uo pipefail

TIP="${1:-}"
DEMO_UUID="${2:-52295a51-1a2d-4b2f-bedd-dacbbc1685f0}"
if [ -z "$TIP" ]; then echo "usage: $0 <broker-ip> [device-uuid]" >&2; exit 2; fi

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"; cd "$REPO_DIR"
OTHER_UUID="00000000-0000-0000-0000-000000000000"

CA="$HOME/rmms-ca/ca.crt"; [ -f "$CA" ] || CA="out/broker/ca.crt"
DEV_DIR="out/device-$DEMO_UUID"
MIR_DIR="$(ls -d out/mirror-* 2>/dev/null | head -1)"
OP_DIR="$(ls -d out/operator-* 2>/dev/null | head -1)"

for f in "$CA" "$DEV_DIR/dev.crt.pem" "$DEV_DIR/dev.key.pem" \
         "$MIR_DIR/cert.pem" "$MIR_DIR/key.pem" "$OP_DIR/cert.pem" "$OP_DIR/key.pem"; do
    [ -f "$f" ] || { echo "missing cert material: $f" >&2; exit 2; }
done

TLS="--cafile $CA -h $TIP -p 8883 --insecure"   # --insecure: skip CN/SAN match (we test ACL, not SAN; broker cert SAN may lag IP)
DEV="$TLS --cert $DEV_DIR/dev.crt.pem --key $DEV_DIR/dev.key.pem"
MIR="$TLS --cert $MIR_DIR/cert.pem --key $MIR_DIR/key.pem"
OP="$TLS  --cert $OP_DIR/cert.pem  --key $OP_DIR/key.pem"

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; jobs -p | xargs -r kill 2>/dev/null' EXIT
PASS=0; FAIL=0
ok()   { printf "  \033[1;32mPASS\033[0m  %s\n" "$1"; PASS=$((PASS+1)); }
bad()  { printf "  \033[1;31mFAIL\033[0m  %s\n" "$1"; FAIL=$((FAIL+1)); }

# observe <role-flags> <topic> <expect-substr> : run a subscriber, fire $FIRE, check capture
# Returns 0 if the capture contains <expect-substr>, 1 otherwise.
observe() {
    local who="$1" topic="$2" want="$3" cap="$TMP/cap.$$"
    : > "$cap"
    timeout 4 mosquitto_sub $who -t "$topic" -v > "$cap" 2>/dev/null &
    local spid=$!
    sleep 1.5
    eval "$FIRE"
    sleep 1.5
    kill "$spid" 2>/dev/null; wait "$spid" 2>/dev/null
    grep -q "$want" "$cap"
}

echo "=== broker ACL harness vs $TIP:8883 (device=$DEMO_UUID) ==="

# A: device -> own subtree, observed by mirror
FIRE="mosquitto_pub $DEV -t rmms/$DEMO_UUID/acltest -m ALLOW_A -q 1"
if observe "$MIR" "rmms/$DEMO_UUID/acltest" "ALLOW_A"; then ok "A device publishes to its own subtree (allowed)"
else bad "A device publish to own subtree NOT observed (should be allowed)"; fi

# B: device -> other device's subtree, must NOT be observed
FIRE="mosquitto_pub $DEV -t rmms/$OTHER_UUID/acltest -m DENY_B -q 1"
if observe "$MIR" "rmms/$OTHER_UUID/acltest" "DENY_B"; then bad "B device published into ANOTHER device subtree (ACL breach!)"
else ok "B device publish to other device subtree blocked (denied)"; fi

# C: operator -> info, observed by mirror
FIRE="mosquitto_pub $OP -t rmms/$DEMO_UUID/info -m '{\"text\":\"ALLOW_C\"}' -q 1"
if observe "$MIR" "rmms/$DEMO_UUID/info" "ALLOW_C"; then ok "C operator publishes to /info (allowed)"
else bad "C operator publish to /info NOT observed (should be allowed)"; fi

# D: operator -> env, must NOT be observed (operator may only write info/screen)
FIRE="mosquitto_pub $OP -t rmms/$DEMO_UUID/env -m DENY_D -q 1"
if observe "$MIR" "rmms/$DEMO_UUID/env" "DENY_D"; then bad "D operator published to /env (ACL breach — operator should be info/screen only!)"
else ok "D operator publish to /env blocked (denied)"; fi

# E: device can read its own /cmd (device pub + device sub on own cmd)
FIRE="mosquitto_pub $DEV -t rmms/$DEMO_UUID/cmd -m '{\"cmd\":\"ALLOW_E\"}' -q 1"
if observe "$DEV" "rmms/$DEMO_UUID/cmd" "ALLOW_E"; then ok "E device reads its own /cmd (allowed)"
else bad "E device could not read its own /cmd (should be allowed)"; fi

# F: device may NOT read another device's topics (operator writes there, device blind)
FIRE="mosquitto_pub $OP -t rmms/$OTHER_UUID/info -m DENY_F -q 1"
if observe "$DEV" "rmms/$OTHER_UUID/info" "DENY_F"; then bad "F device read ANOTHER device's topic (ACL breach!)"
else ok "F device cannot read other device topics (denied)"; fi

# G: anonymous (no client cert) must be refused by require_certificate
if timeout 5 mosquitto_pub --cafile "$CA" -h "$TIP" -p 8883 --insecure \
        -t rmms/$DEMO_UUID/x -m nope 2>/dev/null; then
    bad "G broker accepted a connection with NO client cert (require_certificate broken!)"
else ok "G broker refuses connections without a client cert"; fi

echo ""
echo "=== $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
