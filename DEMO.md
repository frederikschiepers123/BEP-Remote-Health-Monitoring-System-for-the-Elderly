# Supervisor demo — runbook

**Scope of this session:** prove the end-to-end secure path between the
sensor module (firmware on Pico 2 W) and the tablet broker, plus walk through
the architecture and the mirror UI work. No sensors are wired in this
session (constraint: insufficient jumper cables this morning); everything
the supervisor sees here is real hardware running real mTLS + MQTT, with the
sensor side stubbed by a heartbeat message until next session.

Total demo time: **~12 minutes**, plus questions.

---

## 0. Setup

Network: **phone's mobile hotspot** as the private Wi-Fi (proven at
`172.20.10.x`). The tablet's IP changes every hotspot reconnect, which
means the broker cert SAN and the Pico's `/cfg/broker.json` both need
to track today's value. The script below does that automatically.

You need: phone hotspot on, laptop + tablet + Pico 2 W on it, one
WSL2 terminal, one PowerShell window.

### 0.0 One-time prep (do once per laptop, never again)

```bash
# Passwordless SSH to the tablet (asks Termux password once):
./scripts/setup_tablet_ssh.sh <tablet-ip>
```

```powershell
# Windows-side port forward so the tablet can reach MM² on the laptop
# (run from PowerShell as Administrator):
netsh interface portproxy add v4tov4 listenaddress=0.0.0.0 listenport=8080 `
    connectaddress=$(wsl -d Ubuntu -e bash -c "ip -4 addr show eth0 | grep inet | awk '{print \$2}' | cut -d/ -f1") `
    connectport=8080
netsh advfirewall firewall add rule name="MM2 8080" dir=in action=allow `
    protocol=TCP localport=8080
```

### 0.1 Get today's tablet IP

In Termux on the tablet:

```bash
pgrep sshd || sshd
ifconfig 2>/dev/null | grep 'inet ' | grep -v 127.0.0.1
```

Note `<TIP>` (something like `172.20.10.14`).

### 0.2 Laptop-side bring-up — one command

```bash
./scripts/demo_start.sh <TIP>
```

What this does (~15 s + one MagicMirror² boot):

1. Re-mints the broker cert for `<TIP>` and bounces Mosquitto on the
   tablet over SSH.
2. Edits `MagicMirror/config/config.js` to point the bridge at
   `mqtts://<TIP>:8883`.
3. Rewrites `broker.json` in the most-recent `out/device-*/` bundle
   to use `<TIP>` and copies the whole bundle + `provision_device.py`
   to `C:\Users\frede\`.
4. Kills any stale MagicMirror on `:8080` and relaunches it in the
   background, logging to `/tmp/mm2.log`.

When it returns, the laptop side is done. Output ends with the manual
checklist for the next step.

### 0.3 Pico-side bring-up — three drag-and-drops

All from PowerShell at `C:\Users\frede\`. The UF2s should already be
present from the last build; if not, rebuild with
`cmake --build build --target bringup_provision bringup_sensors -j$(nproc)`
and copy them across.

1. **Push today's broker config to the Pico's littlefs.**
   Hold BOOTSEL on the Pico, plug it in, drop `bringup_provision.uf2`
   onto the RP2350 drive. Then in PowerShell:

   ```powershell
   cd C:\Users\frede
   py -3.13 provision_device.py COM<N> device-<uuid>
   ```

   (Replace `COM<N>` with whatever Device Manager shows after the
   Pico re-enumerates. The script ends with `all artefacts verified ✓`
   and reboots the device. Takes ~3 s.)

2. **Flash the running firmware.** BOOTSEL again, drop
   `bringup_sensors.uf2`. The Pico boots, reads identity + WiFi +
   broker from littlefs, joins WiFi, completes mTLS to the broker,
   and starts publishing `rmms/<uuid>/env`, `/air`, `/radar` at
   1 Hz / 0.2 Hz / 1 Hz.

3. **Open the mirror in a browser.** `http://localhost:8080` on the
   laptop, or `http://172.20.10.2:8080` (laptop IP on the hotspot) on
   the tablet. Tiles populate within ~1 s of first publish.

