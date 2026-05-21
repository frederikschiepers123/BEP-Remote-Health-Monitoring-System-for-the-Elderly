# CLAUDE.md — RMMS Tablet

> Read this file completely before writing or modifying any code in this repository.
> It is the single source of truth for the tablet-side stack: MQTT broker,
> USB-MQTT bridge, MagicMirror², and Android autostart glue.

---

## 1. Project context

This repository contains the **tablet-side software** for the Remote Medical
Monitoring System (RMMS) — a TU Delft BSc Applied Project (BAP). It runs on
an **Android tablet** mounted behind a two-way acrylic mirror, serving two
simultaneous roles:

1. **MQTT broker** (Mosquitto under Termux) — the central message bus for
   the sensor module, the Radxa aggregator, and the mirror UI.
2. **Smart mirror client** (MagicMirror² in a Chrome kiosk) — the ambient
   display for the resident.

The tablet is the hub of a three-tier system:

```
Sensor Module ──► Tablet ──────────────► Radxa Dragon Q6A ──► Hospital FHIR
   (firmware     (THIS REPO)              (Radxa repo)          (external)
   repo)
```

This repo contains **everything that runs on the tablet** and nothing else.

The previous BAP group did not have an equivalent — they put the broker on
their SBC, which was the wrong architectural call (see firmware repo
`CLAUDE.md §1` and the technical audit). The tablet-as-broker model
sidesteps several reliability issues at the cost of operational complexity
(Android lifecycle, Termux quirks). This repo manages that complexity.

---

## 2. Components

The tablet runs four things, in roughly this dependency order:

| Component | What it is | Owner |
|---|---|---|
| Mosquitto | MQTT broker, mTLS on `:8883` | `mosquitto/` directory |
| USB-MQTT bridge | Transparent byte pipe from `/dev/ttyACM0` to `localhost:8883` | `bridge/` directory |
| MagicMirror² | Smart mirror UI, runs in Chrome kiosk | `magicmirror/` directory |
| Termux:Boot scripts | Glue that brings 1–3 up at device boot | `autostart/` directory |

Each component has its own subdirectory with its own configs, scripts, and
README. This CLAUDE.md ties them together.

---

## 3. Mosquitto broker

### 3.1 Why Mosquitto and not something else

Mosquitto is in Termux's package repository (`pkg install mosquitto`) and is
the same Mosquitto used everywhere else in this system (workstation dev
broker, integration tests). One implementation, one config syntax, one set
of bugs to know about. Do not substitute mosca, NanoMQ, EMQX, or anything
else.

### 3.2 Configuration

