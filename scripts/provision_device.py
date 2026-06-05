#!/usr/bin/env python3
"""Push a device-provisioning bundle into the Pico over USB-serial.

Pairs with test/bringup/provision.c (the `bringup_provision` UF2). Replaces
the bake_certs.py path, which baked the bundle into the firmware image.

Workflow:
    1. Build the bundle off-device:
           scripts/provision_ca.sh <device-label>
       producing out/device-<uuid>/{ca.der, dev.crt, dev.key,
       device.json, wifi.json, broker.json, sensors.json}.
    2. Flash bringup_provision.uf2 to the target Pico.
    3. Run:
           scripts/provision_device.py /dev/ttyACM0 out/device-<uuid>
       The script writes each file, verifies the device-reported SHA-256,
       runs a final LIST + integrity check, and reboots the device.

The device side enforces a /cfg/ /certs/ /state/ path whitelist; this driver
maps local filenames to the canonical §11 paths. Any artefact in the bundle
dir not listed in BUNDLE_LAYOUT is ignored — keeps unrelated files
(e.g. README, openssl.cnf) out of the device filesystem.
"""

from __future__ import annotations

import argparse
import hashlib
import sys
import time
from pathlib import Path

try:
    import serial  # type: ignore
except ImportError as _e:
    import traceback as _tb
    _tb.print_exc()
    sys.exit(f"pyserial import failed ({_e}) — interpreter={sys.executable}")


# Local filename → on-device canonical path (CLAUDE.md §11).
BUNDLE_LAYOUT: dict[str, str] = {
    "ca.der":       "/certs/ca.der",
    "dev.crt":      "/certs/dev.crt",
    "dev.key":      "/certs/dev.key",
    "device.json":  "/cfg/device.json",
    "wifi.json":    "/cfg/wifi.json",
    "broker.json":  "/cfg/broker.json",
    "sensors.json": "/cfg/sensors.json",
}

# Order matters for human inspection of the log; identity first, config last.
PUT_ORDER = [
    "ca.der", "dev.crt", "dev.key",
    "device.json", "wifi.json", "broker.json", "sensors.json",
]

READY_BANNER = "PROVISION READY v=1"


class ProvisionError(RuntimeError):
    pass


def wait_for_banner(port: serial.Serial, timeout_s: float = 15.0) -> None:
    """Drain device output until the READY banner shows up."""
    deadline = time.monotonic() + timeout_s
    buf = b""
    while time.monotonic() < deadline:
        chunk = port.read(256)
        if chunk:
            buf += chunk
            sys.stdout.write(chunk.decode("ascii", errors="replace"))
            sys.stdout.flush()
            if READY_BANNER.encode() in buf:
                return
        else:
            time.sleep(0.05)
    raise ProvisionError(f"timeout waiting for '{READY_BANNER}' banner")


def read_line(port: serial.Serial, timeout_s: float = 10.0) -> str:
    port.timeout = timeout_s
    raw = port.readline()
    if not raw:
        raise ProvisionError("device read timeout (no newline)")
    return raw.decode("ascii", errors="replace").rstrip("\r\n")


def expect_ok(port: serial.Serial, expected_sha_hex: str | None = None) -> str:
    """Read a response line; require OK; if expected_sha given, compare."""
    resp = read_line(port)
    if not resp.startswith("OK"):
        raise ProvisionError(f"device error: {resp}")
    if expected_sha_hex is not None:
        # Format: "OK sha256=<hex>"
        if "sha256=" not in resp:
            raise ProvisionError(f"OK without sha256: {resp}")
        got = resp.split("sha256=", 1)[1].strip()
        if got.lower() != expected_sha_hex.lower():
            raise ProvisionError(
                f"sha mismatch: device={got} local={expected_sha_hex}"
            )
    return resp