### 0.4 Watch it work

```bash
tail -f /tmp/mm2.log
```

You should see, in order:

```
Ready to go!  ...
MMM-CustomMQTTBridge connecting: mqtts://172.20.10.14:8883 TLS=true ...
MQTT connected — subscribing to: [ 'rmms/+/+' ]
Subscribed.
```

The Pico's PuTTY console (COM<N>, 115200) shows:

```
[bringup] UUID = <uuid>
[bringup] broker = 172.20.10.14:8883
[bringup] CONNECTED — IP = 172.20.10.X
[bringup] MQTT CONNACK status=0 ACCEPTED
[bringup] env T=22.6C H=45.7% P=1014.2hPa seq=1
[bringup] radar pres=1 dist=287mm BR=21.0 HR=89.0 q=0 seq=1
```

On the tablet's Mosquitto window you see `Received PUBLISH from
<uuid>` lines for `/env`, `/air`, `/radar`, and `/status`.

### 0.5 If something looks wrong

| Symptom | Likely cause | Fix |
|---|---|---|
| Pico keeps printing `mqtt reconnect → … (backoff …)` | broker IP in `broker.json` doesn't match today's `<TIP>` | re-run §0.2 + §0.3 step 1 |
| Bridge log says `Hostname/IP does not match certificate's altnames` | broker cert SAN doesn't include today's `<TIP>` | re-run §0.2 (it re-mints the cert) |
| Bridge log says `ECONNREFUSED` | Mosquitto not running on tablet | re-run §0.2 (it restarts mosquitto) |
| MagicMirror says `PORT IN USE: 8080` | old MM² process still bound | `ss -tlnp | grep 8080`, `kill <pid>`, rerun `npm run server` from `MagicMirror/` |
| Tiles show "Measuring..." but PuTTY shows publishes | bridge not connected to broker (TLS / IP / cert mismatch) | `tail /tmp/mm2.log`, fix per the row above |

---

## 1. The demo itself

### 1.1 — Show the architecture poster (~2 min)

Open `CLAUDE.md` in the IDE. Walk through:

- **§1** — three-tier architecture diagram: sensor module → tablet (broker +
  MagicMirror²) → Radxa → hospital FHIR.
- **§2.1** — both transports (USB-CDC + Wi-Fi) use uniform mTLS. v1 scope:
  Wi-Fi only; USB-CDC deferred.
- **§9.5** — mirror UI boundary: MagicMirror² subscribes raw, threshold
  logic in JavaScript on the mirror.
- **§10.2** — security model: ECDSA P-256, static device certs, project CA
  off-device. Three identities (device, mirror, operator) all from one CA.

One sentence per section. The supervisor doesn't need to read it line by
line — show the structure, show that it's authoritative.

### 1.2 — Show the live Pico → tablet publish flow (~3 min)

Three windows visible side by side:

- **Tablet (Termux), Mosquitto's verbose log** — logs every connection,
  every TLS handshake, every PUBLISH.
- **Pico (PuTTY on COM3)** — the firmware's own debug output: WiFi join,
  altcp_tls config, CONNACK, PUBLISH every 10 s.
- **Laptop (WSL2)** — open a subscriber with `mosquitto_sub` so the
  supervisor sees the actual JSON arriving from the Pico's perspective:

```bash
cd ~/projects/BEP-Remote-Health-Monitoring-System-for-the-Elderly
DEVDIR=$(ls -dt out/device-* | head -1)
UUID=$(basename "$DEVDIR" | sed 's/^device-//')
mosquitto_pub -h <TIP> -p 8883 \
    --cafile ~/rmms-ca/ca.crt \
    --cert  "$DEVDIR/dev.crt.pem" \
    --key   "$DEVDIR/dev.key.pem" \
    -i      "$UUID" \
    -t      "rmms/$UUID/log" -m "supervisor demo $(date +%H:%M:%S)" -q 1 -r
```

That's a publish from the laptop side using the same device cert; show
that it lands on the broker too. Now both ends prove the same wire path.

**Talking points to make**:

