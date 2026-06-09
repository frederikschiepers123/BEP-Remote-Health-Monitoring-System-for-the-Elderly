#!/usr/bin/env python3
"""
tablet_presence_screen.py — radar-presence → tablet-screen coupling.

Bridges the firmware's radar presence to the HealthMonitorWakeTest tablet app
(Yasmina's wake/lock app):

    READ   rmms/+/radar    (mTLS, :8883, mirror cert)   -> parse v.presence
    DEBOUNCE  present >= ON_DELAY   -> publish "ON"   to `display` (:1883 plain)
              absent  >= OFF_DELAY  -> publish "OFF"  to `display`

The app reacts to `display`:  "ON" -> WakeService wakelock (screen on, shows
MagicMirror²);  "OFF" -> DeviceAdmin lockNow() (screen off).  Neither the
firmware nor the app is modified — this script is the whole coupling.

WHY a separate bridge (not the app, not the firmware):
  * Firmware publishes raw topics only (CLAUDE.md §9.5); presentation/threshold
    logic lives off-device.
  * The app speaks plain MQTT on localhost:1883 with simple ON/OFF strings; the
    radar lives on the mTLS :8883 listener as JSON.  This bridge is the adapter.

DEPLOYMENT (tablet, Termux) — one-time setup:
  1. mosquitto must expose BOTH listeners:
       listener 8883  (mTLS, require_certificate true)   <- firmware + this read
       listener 1883 127.0.0.1  (plain, localhost)        <- the app + this write
     (allow_anonymous true on the 1883 localhost listener, or per-listener ACL.)
  2. Copy a reader cert to the tablet — the project mirror bundle works
     (ACL `read rmms/+/+`):
       out/mirror-<id>/{ca.crt,cert.pem,key.pem}  ->  ~/rmms/mirror/
  3. pip install paho-mqtt   (Termux: `pip install paho-mqtt`)
  4. Run:  python tablet_presence_screen.py
     (demo_start.sh can launch it each session — see that script.)

Defaults assume the Termux layout above; override with flags/env if different.
"""

import argparse
import json
import os
import sys
import threading
import time

try:
    import paho.mqtt.client as mqtt
except ImportError:
    sys.exit("paho-mqtt not installed — run: pip install paho-mqtt")

# ── Monotonic clock (immune to wall-clock changes) ───────────────────────────
now = time.monotonic


def log(msg: str) -> None:
    # Wall-clock stamp for human logs; the state machine uses monotonic time.
    print(f"[presence-screen {time.strftime('%H:%M:%S')}] {msg}", flush=True)


