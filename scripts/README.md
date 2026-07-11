# scripts/

Operational tooling for the RMMS pipeline — building and provisioning the Pico
sensor module, standing up the Termux tablet tier (Mosquitto broker +
MagicMirror²), and running demos. Each script's header comment is the
authoritative usage doc; this file is only an index.

## Index

| Script | Tier | Purpose |
| --- | --- | --- |
| `build.sh` | firmware | Compile the sensor-module firmware (`build/main/sensor_module.uf2` + `.elf`); optional `--flash` (BOOTSEL) / `--swd` (OpenOCD). Firmware only — no certs, no littlefs content. |
| `provision_ca.sh` | provisioning | Generate the project CA (once) plus broker, mirror, operator, and per-device certs (all ECDSA P-256). CA private key never leaves `$CA_DIR`. |
| `provision_device.py` | provisioning | Push a device bundle (certs + `/cfg` JSON) into the Pico over USB-serial; pairs with the `bringup_provision` UF2, verifies SHA-256 per file, reboots the device. |
| `deploy_home_kit.sh` | provisioning | One-command, idempotent orchestrator for a full home kit (certs → tablet → Pico → SBC), plus a `fleet` mode that loops a TSV manifest. Calls the per-tier tools above. |
| `refresh_broker.sh` | tablet | One-shot broker refresh: re-issue the broker cert for today's tablet IP, scp it to Termux, restart Mosquitto, verify :8883 answers. |
| `setup_tablet_ssh.sh` | tablet | One-time: copy the laptop's SSH public key onto the tablet so `refresh_broker.sh` runs unattended. |
| `tablet_boot.sh` | tablet | Termux:Boot entry point — an Android boot brings up sshd, the broker, the mDNS responder, the presence bridge, and MagicMirror² with no laptop involved. IP-independent and idempotent. |
| `tablet_mdns_responder.py` | tablet | Advertises a stable `tablet.local` name (python-zeroconf, RFC 6762 legacy-unicast) so the Pico finds the broker by name across IP changes. |
| `tablet_presence_screen.py` | tablet | Bridges radar `v.presence` (mTLS `rmms/+/radar`) to the wake/lock app's plain-MQTT `display` topic ("ON"/"OFF") with debounce — screen follows presence. |
| `tablet_start_magicmirror.sh` | tablet | Launches MagicMirror² in server-only mode (`npm run server`), opens Chrome at `localhost:8080`, restarts MM² with backoff if it dies. |
| `demo_start.sh` | demo | One-shot demo bring-up from the laptop: refresh broker cert + restart Mosquitto, start the `tablet.local` mDNS responder, start the radar-presence→screen bridge, refresh the fixed demo-device bundle (first-provision only). MagicMirror runs on the tablet via Termux:Boot — this script does not start it. |
| `test_broker_acl.sh` | demo | Broker mTLS + ACL enforcement harness (7 assertions: own-subtree write, cross-device deny, operator scope, `/cmd` read, anonymous refused). Exit 0 only if all hold. |
| `mock_radxa.py` | SBC | Read-only dev sniffer: subscribe to one device's raw topics, decode the §9.2 envelope, pretty-print values. Its old Radxa-stand-in role is dead — run `sbc/` for real aggregator behaviour. |

`scripts/deprecated/` holds superseded scripts (currently the previous group's
tablet `deploy.sh` and its PDF instructions) — see its own README.

## Typical flows

**Fresh device provisioning**

1. `scripts/provision_ca.sh` — mint (or reuse) the CA and issue the device
   bundle into `out/device-<uuid>/`. For a whole home kit in one go, use
   `scripts/deploy_home_kit.sh` instead — it orchestrates this plus the tablet
   and SBC tiers.
2. Flash the `bringup_provision` UF2, then
   `scripts/provision_device.py /dev/ttyACM0 out/device-<uuid>` to write certs
   and config into littlefs.
3. `scripts/build.sh --flash` (or `--swd`) to build and flash the production
   firmware.

**Demo startup (laptop-driven)**

`scripts/demo_start.sh <tablet-ip>` does the whole session bring-up: broker
cert refresh + Mosquitto restart, `tablet.local` mDNS responder, and the
radar-presence→screen bridge. MagicMirror itself runs on the tablet via
Termux:Boot (`tablet_boot.sh`) and is not started from the laptop. Thanks to
mDNS, the demo Pico is provisioned once and never re-flashed when the IP
changes.

**Tablet self-heal (no laptop)**

`scripts/tablet_boot.sh`, symlinked from `~/.termux/boot/10-rmms`, makes an
Android reboot self-healing: Termux:Boot fires it post-unlock and it restarts
every tablet service, guarded by `pgrep` so double-launches are impossible.
One-time prerequisites (Termux:Boot from F-Droid, battery-optimization off)
are listed in its header.