- "The Pico is connecting over TLS 1.2 with ECDHE-ECDSA on a P-256 curve.
  The broker is enforcing client certs — the tablet's Mosquitto log shows
  the cert's Common Name as the username, which we use for ACL gating."
- "Each topic is under `rmms/<device-uuid>/...`. The ACL is keyed on the
  cert's Common Name, so devices can only publish to their own subtree."
- "Right now the Pico publishes a heartbeat. When the sensors are wired up
  it publishes the same way to `rmms/<uuid>/env`, `/air`, `/radar` — the
  driver code's already in place (`components/sensor_env/bme280.c`); we
  needed more jumper cables than we had this morning. Tomorrow we'll wire
  up the BME280 and the demo extends naturally."

### 1.3 — Show the provisioning script (~2 min)

Open `scripts/provision_ca.sh`. Walk through:

- One ECDSA P-256 project CA, generated once, **stays on the
  provisioning host** (`~/rmms-ca/ca.key`, mode 600, gitignored).
- Per-device certs issued from that CA. Device cert CN = device UUID.
- Two non-device identities also issued by the same CA: a `mirror-<id>`
  cert that the MagicMirror² uses for read-only access to the entire
  device tree, and an `operator-<id>` cert that lets the laptop (PoC) or
  Radxa (production) push `info` / `screen` topics to the mirror.
- The generated Mosquitto ACL file uses `pattern write rmms/%u/#` for
  devices and explicit `user <cn>` blocks for mirror + operator.

**Talking point**: "The audit of the previous BAP's firmware found an
unencrypted CA private key committed to their public repo. Our CA never
touches Git — `.gitignore` blocks `*.key/*.pem/*.der/*.crt/*.csr/out/`
unconditionally; the script's design forces the CA dir to live in
`$HOME/rmms-ca/` outside the repo."

### 1.4 — Show the MagicMirror² mirror UI (Yasmina's work) (~3 min)

`MagicMirror/modules/MMM-SensorUI/MMM-SensorUI.js` and `MMM-SensorUI.css`.

Walk through:

- The mirror is a custom MM² module that consumes raw MQTT topics directly
  via `MMM-CustomMQTTBridge`. No intermediate "tablet UI service" — the
  threshold logic lives in JavaScript on the mirror.
- Tile layout: vital wrapper (heart rate + breath rate), env wrapper
  (temperature + humidity + air quality), info wrapper (text from
  operator).
- Severity colors: green / yellow / orange / red, mapped per sensor in
  the JS — WHO/AHA/ASHRAE ranges as starting points (CLAUDE.md "MM-UI #2"
  documents the exact ranges).
- Until sensor data arrives, tiles render in "Measuring..." mode with the
  Font Awesome icons but no values.

**Talking point**: "When the firmware is wired up to real sensors and the
mirror is running on the tablet, the entire vital-display flow is:
Pico → tablet broker → MM-CustomMQTTBridge → MMM-SensorUI tile → behind
the two-way acrylic. We've proven every leg of that path independently
this session."

### 1.4.5 — The actual mirror running with live data (~2 min)

This is the headline demo. Three things visible at the same time:

- **Tablet's web browser** at `http://<laptop-ip>:8080` showing
  MagicMirror² with tiles for heart rate, breath rate, temperature,
  humidity, air quality. Tiles light up with live values from the Pico
  within seconds.
- **Pico's OLED** cycling through four pages (Network/Broker → Env →
  Air → Build) — same data the mirror displays, on the device itself.
- **Tablet's Mosquitto window** logging incoming `PUBLISH from <uuid>`
  for `/env` and `/air` every 1–5 seconds.

**Talking point:** "Three independent views of the same data. The mirror
on the tablet is the user-facing layer; the OLED is on-device confirmation;
the Mosquitto log is what the Radxa would see for FHIR translation. Each
view confirms the path independently. The whole chain is over mTLS —
break any cert and the whole thing stops working immediately."

### 1.5 — Walk through the bring-up ladder (~2 min)

`CLAUDE.md §15` — the 17-step bring-up order.

Show that steps 1-6 are **confirmed on hardware** this session:

