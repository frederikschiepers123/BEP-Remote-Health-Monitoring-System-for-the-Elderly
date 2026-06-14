"""
mock_radxa.py — raw-topic sniffer for the RMMS sensor module.

⚠️  DEPRECATED ROLE — read this before using.

    This script used to stand in for the Radxa aggregator by republishing
    qualitative summaries to a `rmms/ui/<uuid>/...` namespace for an old
    MagicMirror² module (`MMM-RMMS`). **That whole path is dead:**

      • There is no `rmms/ui/...` namespace anymore. The mirror subscribes to
        the firmware's RAW `rmms/<uuid>/...` topics directly via
        `MMM-CustomMQTTBridge` → `MMM-SensorUI` (root CLAUDE.md §9.5).
      • The real aggregator + FHIR translator now lives in `sbc/`
        (sbc/CLAUDE.md). For anything resembling production behaviour — FHIR
        Observations, store-and-forward, time-sync publishing — run that, e.g.
        `cd sbc && docker compose -f docker-compose.dev.yml up` (local HAPI).

    So this file no longer publishes anything. It is now a small, read-only
    DEV TOOL: subscribe to one device's raw topics, decode the §9.2 JSON
    envelope, and pretty-print the values + a quick qualitative sanity label to
    stdout. Handy for a "is the Pico publishing sane data?" check during a demo
    without the mirror or the SBC.

Usage:
    # mTLS (same cert bundle the mirror/SBC would use):
    python mock_radxa.py --uuid <uuid> --host tablet.local \
        --ca   ~/rmms-ca/ca.crt \
        --cert out/mirror-<id>/cert.pem \
        --key  out/mirror-<id>/key.pem

    # No TLS (only against the broker's loopback :1883 dev listener):
    python mock_radxa.py --uuid <uuid> --host localhost --port 1883 --no-tls

Requirements:
    pip install paho-mqtt
"""

import argparse
import json
import ssl
import sys

import paho.mqtt.client as mqtt

# ── Qualitative sanity labels (DEV ONLY) ──────────────────────────────────────
# Placeholder ranges just for a readable console summary. The real clinical
# thresholds live on the mirror (MMM-SensorUI.js) and the SBC — not here.

HEART_OK_MIN, HEART_OK_MAX = 50.0, 100.0      # bpm
HEART_CHECK_MIN, HEART_CHECK_MAX = 40.0, 120.0

BREATH_OK_MIN, BREATH_OK_MAX = 12.0, 20.0     # rpm
BREATH_CHECK_MIN, BREATH_CHECK_MAX = 8.0, 25.0

TEMP_COLD_MAX, TEMP_COMFORTABLE_MAX = 18.0, 24.0   # °C
HUM_GOOD_MIN, HUM_GOOD_MAX = 30.0, 70.0            # %


def wellness_label(heart, breath):
    if heart is None and breath is None:
        return "no-data"
    status = "ok"
    if heart is not None:
        if heart < HEART_CHECK_MIN or heart > HEART_CHECK_MAX:
            status = "alert"
        elif heart < HEART_OK_MIN or heart > HEART_OK_MAX:
            status = "check"
    if breath is not None:
        if breath < BREATH_CHECK_MIN or breath > BREATH_CHECK_MAX:
            status = "alert"
        elif status != "alert" and (breath < BREATH_OK_MIN or breath > BREATH_OK_MAX):
            status = "check"
    return status


def temp_label(t):
    if t is None:
        return "?"
    if t < TEMP_COLD_MAX:
        return "cold"
    return "comfortable" if t < TEMP_COMFORTABLE_MAX else "warm"


def hum_label(h):
    if h is None:
        return "?"
    return "good" if HUM_GOOD_MIN <= h <= HUM_GOOD_MAX else "poor"


# ── MQTT callbacks ────────────────────────────────────────────────────────────

def on_connect(client, userdata, flags, rc, properties=None):
    if rc != 0:
        print(f"[tap] broker connection failed: rc={rc}", flush=True)
        return
    uuid = userdata["uuid"]
    # One-level wildcard catches env / air / radar / light / status / log.
    client.subscribe(f"rmms/{uuid}/+", qos=1)
    print(f"[tap] connected — sniffing rmms/{uuid}/+ (read-only)", flush=True)


