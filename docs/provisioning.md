# Provisioning — CA, certificates, and device identity

How a fresh workstation goes from nothing to a fully provisioned fleet:
project CA → broker/mirror/operator bundles → per-device bundles → files on
the Pico's littlefs. This is the operational companion to `CLAUDE.md §9.4`
(identity model) and `§10.2` (crypto constraints). There is **no enrollment
protocol** — devices are born with their identity at provisioning time.

Everything below is driven by two scripts; their header comments are the
authoritative usage docs:

- `scripts/provision_ca.sh` — mints the CA (once) and all certificate bundles.
- `scripts/provision_device.py` — pushes a device bundle onto a Pico over
  USB-serial (pairs with the `bringup_provision.uf2` image).

`scripts/deploy_home_kit.sh` orchestrates the whole home-kit (broker + mirror
+ device bundles + tablet deploy) on top of these; for a single device the
manual flow below is enough.

## 1. One-time: create the CA and the infrastructure identities

```bash
BROKER_IP=<tablet-LAN-ip> ./scripts/provision_ca.sh
```

First run creates, all ECDSA P-256:

| Output | Purpose |
|---|---|
| `~/rmms-ca/` (mode 700, gitignored) | **CA private key — never leaves this directory.** |
| `out/broker/` (`ca.crt`, `broker.crt`, `broker.key`, `mosquitto.conf`, `acl`) | Mosquitto mTLS listener on the tablet. |
| `out/mirror-<id>/` (PEM) | MagicMirror²'s `MMM-CustomMQTTBridge` subscriber identity (`CN=mirror-<id>`; generated ACL: `topic read rmms/#`, read-only over the whole device tree). |
| `out/operator-<id>/` (PEM) | Operator identity for publishing `info`/`screen` (`CN=operator-<id>`). |
| `out/device-<uuid>/` (DER + PEM) | First device bundle. |

`BROKER_HOST` (default `tablet.local`) and `BROKER_IP` go into the broker
cert's SAN — set `BROKER_IP` to the tablet's real LAN IP or TLS hostname
verification will fail for IP-based connects. Cert lifetimes: CA 10 years
(`CA_DAYS`), leaves 2 years (`LEAF_DAYS`, per `CLAUDE.md §16 Q8`).

## 2. Per device: mint a bundle, add the config files, push it

```bash
# Mint the certs (reuses the existing CA/broker/mirror/operator):
./scripts/provision_ca.sh                       # or DEVICE_UUID=... to re-issue
```

**`provision_ca.sh` produces only the certs + `device.json`.** The three
config files the firmware also needs must be added to `out/device-<uuid>/`
by hand (schemas: `CLAUDE.md §11`; `provision_device.py` silently skips any
that are missing, and a device without them can never connect):

```bash
cd out/device-<uuid>
cat > wifi.json    <<'J'
{"_v":1,"ssid":"<your-ssid>","psk":"<your-psk>","country":"NL"}
J
cat > broker.json  <<'J'
{"_v":1,"host":"tablet.local","ip":"","port":8883}
J
cat > sensors.json <<'J'
{"_v":1,"radar":"bha2","env":"aht21","light":"bh1750"}
J
```

`sensors.json` selects the populated hardware variant
(`radar: "bha2"|"hmmd"`, `env: "bmp280"|"aht21"`, `light: "bh1750"|"gl5516"`).
(`scripts/deploy_home_kit.sh` automates broker.json + a default sensors.json
for new device dirs, but wifi.json is always yours to write — credentials
never live in the repo.)

```bash
# Flash the provisioning firmware, then push the bundle over USB-serial:
cp build/test/bringup/bringup_provision.uf2 /run/media/$USER/RP2350/
./scripts/provision_device.py /dev/ttyACM0 out/device-<uuid>
```

`provision_device.py` writes each file to the canonical littlefs paths
(`/certs/ca.der`, `/certs/dev.crt`, `/certs/dev.key`, `/cfg/*.json` — see
`CLAUDE.md §11`), verifies device-reported SHA-256 per file, runs a final
integrity check, and reboots. After that, flash the real firmware
(`sensor_module.uf2` or `bringup_sensors.uf2`) — reflashing firmware never
touches `/certs` or `/cfg`.

## 3. Verifying the chain end-to-end

- `scripts/test_broker_acl.sh <broker-ip>` proves the Mosquitto ACL: device
  writes only its own subtree, mirror reads everything, operator writes only
  `info`/`screen`, no-cert connections refused (7/7 assertions — bring-up
  step 12, `CLAUDE.md §16 Q9`).
- `docs/report/mqtt_mtls_proof/` holds captured evidence of the mTLS
  handshake, no-cert refusal, and publish/PUBACK against the live tablet
  broker (PEM bodies redacted per `§19`).

## 4. CA custody (§16 Q7 — read before the handover ends)

The security of the entire fleet is the security of `~/rmms-ca/` on the
provisioning workstation. Rules, from `CLAUDE.md §10.2`:

- The CA key never touches a deployed device, the repo, or a shared drive.
- Whoever inherits the project must either **(a) receive `~/rmms-ca/` via an
  encrypted, offline transfer** (encrypted USB stick, then delete the source
  copy), or **(b) regenerate the CA** — which means re-provisioning *every*
  identity: broker, mirror, operator, and each device (re-flash
  `bringup_provision.uf2` + re-push bundles). Option (b) is a clean start and
  takes under an hour for a handful of devices; prefer it if there is any
  doubt about the old key's exposure.
- If a single **device** cert is compromised: re-issue with
  `DEVICE_UUID=<uuid> ./scripts/provision_ca.sh` and re-provision that device.
  (No revocation infrastructure exists in v1 — the broker trusts the CA, so a
  stolen device key is only fully neutralized by rotating the CA. Known v1
  limitation, `CLAUDE.md §10.4`.)
- Compromise of the provisioning workstation = compromise of the fleet.
  Rotate the CA (option b).

## 5. What is deliberately absent

No bootstrap credentials, no shared enrollment secret, no QR codes, no UDP
broker discovery, no on-device key generation, no OTA cert rotation. These
were all failure modes of the previous group's design (`docs/technical-audit.md`
§D) and are excluded by `CLAUDE.md §9.4`/`§10`. Re-introducing any of them
requires an ADR that defeats the audit's findings.