| Step | Confirmed |
|---|---|
| 1 Blink | ✅ |
| 2 FreeRTOS hello | ✅ |
| 3 USB-serial dev console | ✅ |
| 4 littlefs mount + atomic config writes | ✅ |
| 5 Wi-Fi associate + DHCP | ✅ |
| 6 mbedTLS + MQTT publish over Wi-Fi | ✅ this morning, running right now |

Steps 7-17 are pending: sensor drivers (next session), transport selector,
end-to-end identity test against production ACL, soak test. The architecture
they walk into in step 17 isn't being designed — it's the same one being
demoed in step 6.

### 1.6 — Hand off questions

Be ready to answer:

- **"Why TLS 1.2 not 1.3?"** — pico-sdk's pico_mbedtls library doesn't
  compile the TLS 1.3 implementation sources in this version. CLAUDE.md
  notes this and the config is set so a future SDK update flips it on.
- **"Why is the BME280 wired but you didn't demo it?"** — jumper cable
  shortage this morning. The driver compiles and the bring-up UF2 is
  already staged (`bringup_sensors.uf2`); next session is hardware-wiring
  and live sensor data.
- **"How big is the firmware?"** — ~900 KB UF2 including FreeRTOS,
  lwIP, mbedTLS, TinyUSB, the cyw43 firmware blob, our application
  code, and the device cert. Comfortably inside the Pico 2's 4 MB flash.
- **"What happens if Wi-Fi drops?"** — exponential backoff reconnect on
  the firmware side; broker has Last Will and Testament set to
  `rmms/<uuid>/status = offline` (retained), so the mirror sees the
  device as offline within the keepalive window (60 s on Wi-Fi).
- **"What does the Radxa do?"** — subscribes to the same raw topics,
  aggregates and buffers them, translates to FHIR `Observation` resources
  with LOINC/UCUM codes, posts to the hospital FHIR endpoint. None of
  that lives in this firmware repo (`CLAUDE.md §9.6`).

---

## Plan B — if something is broken at demo time

If the Pico can't reach the broker (Wi-Fi issue, hotspot IP churn,
Mosquitto crashed), pivot to the **laptop ↔ tablet only** demo. Same
contract, no Pico needed:

```bash
# Open two WSL2 tabs. In each, set up vars:
cd ~/projects/BEP-Remote-Health-Monitoring-System-for-the-Elderly
DEVDIR=$(ls -dt out/device-* | head -1)
UUID=$(basename "$DEVDIR" | sed 's/^device-//')

# Tab A — publish a retained message first (QoS 1 so it definitely lands):
mosquitto_pub -h <TIP> -p 8883 \
    --cafile ~/rmms-ca/ca.crt \
    --cert  "$DEVDIR/dev.crt.pem" \
    --key   "$DEVDIR/dev.key.pem" \
    -i      "$UUID" \
    -t      "rmms/$UUID/cmd" -m '{"cmd":"activate"}' -q 1 -r

# Tab B — subscribe; receives the retained message immediately:
mosquitto_sub -h <TIP> -p 8883 \
    --cafile ~/rmms-ca/ca.crt \
    --cert  "$DEVDIR/dev.crt.pem" \
    --key   "$DEVDIR/dev.key.pem" \
    -i      "$UUID" \
    -t      "rmms/$UUID/cmd" -v
```

(Note: `/cmd` is used here instead of `/log` because the ACL allows the
device cert to *subscribe* only to its own `/cmd` topic. For the live Pico
demo the Pico publishes `/log` and the broker side shows it — different
direction, both allowed by the ACL.)

That alone proves: project CA + mTLS handshake + ACL enforcement +
Mosquitto on Termux.

If even Wi-Fi is broken, walk the supervisor through the repo:
`CLAUDE.md`, `scripts/provision_ca.sh`, `MagicMirror/modules/MMM-SensorUI/`,
`test/bringup/`. The narrative of the work stands without a live demo.

---

## After the demo

- Update CLAUDE.md §15 step 6 from "confirmed" to also mark anything new.
- Push any local changes (`git push origin main`).
- Wire up the BME280 → ENS160 → radar in the next session.