def on_message(client, userdata, msg):
    uuid = userdata["uuid"]
    suffix = msg.topic.removeprefix(f"rmms/{uuid}/")

    try:
        payload_str = msg.payload.decode("utf-8")
    except UnicodeDecodeError:
        print(f"[tap] {suffix}: <non-utf8 payload>", flush=True)
        return

    # status / log are plain strings, not JSON envelopes.
    if suffix == "status":
        print(f"[tap] status   : {payload_str.strip()}", flush=True)
        return
    if suffix == "log":
        print(f"[tap] log      : {payload_str.strip()}", flush=True)
        return

    try:
        data = json.loads(payload_str)
    except json.JSONDecodeError as e:
        print(f"[tap] {suffix}: JSON parse error: {e}", flush=True)
        return

    q = data.get("q", 3)
    seq = data.get("seq", "?")
    v = data.get("v", {})
    qtag = {0: "ok", 1: "stale", 2: "degraded", 3: "invalid"}.get(q, f"q{q}")

    if suffix == "env":
        t, h, p = v.get("temp_c"), v.get("hum_pct"), v.get("pres_hpa")
        pres = "null" if p is None else f"{p:.1f}hPa"
        print(f"[tap] env  #{seq:<5} [{qtag}]: {t}°C / {h}% / {pres}  "
              f"({temp_label(t)}, hum {hum_label(h)})", flush=True)

    elif suffix == "air":
        print(f"[tap] air  #{seq:<5} [{qtag}]: CO2={v.get('co2_ppm')}ppm  "
              f"TVOC={v.get('tvoc_ppb')}ppb  AQI={v.get('aqi')}", flush=True)

    elif suffix == "radar":
        rm = v.get("resp_motion")
        rm_s = "?" if rm is None else ("motion" if rm else "NO-MOTION")
        print(f"[tap] radar#{seq:<5} [{qtag}]: present={v.get('presence')}  "
              f"dist={v.get('distance_mm')}mm  breath={v.get('breath_bpm')}  "
              f"heart={v.get('heart_bpm')}  resp={rm_s}  "
              f"(wellness {wellness_label(v.get('heart_bpm'), v.get('breath_bpm'))})",
              flush=True)

    elif suffix == "light":
        print(f"[tap] light#{seq:<5} [{qtag}]: {v.get('lux')} lux", flush=True)

    else:
        print(f"[tap] {suffix} #{seq} [{qtag}]: {v}", flush=True)


# ── Entry point ───────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="RMMS raw-topic sniffer (read-only dev tool)")
    parser.add_argument("--uuid", required=True, help="Sensor module UUID")
    parser.add_argument("--host", default="localhost", help="Broker hostname or IP")
    parser.add_argument("--port", type=int, default=8883, help="Broker port (8883 mTLS / 1883 loopback)")
    parser.add_argument("--ca", default=None, help="CA certificate path (.pem)")
    parser.add_argument("--cert", default=None, help="Client certificate path (.pem)")
    parser.add_argument("--key", default=None, help="Client private key path (.pem)")
    parser.add_argument("--no-tls", action="store_true", help="Disable TLS (loopback :1883 dev listener only)")
    args = parser.parse_args()

    client = mqtt.Client(
        client_id=f"rmms-tap-{args.uuid[:8]}",
        userdata={"uuid": args.uuid},
        protocol=mqtt.MQTTv311,
    )
    client.on_connect = on_connect
    client.on_message = on_message

    if not args.no_tls:
        if not all([args.ca, args.cert, args.key]):
            print("error: --ca, --cert, and --key are required unless --no-tls is set",
                  file=sys.stderr)
            sys.exit(1)
        tls_ctx = ssl.create_default_context(ssl.Purpose.SERVER_AUTH, cafile=args.ca)
        tls_ctx.load_cert_chain(certfile=args.cert, keyfile=args.key)
        tls_ctx.minimum_version = ssl.TLSVersion.TLSv1_2
        client.tls_set_context(tls_ctx)
    else:
        print("[tap] WARNING: TLS disabled — loopback dev listener only", flush=True)

    print(f"[tap] connecting to {args.host}:{args.port} for device {args.uuid}", flush=True)
    client.connect(args.host, args.port, keepalive=60)

    try:
        client.loop_forever()
    except KeyboardInterrupt:
        print("\n[tap] stopped", flush=True)


if __name__ == "__main__":
    main()
