# Radar-presence → tablet-screen coupling

Turn the tablet screen **on** when the radar sees a person, **off** when it
doesn't — so the MagicMirror² display only lights up when someone's there.

## The chain

```
┌──────────┐  rmms/<uuid>/radar   ┌─────────────────────────┐  display   ┌─────────────┐
│ firmware │ ───(8883 mTLS)─────► │ tablet_presence_screen  │ ─(1883)──► │ Health-     │
│ MR60BHA2 │   {"v":{"presence":  │ .py  (Termux)           │   "ON" /   │ MonitorWake │
│          │      true,...}}      │  debounce + state mach. │   "OFF"    │ Test app    │
└──────────┘                      └─────────────────────────┘            └──────┬──────┘
                                                                                │
                                       "ON"  → WakeService wakelock (screen on) │
                                       "OFF" → DeviceAdmin lockNow() (screen off)
```

Three independent pieces, each unchanged by the others:

| Piece | Role | Repo location |
|---|---|---|
| **Firmware** | publishes raw `rmms/<uuid>/radar` with `v.presence` (§9.1/§9.2.2) | `components/sensor_radar/`, `transport_mqtt/` |
| **Bridge** | reads radar (mTLS), debounces, writes `ON`/`OFF` to `display` (plain) | `scripts/tablet_presence_screen.py` |
| **App** | `display` `ON` → wake screen; `OFF` → lock screen | `HealthMonitorWakeTest/` (Yasmina) |

The firmware only emits raw topics; the presence→screen *policy* lives off-device
(CLAUDE.md §9.5). The app speaks plain MQTT + simple `ON`/`OFF` strings; the radar
is JSON on the mTLS listener. The bridge is the adapter between the two.

## Debounce behaviour

- Person present, sustained ≥ `--on-delay` (default **2 s**) → publish `ON`.
- Absent, sustained ≥ `--off-delay` (default **10 s**) → publish `OFF`.
- While present, re-assert `ON` every `--keepalive` s (default **30 s**) so a
  short system screen-timeout can't dim it mid-presence (harmless on a
  never-timeout kiosk display).
- No radar for `--stale` s (default **15 s**, e.g. Wi-Fi drop / device offline)
  → force `OFF`, so the screen doesn't stay lit after the sensor disappears.
- Quality `q=3` (invalid) radar samples are ignored; brief ghost detections are
  smoothed out by the on-delay.

Tune for the room — e.g. a longer `--off-delay` so the mirror doesn't blink off
when someone briefly steps out of radar range.

## Prerequisites (tablet, one-time)

1. **mosquitto with two listeners:**
   - `8883` mTLS, `require_certificate true` (firmware + the bridge's read side).
   - `1883` **127.0.0.1** plain (the app's IPC channel + the bridge's write side).
     This localhost-only plain listener is the app's design (Yasmina); it is
     *not* a network-facing second broker listener. A network-exposed plain
     listener would contradict CLAUDE.md §19.1 and need an ADR — keep `:1883`
     bound to `127.0.0.1`.
2. **Reader cert:** the project mirror bundle (`out/mirror-<id>/`, ACL
   `read rmms/+/+`) copied to `~/rmms/mirror/{ca.crt,cert.pem,key.pem}`.
3. `pip install paho-mqtt` in Termux.
4. The app installed, **Device Admin granted** (for `lockNow()`), and running
   (BootReceiver auto-starts it on boot).

## Running it

**Automatic:** `scripts/demo_start.sh <tablet-ip>` pushes the mirror cert + the
bridge to the tablet, installs paho-mqtt, and starts it (step 2b). Guarded — it
warns and continues if a prerequisite is missing.

**Manual** (on the tablet, after the prerequisites):
```bash
python tablet_presence_screen.py --insecure
# --insecure skips only the broker-cert hostname check for the 127.0.0.1
# connection (the CA still validates the broker cert; mutual auth still happens).
```

## Testing

1. **App path alone** (no radar): `mosquitto_pub -h 127.0.0.1 -p 1883 -t display -m ON`
   should wake the screen; `-m OFF` should lock it. Confirms the app + `:1883`.
2. **Bridge alone:** run it, then publish a fake radar sample to the mTLS broker
   with the operator/mirror cert and watch its log print `radar presence -> True`
   then `published 'ON'`.
3. **End-to-end:** with the firmware connected and publishing radar, walk in and
   out of the sensor's range and watch the screen follow (after the on/off delays).
