"""
mock_radxa.py — development stand-in for the Radxa aggregation service.

Subscribes to raw sensor topics published by the firmware, applies simple
threshold logic, and republishes qualitative summaries to rmms/ui/<uuid>/...
so that MagicMirror² / MMM-RMMS can be tested without the full Radxa service.

This script is intentionally simple — it is a dev tool, not production code.
For the real implementation see docs/CLAUDE_radxa.md.

Usage:
    python mock_radxa.py --uuid <device-uuid> --host <broker-host>

    # With mTLS (same certs as the real Radxa would use):
    python mock_radxa.py --uuid <uuid> --host tablet.local \
        --ca   /path/to/ca.der \
        --cert /path/to/radxa.crt \
        --key  /path/to/radxa.key

    # Without TLS (only works if broker allows it — not default):
    python mock_radxa.py --uuid <uuid> --host localhost --no-tls

Requirements:
    pip install paho-mqtt
"""

import argparse
import json
import ssl
import sys
import time

import paho.mqtt.client as mqtt

# ── Thresholds ────────────────────────────────────────────────────────────────
# These are placeholder values for development only.
# The real service uses clinically sourced thresholds — see CLAUDE_radxa.md §11.2.

HEART_OK_MIN    = 50.0   # bpm
HEART_OK_MAX    = 100.0
HEART_CHECK_MIN = 40.0
HEART_CHECK_MAX = 120.0

BREATH_OK_MIN    = 12.0  # rpm
BREATH_OK_MAX    = 20.0
BREATH_CHECK_MIN = 8.0
BREATH_CHECK_MAX = 25.0

TEMP_COLD_MAX       = 18.0  # °C
TEMP_COMFORTABLE_MAX = 24.0

HUM_GOOD_MIN = 30.0  # %
HUM_GOOD_MAX = 70.0

# ── State ─────────────────────────────────────────────────────────────────────

state = {
    "presence":    False,
    "heart_bpm":   None,
    "breath_bpm":  None,
    "temp_c":      None,
    "hum_pct":     None,
    "online":      False,
    "last_seen_ms": 0,
    "wellness_since_ms": 0,
    "wellness_status": "ok",
}


# ── Threshold logic ───────────────────────────────────────────────────────────

def calc_wellness():
    heart  = state["heart_bpm"]
    breath = state["breath_bpm"]

    if heart is None and breath is None:
        return "ok"   # no data yet — don't alarm

    status = "ok"

    if heart is not None:
        if heart < HEART_CHECK_MIN or heart > HEART_CHECK_MAX:
            status = "alert"
        elif heart < HEART_OK_MIN or heart > HEART_OK_MAX:
            status = "check"

    if breath is not None:
        if breath < BREATH_CHECK_MIN or breath > BREATH_CHECK_MAX:
            status = "alert"
        elif (status != "alert" and
              (breath < BREATH_OK_MIN or breath > BREATH_OK_MAX)):
            status = "check"

    return status


def calc_temp_label():
    t = state["temp_c"]
    if t is None:
        return "comfortable"
    if t < TEMP_COLD_MAX:
        return "cold"
    if t < TEMP_COMFORTABLE_MAX:
        return "comfortable"
    return "warm"


def calc_air_label():
    h = state["hum_pct"]
    if h is None:
        return "good"
    return "good" if HUM_GOOD_MIN <= h <= HUM_GOOD_MAX else "poor"


def calc_confidence():
    # Simplified: real service would use distance + signal quality
    return "high" if state["presence"] else "low"


def now_ms():
    return int(time.monotonic() * 1000)


# ── MQTT callbacks ────────────────────────────────────────────────────────────

def on_connect(client, userdata, flags, rc, properties=None):
    if rc != 0:
        print(f"[mock_radxa] broker connection failed: rc={rc}", flush=True)
        return

    uuid = userdata["uuid"]
    print(f"[mock_radxa] connected — subscribing to rmms/{uuid}/...", flush=True)

    client.subscribe(f"rmms/{uuid}/env",    qos=1)
    client.subscribe(f"rmms/{uuid}/radar",  qos=1)
    client.subscribe(f"rmms/{uuid}/light",  qos=1)
    client.subscribe(f"rmms/{uuid}/status", qos=1)