class PresenceScreen:
    """State machine: debounced radar presence -> screen ON/OFF commands."""

    def __init__(self, args):
        self.a = args
        # Latest debounced inputs (None until the first usable radar sample).
        self.present = None          # bool: last radar presence
        self.since = now()           # monotonic time `present` last changed
        self.last_msg = 0.0          # monotonic time of last usable radar msg
        self.screen = None           # "ON" / "OFF" / None(unknown at start)
        self.last_on_pub = 0.0       # monotonic time we last (re)sent "ON"
        self.lock = threading.Lock()
        self.pub = None              # plain :1883 client (set in run())

    # ── radar ingestion (mTLS sub callback) ─────────────────────────────────
    def on_radar(self, _client, _ud, msg):
        try:
            env = json.loads(msg.payload.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            return
        # §9.2.1 quality: 3 = invalid — ignore (don't update presence on junk).
        if int(env.get("q", 0)) == 3:
            return
        v = env.get("v", env)
        if "presence" not in v:
            return
        p = bool(v["presence"])
        with self.lock:
            self.last_msg = now()
            if p != self.present:
                self.present = p
                self.since = now()
                log(f"radar presence -> {p}")

    # ── publish helper (plain :1883) ────────────────────────────────────────
    def send(self, cmd: str):
        if self.pub is None:
            return
        self.pub.publish(self.a.display_topic, cmd, qos=1, retain=False)
        log(f"published '{cmd}' to {self.a.display_topic}")

    # ── periodic evaluation of the debounced state machine ──────────────────
    def evaluate(self):
        with self.lock:
            present = self.present
            held = now() - self.since
            stale = (self.last_msg > 0) and (now() - self.last_msg > self.a.stale)

        # No radar yet: do nothing (don't touch the screen on a cold start).
        if present is None:
            return

        # Device went quiet (offline / Wi-Fi drop): treat as absent so the
        # screen doesn't stay on forever after the sensor disappears.
        if stale:
            if self.screen != "OFF":
                log(f"radar stale > {self.a.stale}s — forcing OFF")
                self.send("OFF")
                self.screen = "OFF"
            return

        if present and held >= self.a.on_delay:
            if self.screen != "ON":
                self.send("ON")
                self.screen = "ON"
                self.last_on_pub = now()
            elif self.a.keepalive > 0 and now() - self.last_on_pub >= self.a.keepalive:
                # Re-assert ON so a short system screen-timeout can't dim it
                # mid-presence. Harmless on a never-timeout kiosk display.
                self.send("ON")
                self.last_on_pub = now()
        elif (not present) and held >= self.a.off_delay:
            if self.screen != "OFF":
                self.send("OFF")
                self.screen = "OFF"

    # ── wiring ──────────────────────────────────────────────────────────────
    def run(self):
        # Reader: mTLS to :8883, subscribe rmms/+/radar.
        sub = mqtt.Client(client_id="presence-screen-sub", clean_session=True)
        sub.tls_set(ca_certs=self.a.ca, certfile=self.a.cert, keyfile=self.a.key)
        if self.a.insecure:
            sub.tls_insecure_set(True)
        sub.message_callback_add(self.a.radar_topic, self.on_radar)

        def on_connect(c, _ud, _flags, rc):
            if rc == 0:
                log(f"mTLS connected {self.a.host}:{self.a.tls_port}; "
                    f"subscribing {self.a.radar_topic}")
                c.subscribe(self.a.radar_topic, qos=1)
            else:
                log(f"mTLS connect failed rc={rc}")

        sub.on_connect = on_connect
        sub.reconnect_delay_set(min_delay=1, max_delay=30)

        # Writer: plain to :1883 (localhost) for the app's `display` topic.
        self.pub = mqtt.Client(client_id="presence-screen-pub", clean_session=True)
        self.pub.reconnect_delay_set(min_delay=1, max_delay=30)

        while True:
            try:
                self.pub.connect(self.a.host, self.a.plain_port, keepalive=30)
                break
            except OSError as e:
                log(f"plain :{self.a.plain_port} connect retry ({e})")
                time.sleep(3)
        self.pub.loop_start()

        while True:
            try:
                sub.connect(self.a.host, self.a.tls_port, keepalive=30)
                break
            except OSError as e:
                log(f"mTLS :{self.a.tls_port} connect retry ({e})")
                time.sleep(3)
        sub.loop_start()

        log(f"running: ON after {self.a.on_delay}s present, "
            f"OFF after {self.a.off_delay}s absent, keepalive={self.a.keepalive}s")
        try:
            while True:
                self.evaluate()
                time.sleep(self.a.eval_interval)
        except KeyboardInterrupt:
            log("stopping")
        finally:
            sub.loop_stop()
            self.pub.loop_stop()


def main():
    home = os.path.expanduser("~")
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=os.environ.get("BROKER_HOST", "127.0.0.1"))
    ap.add_argument("--tls-port", type=int, default=8883)
    ap.add_argument("--plain-port", type=int, default=1883)
    ap.add_argument("--radar-topic", default="rmms/+/radar")
    ap.add_argument("--display-topic", default="display")
    # Mirror cert bundle (read rmms/+/+). Defaults to ~/rmms/mirror/ on the tablet.
    ap.add_argument("--ca",   default=os.environ.get("MIRROR_CA",   f"{home}/rmms/mirror/ca.crt"))
    ap.add_argument("--cert", default=os.environ.get("MIRROR_CERT", f"{home}/rmms/mirror/cert.pem"))
    ap.add_argument("--key",  default=os.environ.get("MIRROR_KEY",  f"{home}/rmms/mirror/key.pem"))
    ap.add_argument("--insecure", action="store_true",
                    help="skip broker-cert hostname check (self-signed test CA)")
    # Debounce tuning.
    ap.add_argument("--on-delay",  type=float, default=2.0,
                    help="seconds of sustained presence before screen ON")
    ap.add_argument("--off-delay", type=float, default=10.0,
                    help="seconds of sustained absence before screen OFF")
    ap.add_argument("--keepalive", type=float, default=30.0,
                    help="re-assert ON every N s while present (0 = never)")
    ap.add_argument("--stale", type=float, default=15.0,
                    help="no radar for N s -> force OFF (device offline)")
    ap.add_argument("--eval-interval", type=float, default=0.5)
    args = ap.parse_args()

    for path in (args.ca, args.cert, args.key):
        if not os.path.exists(path):
            sys.exit(f"reader cert missing: {path}\n"
                     "Copy out/mirror-<id>/{ca.crt,cert.pem,key.pem} to the tablet "
                     "(see this script's header), or pass --ca/--cert/--key.")

    PresenceScreen(args).run()


if __name__ == "__main__":
    main()