def put_file(port: serial.Serial, dev_path: str, payload: bytes) -> None:
    expected = hashlib.sha256(payload).hexdigest()
    header = f"PUT {dev_path} {len(payload)}\n".encode("ascii")
    port.write(header)
    port.write(payload)
    port.flush()
    resp = expect_ok(port, expected_sha_hex=expected)
    print(f"  -> {dev_path} ({len(payload)} B) {resp}")


def do_hello(port: serial.Serial) -> None:
    port.write(b"HELLO\n")
    port.flush()
    resp = read_line(port)
    if resp != "HELLO OK":
        raise ProvisionError(f"unexpected HELLO response: {resp}")


def do_list(port: serial.Serial) -> dict[str, tuple[int, str]]:
    port.write(b"LIST\n")
    port.flush()
    files: dict[str, tuple[int, str]] = {}
    while True:
        line = read_line(port, timeout_s=5.0)
        if line == "LIST END":
            return files
        if not line.startswith("FILE "):
            raise ProvisionError(f"unexpected LIST line: {line}")
        # "FILE /path size shahex"
        _, path, size_s, sha = line.split()
        files[path] = (int(size_s), sha)


def do_reboot(port: serial.Serial) -> None:
    port.write(b"REBOOT\n")
    port.flush()
    resp = read_line(port)
    if resp != "BYE":
        raise ProvisionError(f"unexpected REBOOT response: {resp}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("port", help="serial port e.g. /dev/ttyACM0 or COM7")
    ap.add_argument("bundle", help="path to out/device-<uuid>/")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--no-reboot", action="store_true",
                    help="leave device in command loop instead of REBOOT")
    args = ap.parse_args()

    bundle = Path(args.bundle)
    if not bundle.is_dir():
        sys.exit(f"bundle dir not found: {bundle}")

    # Load whatever the bundle has — provision_ca.sh only mints identity
    # (device.json + certs); runtime config (wifi/broker/sensors) is supplied
    # per-deployment and may legitimately be absent on an identity-only run.
    # Required: certs + device.json. Anything else is best-effort.
    REQUIRED = {"ca.der", "dev.crt", "dev.key", "device.json"}
    payloads: dict[str, tuple[str, bytes]] = {}
    skipped: list[str] = []
    for fname in PUT_ORDER:
        src = bundle / fname
        if not src.exists():
            if fname in REQUIRED:
                sys.exit(f"bundle missing required file: {src}")
            skipped.append(fname)
            continue
        payloads[fname] = (BUNDLE_LAYOUT[fname], src.read_bytes())
    if skipped:
        print(f"[provision] not in bundle, will skip: {', '.join(skipped)}")

    print(f"[provision] opening {args.port} @ {args.baud}")
    port = serial.Serial(args.port, args.baud, timeout=2.0)
    try:
        wait_for_banner(port)
        do_hello(port)

        print("[provision] pushing artefacts")
        for fname in PUT_ORDER:
            if fname not in payloads: continue
            dev_path, data = payloads[fname]
            put_file(port, dev_path, data)

        print("[provision] verifying via LIST")
        listed = do_list(port)
        problems: list[str] = []
        for fname in PUT_ORDER:
            if fname not in payloads: continue
            dev_path, data = payloads[fname]
            expected_sha = hashlib.sha256(data).hexdigest()
            entry = listed.get(dev_path)
            if entry is None:
                problems.append(f"missing on device: {dev_path}")
                continue
            size, sha = entry
            if size != len(data):
                problems.append(
                    f"{dev_path}: size {size} on device vs {len(data)} local"
                )
            if sha.lower() != expected_sha.lower():
                problems.append(
                    f"{dev_path}: sha {sha} on device vs {expected_sha} local"
                )
        if problems:
            for p in problems:
                print(f"  !! {p}")
            raise ProvisionError("verification failed")
        print("[provision] all artefacts verified ✓")

        if args.no_reboot:
            print("[provision] leaving device in command loop (--no-reboot)")
        else:
            print("[provision] rebooting device")
            do_reboot(port)

    except ProvisionError as e:
        print(f"[provision] FAIL: {e}", file=sys.stderr)
        return 1
    finally:
        port.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
