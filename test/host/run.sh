#!/usr/bin/env bash
# Host-native unit tests (CLAUDE.md §14.1). Compiles each driver's logic half
# against the stub HAL in test/host/stubs/ and runs the cmocka suite — no Pico
# toolchain, no hardware.
#
# CMocka: apt `libcmocka-dev`, or a local build under ~/.local (this script
# auto-detects the latter via CMOCKA_PREFIX, default ~/.local).
#
# Usage: test/host/run.sh        (build + run all)
#        CMOCKA_PREFIX=/path test/host/run.sh

set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
PREFIX="${CMOCKA_PREFIX:-$HOME/.local}"
OUT="$(mktemp -d)"; trap 'rm -rf "$OUT"' EXIT

CMOCKA_INC="$PREFIX/include"; CMOCKA_LIB="$PREFIX/lib"
if [ ! -f "$CMOCKA_INC/cmocka.h" ]; then
    # fall back to system cmocka (e.g. apt libcmocka-dev in CI)
    CMOCKA_INC="/usr/include"; CMOCKA_LIB=""
    [ -f /usr/include/cmocka.h ] || { echo "cmocka not found (install libcmocka-dev or build to ~/.local)"; exit 2; }
fi

CFLAGS="-DHOST_TEST=1 -std=c11 -g -Wall
  -I$HERE/stubs
  -I$REPO/components/board -I$REPO/components/log
  -I$REPO/components/sensor_env -I$REPO/components/sensor_air
  -I$REPO/components/sensor_light -I$REPO/components/sensor_radar
  -I$REPO/components/spool
  -I$CMOCKA_INC"
# Only emit -L/-rpath for a non-system cmocka prefix.
if [ -n "$CMOCKA_LIB" ]; then
    LDFLAGS="-L$CMOCKA_LIB -Wl,-rpath,$CMOCKA_LIB -lcmocka -lm"
else
    LDFLAGS="-lcmocka -lm"
fi
STUBS="$HERE/stubs/host_stubs.c"

# name : test.c : driver-sources (space-sep)
build_run() {
    local name="$1" test_c="$2"; shift 2
    local bin="$OUT/$name"
    # shellcheck disable=SC2086
    if gcc $CFLAGS "$HERE/$test_c" "$@" "$STUBS" $LDFLAGS -o "$bin" 2> "$OUT/$name.log"; then
        if "$bin"; then echo "  ✓ $name"; else echo "  ✗ $name (test failures)"; FAIL=1; fi
    else
        echo "  ✗ $name (COMPILE FAILED)"; sed 's/^/      /' "$OUT/$name.log" | head -25; FAIL=1
    fi
}

FAIL=0
echo "=== host unit tests ==="
build_run test_sensor_env test_sensor_env.c "$REPO/components/sensor_env/bme280.c"
build_run test_aht21      test_aht21.c      "$REPO/components/sensor_env/aht21.c"
build_run test_bh1750     test_bh1750.c     "$REPO/components/sensor_light/bh1750.c"
build_run test_ens160     test_ens160.c     "$REPO/components/sensor_air/ens160.c"
build_run test_radar_bha2 test_radar_bha2.c "$REPO/components/sensor_radar/radar_bha2.c"
build_run test_spool      test_spool.c      "$REPO/components/spool/spool.c"

echo ""
[ "$FAIL" -eq 0 ] && echo "ALL HOST TESTS PASSED" || echo "SOME HOST TESTS FAILED"
exit "$FAIL"