def on_message(client, userdata, msg):
    uuid   = userdata["uuid"]
    topic  = msg.topic
    suffix = topic.removeprefix(f"rmms/{uuid}/")

    try:
        payload_str = msg.payload.decode("utf-8")
    except Exception:
        print(f"[mock_radxa] cannot decode payload on {topic}", flush=True)
        return

    # status is a plain string, not JSON
    if suffix == "status":
        state["online"] = (payload_str.strip('"') == "online")
        state["last_seen_ms"] = now_ms()
        publish_connection(client, uuid)
        return

    try:
        data = json.loads(payload_str)
    except json.JSONDecodeError as e:
        print(f"[mock_radxa] JSON parse error on {topic}: {e}", flush=True)
        return

    q = data.get("q", 3)
    v = data.get("v", {})

    if suffix == "env":
        if q <= 2:   # ok or degraded — use the value
            state["temp_c"]  = v.get("temp_c")
            state["hum_pct"] = v.get("hum_pct")
        publish_ambient(client, uuid)
        publish_temperature(client, uuid)

    elif suffix == "radar":
        if q <= 2:
            state["presence"]   = v.get("presence", False)
            state["heart_bpm"]  = v.get("heart_bpm")
            state["breath_bpm"] = v.get("breath_bpm")
        state["last_seen_ms"] = now_ms()
        publish_presence(client, uuid)
        publish_wellness(client, uuid)
        publish_heart_rate(client, uuid)
        publish_connection(client, uuid)

    elif suffix == "light":
        pass   # lux not surfaced on mirror


def publish_presence(client, uuid):
    payload = json.dumps({
        "present":    state["presence"],
        "confidence": calc_confidence(),
    })
    client.publish(f"rmms/ui/{uuid}/presence", payload, qos=1, retain=True)
    print(f"[mock_radxa] → presence  {payload}", flush=True)


def publish_wellness(client, uuid):
    new_status = calc_wellness()
    if new_status != state["wellness_status"]:
        state["wellness_since_ms"] = now_ms()
        state["wellness_status"]   = new_status

    payload = json.dumps({
        "status":   state["wellness_status"],
        "since_ms": now_ms() - state["wellness_since_ms"],
    })
    client.publish(f"rmms/ui/{uuid}/wellness", payload, qos=1, retain=True)
    print(f"[mock_radxa] → wellness  {payload}", flush=True)


def publish_ambient(client, uuid):
    payload = json.dumps({
        "temp": calc_temp_label(),
        "air":  calc_air_label(),
    })
    client.publish(f"rmms/ui/{uuid}/ambient", payload, qos=1, retain=True)
    print(f"[mock_radxa] → ambient   {payload}", flush=True)


def publish_heart_rate(client, uuid):
    bpm = state["heart_bpm"]
    if bpm is None:
        return   # no reading yet — don't publish null
    payload = json.dumps({"bpm": round(bpm, 1)})
    client.publish(f"rmms/ui/{uuid}/heart_rate", payload, qos=1, retain=True)
    print(f"[mock_radxa] → heart_rate {payload}", flush=True)


def publish_temperature(client, uuid):
    temp = state["temp_c"]
    if temp is None:
        return   # no reading yet — don't publish null
    payload = json.dumps({"temp_c": round(temp, 1)})
    client.publish(f"rmms/ui/{uuid}/temperature", payload, qos=1, retain=True)
    print(f"[mock_radxa] → temperature {payload}", flush=True)


def publish_connection(client, uuid):
    payload = json.dumps({
        "online":       state["online"],
        "last_seen_ms": now_ms() - state["last_seen_ms"],
    })
    client.publish(f"rmms/ui/{uuid}/connection", payload, qos=1, retain=True)
    print(f"[mock_radxa] → connection {payload}", flush=True)


# ── Entry point ───────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Mock Radxa UI publisher")
    parser.add_argument("--uuid",   required=True, help="Sensor module UUID")
    parser.add_argument("--host",   default="localhost", help="Broker hostname or IP")
    parser.add_argument("--port",   type=int, default=8883, help="Broker port")
    parser.add_argument("--ca",     default=None, help="CA certificate path (.der or .pem)")
    parser.add_argument("--cert",   default=None, help="Client certificate path")
    parser.add_argument("--key",    default=None, help="Client private key path")
    parser.add_argument("--no-tls", action="store_true", help="Disable TLS (dev only)")
    args = parser.parse_args()

    client = mqtt.Client(
        client_id=f"mock-radxa-{args.uuid[:8]}",
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
        print("[mock_radxa] WARNING: TLS disabled — for local dev only", flush=True)

    print(f"[mock_radxa] connecting to {args.host}:{args.port} "
          f"for device {args.uuid}", flush=True)

    client.connect(args.host, args.port, keepalive=60)

    try:
        client.loop_forever()
    except KeyboardInterrupt:
        print("\n[mock_radxa] stopped", flush=True)


if __name__ == "__main__":
    main()