`mosquitto/mosquitto.conf` (deployed to Termux's `$PREFIX/etc/mosquitto/`):

```
listener 8883
protocol mqtt

cafile      /data/data/com.termux/files/home/rmms/certs/ca.der
certfile    /data/data/com.termux/files/home/rmms/certs/tablet.crt
keyfile     /data/data/com.termux/files/home/rmms/certs/tablet.key
require_certificate true
tls_version tlsv1.2

use_identity_as_username true
allow_anonymous false

acl_file    /data/data/com.termux/files/home/rmms/mosquitto/acl

persistence true
persistence_location /data/data/com.termux/files/home/rmms/mosquitto/data/
queue_qos0_messages false
max_queued_messages 10000

log_dest file /data/data/com.termux/files/home/rmms/mosquitto/log/mosquitto.log
log_type notice
log_type warning
log_type error
```

### 3.3 ACL

The ACL is **pattern-based, not per-device**. Adding new devices does not
require touching the broker config:

```
# mosquitto/acl

# Each device can write only under its own UUID prefix and read its own command topic
pattern write rmms/%c/#
pattern read  rmms/%c/cmd
pattern read  rmms/%c/time/set

# The Radxa aggregator has a known CN; full read of raw vitals + publish to UI topics
user radxa-aggregator
topic read rmms/+/env
topic read rmms/+/radar
topic read rmms/+/light
topic read rmms/+/status
topic write rmms/ui/+/#

# MagicMirror² is a read-only subscriber of UI topics
user magicmirror-client
topic read rmms/ui/+/#
```

The `%c` substitution = "the CN from the client's certificate." This is why
`use_identity_as_username true` matters. **Adding a new sensor module is
zero broker-side configuration** — the device's cert CN dictates its
permissions automatically.

### 3.4 Persistence and storage

Mosquitto persists retained messages and queued QoS 1 messages to
`persistence_location` between restarts. On Termux this lives in the
app-private storage area, which survives Termux restarts but not app
uninstalls.

**Do not put the persistence file on the Android shared storage** (`/sdcard`
or `/storage/emulated/0/`). Those are scoped storage with limited fsync
guarantees and are not safe for a database-shaped file.

---

## 4. USB-MQTT bridge

### 4.1 What it is

A **transparent byte pipe** between `/dev/ttyACM0` (the firmware's USB-CDC
serial endpoint) and `localhost:8883` (Mosquitto's mTLS listener). It does
**not** parse MQTT, does **not** terminate TLS, does **not** look at the
bytes. It is `socat` with a service wrapper.

```
Firmware                                      Bridge                                Mosquitto
  │ mTLS  ────► /dev/ttyACMx ────► [byte pipe] ────► localhost:8883 ────► mTLS terminates here
  │                                                                           │
  └─── identical TLS session as the Wi-Fi path ───────────────────────────────┘
```

The bridge being a dumb pipe is **the entire point**. It is what allows the
firmware to use one TLS code path (firmware §8) and Mosquitto to enforce one
ACL model (§3.3 above) regardless of which physical path the device is on.

### 4.2 Implementation

For dev/initial deployment: `socat`. One process per attached device.

```bash
# bridge/run-bridge.sh
exec socat -d \
    "FILE:/dev/ttyACM0,raw,echo=0,b115200" \
    "TCP:localhost:8883"
```

For production: a small Python service (`bridge/bridge.py`) that handles:
- Device hotplug (re-spawn on disconnect/reconnect).
- Multiple simultaneous devices (`/dev/ttyACM0`, `/dev/ttyACM1`, ...).
- Android USB host permissions (more on this below).
- Restart-on-failure within the same process (no need for Termux to
  micromanage).

Termux's USB plugin (`termux-usb`) is required for `/dev/ttyACM*` to appear.
Without it, Android does not expose the device to userspace. Install order:

```bash
pkg install termux-api
# Install the Termux:USB add-on from F-Droid (NOT Google Play)
# On first connect of a Pico, Android shows a permission dialog
termux-usb -l                   # list attached USB devices
termux-usb -e /dev/bus/usb/...  # grant the bridge access
```

### 4.3 What the bridge must NOT do

These are bridge anti-patterns that defeat the architecture:

- **Parse MQTT.** The bridge has no business looking at MQTT framing. If it
  did, it would need to handle TLS, which means it would need device certs,
  which means it would become a man-in-the-middle. Do not.
- **Terminate TLS.** Same reason. The endpoint of the firmware's TLS session
  is Mosquitto, on `localhost:8883`. The bridge is below the TLS layer.
- **Re-encode or buffer beyond what `socat` does natively.** Bytes go in,
  same bytes come out. Latency cost is unmeasurable; introducing buffering
  introduces stalls.
- **Log byte contents.** That would log TLS records, which is useless noise.
  Log connect/disconnect events and counts; that's it.

### 4.4 Android lifecycle

The bridge runs under Termux, which runs as a normal Android app, which
means Android's process lifecycle applies. Specifically:

- **Doze mode** will kill the bridge if the screen is off and the device
  is idle. Solution: the tablet runs with screen-on (it's behind the
  mirror; the mirror UI is the screen).
- **Battery optimization** must be disabled for Termux on
  manufacturer-skinned Android (Samsung, Xiaomi, Huawei, OnePlus all stack
  proprietary killers above stock Doze). Open Settings → Apps → Termux →
  Battery → "Unrestricted" (Samsung) / "No restrictions" (Xiaomi) / similar.
- **Termux:Boot** (separate add-on app) is required for autostart at boot —
  Android does not auto-launch normal apps. Without it, the broker dies
  on every reboot until someone opens Termux manually.

These constraints make the tablet-as-broker model operationally fragile.
Mitigations are in §6. **Confirm the tablet is mains-powered and never
sleeps** before claiming the deployment works.

---

## 5. MagicMirror²

### 5.1 Run mode

**Open question** — see §8. The current plan: Termux + Node.js + headless
Chrome in kiosk mode pointing at `http://localhost:8080`. This is fragile;
alternatives include a packaged Android wrapper or a different smart-mirror
framework entirely.

Until decided, this section documents the MM² path.

### 5.2 Module configuration

`magicmirror/config/config.js` is the standard MagicMirror² config plus the
RMMS-specific module:

```javascript
module.exports = {
  address: "0.0.0.0",
  port: 8080,
  ipWhitelist: ["127.0.0.1"],   // localhost only
  modules: [
    {
      module: "clock",
      position: "top_left",
    },
    {
      module: "MMM-RMMS",          // custom; see below
      position: "middle_center",
      config: {
        mqttHost: "localhost",
        mqttPort: 8883,
        mqttCa:   "/data/data/com.termux/files/home/rmms/certs/ca.der",
        mqttCert: "/data/data/com.termux/files/home/rmms/certs/mirror.crt",
        mqttKey:  "/data/data/com.termux/files/home/rmms/certs/mirror.key",
        deviceUuid: "550e8400-e29b-41d4-a716-446655440000",  // the sensor module this mirror belongs to
      }
    },
  ],
};
```

### 5.3 The MMM-RMMS module

`magicmirror/modules/MMM-RMMS/` is a **custom module**, not MMM-MQTT. The
rationale: MMM-MQTT is a thin string-to-DOM mapper; we need a module that
understands the RMMS UI-topic schema (see Radxa repo §11) and renders
qualitative status, not raw values.

Subscribes to:
- `rmms/ui/<deviceUuid>/presence`
- `rmms/ui/<deviceUuid>/wellness`
- `rmms/ui/<deviceUuid>/ambient`
- `rmms/ui/<deviceUuid>/connection`

Publishes nothing. The mirror is read-only.

UI rules (per the firmware repo §9.5 elderly-UX requirement):
- **No raw numbers.** Heart rate, breath rate, exact temperatures — none of
  these appear on the mirror.
- **No clinical jargon.** "Wellness: OK" not "HR: normal sinus rhythm."
- **No urgency theatre.** Red alerts and beeping are inappropriate for
  ambient passive monitoring. Use colour and shape changes for state
  transitions, not animations.
- **No interactivity.** The mirror is passive. No taps, no swipes — touch
  on the acrylic would be unreliable anyway.

### 5.4 Why not MMM-MQTT directly

MMM-MQTT subscribes to topics and renders the payload as a DOM string. It
has no concept of:
- mTLS (it's a plain MQTT client).
- Multi-field qualitative payloads.
- State transitions and timing (e.g., "show offline if last_seen > 60s ago").

We'd end up with a wrapper that does all the real work, with MMM-MQTT as a
useless intermediary. Just write the module directly.

### 5.5 Cert for the mirror module

The mirror needs its own client cert (`mirror.crt`, CN `magicmirror-client`)
because the broker uses mTLS. The cert grants read-only access to
`rmms/ui/+/#` per the ACL (§3.3). Provisioned during install, same flow as
the Radxa and sensor modules.

---

## 6. Autostart

### 6.1 The dependency chain

Boot order matters:
1. Android boots.
2. Termux:Boot fires.
3. Mosquitto starts (no dependencies).
4. Bridge starts (requires Mosquitto + USB device).
5. MagicMirror² starts (requires Mosquitto + display).

Termux:Boot runs `~/.termux/boot/` scripts on device startup. Each script
should be idempotent and short — long-running services go in
`~/.termux/services/` and are managed by `termux-services` (runit).

### 6.2 Files

```
autostart/
├── boot/
│   └── 00-start-services.sh        # → ~/.termux/boot/
└── services/
    ├── mosquitto/run                # → ~/.termux/services/mosquitto/run
    ├── rmms-bridge/run              # → ~/.termux/services/rmms-bridge/run
    └── magicmirror/run              # → ~/.termux/services/magicmirror/run
```

`00-start-services.sh`:
```bash
#!/data/data/com.termux/files/usr/bin/sh
termux-wake-lock
sv up mosquitto
sleep 2
sv up rmms-bridge
sv up magicmirror
```

`services/mosquitto/run`:
```bash
#!/data/data/com.termux/files/usr/bin/sh
exec 2>&1
exec mosquitto -c $PREFIX/etc/mosquitto/mosquitto.conf
```

`services/rmms-bridge/run` (initial socat version):
```bash
#!/data/data/com.termux/files/usr/bin/sh
exec 2>&1
exec socat -d \
    "FILE:/dev/ttyACM0,raw,echo=0,b115200" \
    "TCP:localhost:8883"
```

`runit` restarts these services on exit automatically — no extra plumbing.

### 6.3 `termux-wake-lock`

**Always called before starting services.** Prevents Android from
suspending Termux. Released only when all services stop (cleanly or via
`termux-wake-unlock`).

Without `termux-wake-lock`, Doze will kill everything within minutes once
the screen goes off. With the wake lock + Doze whitelist (§4.4), the
services stay up.

---

## 7. Installation (the deployment script for the tablet)

`install.sh` runs in Termux and does:

1. Check Termux version (≥ 0.119 for current Termux:USB).
2. Install required packages: `pkg install mosquitto socat openssl
   nodejs-lts python`.
3. Install MagicMirror² (`git clone`, `npm install --production`).
4. Prompt for and store certs in `~/rmms/certs/` (mode 0600).
5. Copy configs from this repo to their Termux paths.
6. Symlink autostart scripts to `~/.termux/boot/` and `~/.termux/services/`.
7. Print install summary with broker IP, listener port, cert paths, and
   the next-steps checklist (open Battery settings, install Termux:Boot
   from F-Droid, install Termux:USB from F-Droid, grant USB device
   permission to Termux).

**Manual steps that cannot be scripted** (Android does not allow it):
- Disabling battery optimization for Termux.
- Installing Termux:Boot and Termux:USB add-ons (must be from F-Droid; not
  on Play Store).
- First-time USB device permission grant on device connect.

Document these in the install script's final output so the operator does
them in the right order.

---

## 8. Open questions (BLOCKING — resolve before claiming v1)

1. **Tablet model and Android version.** Specific tablet not yet selected.
   Critical specs: ≥ 4 GB RAM (Mosquitto + Node.js + Chrome is ~800 MB
   resident), USB-C host mode support, Android 10+ for current Termux
   compatibility, no aggressive vendor process-killing (avoid Xiaomi
   without serious workaround effort).
2. **MagicMirror² run mode.** Termux + Chrome kiosk vs. a packaged WebView
   wrapper vs. a different smart-mirror framework. See §5.1.
3. **Bridge implementation path.** Start with socat; promote to Python
   service when multi-device support is needed. When does that happen?
4. **Termux:Boot reliability across vendor skins.** Confirmed working on
   stock Android; needs validation on whatever tablet is chosen.
5. **mDNS responder on Termux.** The firmware (§8.2 of firmware repo) wants
   to resolve `_mqtt._tcp.local`. Avahi is not in Termux packages.
   Alternatives: Bonjour through Java/JNI (complex), or skip mDNS and use
   static IP (simpler, fragile to DHCP changes). Currently leaning toward
   static IP with a configured fallback.
6. **Time push to firmware.** If the RTC sync mechanism (firmware §16
   question 6) is decided as "tablet pushes time over MQTT," that
   publisher runs from this repo as a small Python script alongside the
   bridge. Owner of that decision is the firmware team, but the
   implementation lands here.

---

## 9. Non-goals (v1)

- Web admin UI for the tablet stack. Operations via SSH-into-Termux or by
  unplugging the tablet and editing files via adb.
- OTA updates of the tablet software. Updates via reinstall.
- Multiple sensor modules per tablet. Architecturally supported (ACL
  pattern matches), but operationally one module per home for v1.
- Tablet-side data analysis. The tablet is a broker and a display, nothing
  else. Analysis lives on the Radxa.
- Tablet-side caching of MagicMirror² state. If the tablet reboots, the
  mirror starts blank until the next sample arrives.
- Voice interaction, accessibility features, multilingual UI. All future
  work.

---

## 10. References

- Firmware repo `CLAUDE.md` — sensor module side, especially §8.2 (Wi-Fi
  transport), §9 (MQTT contract), §9.5 (UI-presentation topic boundary),
  §10 (security model).
- Radxa repo `CLAUDE.md` — aggregator side, especially §11 (UI publisher,
  which defines what topics this tablet's MagicMirror² subscribes to).
- Mosquitto config docs: <https://mosquitto.org/man/mosquitto-conf-5.html>.
- Termux:Boot: <https://github.com/termux/termux-boot>.
- Termux:USB: <https://github.com/termux/termux-usb>.
- Termux Services (runit): <https://wiki.termux.com/wiki/Termux-services>.
- MagicMirror² docs: <https://docs.magicmirror.builders/>.

---

## 11. Instructions for Claude Code specifically

When working in this repository:

- **Always read this file before proposing a change.** Cite the section if
  a proposal conflicts.
- **The bridge is a dumb byte pipe.** Do not add MQTT parsing, TLS
  termination, or content inspection to it. If you find yourself wanting
  to, you are solving the wrong problem.
- **No credentials, certs, or `.env` files in this repo.** All certs live
  on the tablet's filesystem; the install script copies them, never
  commits them.
- **MMM-MQTT is not used.** If you find a reference to it, it's wrong.
  See §5.4.
- **The mirror is read-only.** It subscribes; it does not publish.
- **No raw numeric vitals on the mirror UI.** Per firmware §9.5 and the
  elderly-UX requirement.
- **Mosquitto config is one file.** Do not split into `mosquitto-main.conf`
  + `mosquitto-tls.conf` + an Include directive. The previous group did
  this and the audit catches it (their config required two listeners but
  documented one).
- **Termux is not a generic Linux.** Paths start with
  `/data/data/com.termux/files/`. systemd does not exist. Use runit via
  termux-services for daemons.
- When asked to "make it autostart", first answer: is Termux:Boot
  installed and the wake-lock acquired? If not, no Android-side hack will
  make this work.

### 11.1 Anti-patterns to avoid

| Don't | Do |
|---|---|
| Hardcode the tablet's IP into the firmware or Radxa configs | Use mDNS (when supported) or env-var-driven static config |
| Run Mosquitto on the default :1883 plaintext listener | Only `:8883` with mTLS — single listener, like firmware repo §8 |
| Skip `termux-wake-lock` "to save battery" | The tablet is mains-powered; the wake lock is mandatory |
| Embed the bridge logic into MagicMirror² as a "module" | Three separate processes, three separate failure domains |
| Use `mosquitto_pub` from a shell script as the time-sync publisher | If the firmware needs time, write a small Python service that watches the cert directory and publishes on broker reconnect |
| Use `setInterval` in the MagicMirror² module to poll | The MM² module API supports MQTT message events directly — use them |
