# Running the end-to-end demo

The live demo: a provisioned Pico publishes real sensor data (radar vitals,
env, air, light) over Wi-Fi + mTLS to the tablet's Mosquitto broker, and the
MagicMirror² behind the acrylic renders the tiles. Total setup ≈ 1 minute once
the parts have been provisioned.

## Prerequisites (one-time)

1. A provisioned Pico — certs + `/cfg` in littlefs (`docs/provisioning.md`),
   flashed with `bringup_sensors.uf2` (or `bringup_sensors_slow.uf2`, the
   same image at a 100 kHz I²C clock, for boards with marginal bus wiring).
   Both include the 7 OLED troubleshooting pages.
2. A deployed tablet — broker + MagicMirror² + wake app via
   `scripts/deploy_home_kit.sh`; reboot-proof through Termux:Boot
   (`scripts/tablet_boot.sh`).
3. A **phone hotspot** for networking. Not eduroam — it blocks
   client-to-client traffic. On an iPhone, keep *Maximize Compatibility* ON.

## Each session

```bash
scripts/demo_start.sh <tablet-ip> [device-uuid]
```

That refreshes the broker cert for today's IP, starts the `tablet.local`
mDNS responder, and starts the radar-presence→screen bridge — see the script
header for the fixed-demo-UUID note. The mirror itself runs **on the tablet**
via Termux:Boot (`scripts/tablet_boot.sh`); if it's down, restart it there,
not from the laptop. Then power the Pico. Because the device resolves the
broker by name (`tablet.local`), a changed hotspot IP needs **no
re-provisioning and no reflash**.

Watch raw data with the mirror cert if needed (the operator cert cannot —
its ACL is write-only for `info`/`screen`):

```bash
mosquitto_sub -h <tablet-ip> -p 8883 --cafile out/broker/ca.crt \
  --cert out/mirror-<id>/cert.pem --key out/mirror-<id>/key.pem \
  -t 'rmms/+/+' -v
```

## If something doesn't come up

Work the table in `docs/handover.md` ("known hardware gotchas") — it covers
every failure this group actually hit: hotspot join gating, tablet Doze
killing the broker, radar distance limits, ghost presence from a wall, I²C
pull-ups, ENS160 warm-up. The OLED troubleshooting pages
(`bringup_sensors_slow`) show link state and per-sensor health on-device.

The June 2026 supervisor-demo snapshot this file replaced is archived at
`docs/archive_demo_2026-06.md`.
