# Remote Health Monitoring System for the Elderly

TU Delft BSc Applied Project (BAP). A three-tier system that passively monitors
an elderly person at home — mmWave radar vitals (heart/breath rate, presence),
environment, air quality, light — and delivers the data to a hospital-side
FHIR R4 endpoint, with a smart-mirror UI in the home:

```
┌──────────────────┐   MQTT/mTLS   ┌──────────────────────┐   MQTT/mTLS   ┌──────────────┐   FHIR R4   ┌──────────┐
│  Sensor module   │ ────────────► │  Android tablet      │ ◄──────────── │  SBC         │ ──────────► │ Hospital │
│  Pico 2 W (C11)  │               │  Mosquitto broker    │               │  aggregator  │             │ endpoint │
│  radar/env/air/  │               │  + MagicMirror² UI   │               │  (Python,    │             │ (HAPI in │
│  light + OLED    │               │  behind the mirror   │               │  sbc/)       │             │  dev)    │
└──────────────────┘               └──────────────────────┘               └──────────────┘             └──────────┘
```

Every byte between tiers is mTLS-encrypted (ECDSA P-256, project CA, static
factory-provisioned certs). The firmware publishes raw JSON samples on
`rmms/<uuid>/...`; the mirror and the SBC are independent subscribers of the
same topics.

## Read this first (in order)

1. **[`CLAUDE.md`](CLAUDE.md)** — the firmware + system architecture contract.
   Single source of truth; everything else defers to it.
2. **[`docs/CLAUDE_tablet.md`](docs/CLAUDE_tablet.md)** — the tablet tier
   (broker, MagicMirror², mDNS, presence→screen coupling, autostart).
3. **[`sbc/CLAUDE.md`](sbc/CLAUDE.md)** — the SBC aggregator / FHIR tier.
4. **[`docs/handover.md`](docs/handover.md)** — what is hardware-proven vs
   built-only, known hardware/network gotchas, and CA/cert custody.

## Repository map

| Path | What it is |
|---|---|
| `main/`, `components/` | **Pico 2 W firmware** (C11; pico-sdk + FreeRTOS + lwIP/mbedTLS + littlefs). One component per concern — layout in `CLAUDE.md §6`. |
| `test/host/` | Native unit tests (CMocka) for driver logic — `bash test/host/run.sh`. |
| `test/bringup/` | Standalone bring-up/diagnostic firmware images (blink → sensors → Wi-Fi → mTLS → full bench). `bringup_sensors` is the demo image. |
| `docs/` | System docs: bring-up procedure, MQTT topic contract, pin map, ADRs, provisioning, handover, technical audit of the previous group's code. |
| `scripts/` | Operational scripts: build, CA + device provisioning, tablet deploy/boot, demo startup, broker ACL test. Index: `scripts/README.md`. |
| `sbc/` | **SBC aggregator** — standalone Python project (own README, RUNBOOK, CLAUDE.md, tests). Never compiled into the firmware. |
| `MagicMirror/` | Vendored upstream MagicMirror² tree (see its README). Project work lives only in `modules/MMM-SensorUI/`, `modules/MMM-CustomMQTTBridge/`, `css/`, `config/` — the live `config.js` holds LAN specifics, is gitignored, and lives on the tablet; `config/config.custom.sample.js` is a UI-only starter. |
| `HealthMonitorWakeTest/` | **Android wake app** (Android Studio project): radar presence ↔ tablet screen on/off via the localhost MQTT listener (ADR-0004). See its README. |
| `apk/` | Prebuilt APK of the wake app. |
| `fully/` | Exported settings for the optional Fully Kiosk Browser (mirror kiosk mode). |
| `third_party/` | Git submodules: FreeRTOS-Kernel, littlefs. **pico-sdk is not vendored** — see the firmware quickstart. |
| `documents_and_diagrams/` | Thesis report, defense presentation, system diagrams. |
| `papers/` | Reference papers + the complete reference list. |
| `lasercutting_files/` | Enclosure/acrylic lasercutting files (DXF + Fusion 360). |
| `PCB/` | This group's KiCad PCB design files + bench measurements (buck converter scope shots). |
| `Readout Code HMMD-MR60BHA2 and BMP/` | Standalone sensor-readout experiments (radar filtering tests, BMP readout) used during bring-up — not part of the production firmware. |

The **previous BAP group's material** (`bestanden_vorige_BAP/`) was removed
from the tree during handover cleanup — it is reference-only and available in
git history (any pre-July-2026 commit). Their registration protocol was
audited (`docs/technical-audit.md`) and deliberately replaced
(`CLAUDE.md §9.4`); their committed private keys are burned — never reuse
anything cryptographic from that tree.

## Quickstart per tier

### Firmware (Pico 2 W)

```bash
git clone --recurse-submodules <this-repo>            # brings FreeRTOS-Kernel + littlefs
git clone https://github.com/raspberrypi/pico-sdk.git ~/pico/pico-sdk
(cd ~/pico/pico-sdk && git submodule update --init)
export PICO_SDK_PATH=~/pico/pico-sdk

mkdir build && cd build
cmake -DPICO_BOARD=pico2_w -DCMAKE_BUILD_TYPE=Debug ..
cmake --build . -j$(nproc)
# → main/sensor_module.uf2 (production) + test/bringup/*.uf2 (bring-up images)
```

Flash: hold BOOTSEL while plugging in, copy the `.uf2` to the mounted drive
(details + SWD alternative: `CLAUDE.md §5`). Bring-up order with expected
results per step: `docs/bring_up.md`. Host tests: `bash test/host/run.sh`.

### Certificates + device provisioning

Identity is factory-provisioned: certs are generated **off-device** by the
project CA and written to the Pico's littlefs; there is no enrollment protocol
(`CLAUDE.md §9.4`). The full procedure — CA creation, broker/mirror/operator/
device bundles, `bringup_provision.uf2` + `scripts/provision_device.py` — is in
**[`docs/provisioning.md`](docs/provisioning.md)**. The CA private key is *not*
in this repo; custody is covered in `docs/handover.md`.

### Tablet (broker + mirror)

`scripts/deploy_home_kit.sh` provisions/deploys the whole tablet kit over ssh
(Termux); `scripts/tablet_boot.sh` + Termux:Boot make the tablet self-healing
across reboots. Tier spec: `docs/CLAUDE_tablet.md`.

### SBC aggregator (FHIR)

```bash
cd sbc && python3 -m venv .venv && .venv/bin/pip install -e .[dev]
.venv/bin/python -m pytest tests -q
```

Full install/run/prove-the-FHIR-output walkthrough: `sbc/RUNBOOK.md`.

### End-to-end demo

`scripts/demo_start.sh <tablet-ip>` starts a demo session; a provisioned Pico
running `bringup_sensors.uf2` publishes live sensor data and the mirror renders
it. Networking caveats (use a phone hotspot, not eduroam; keep the tablet
awake) are in `docs/handover.md`.

## Where the contracts live

- MQTT topics + JSON payload schema: `CLAUDE.md §9`, `docs/mqtt_topics.md`.
- Sensor-JSON → FHIR mapping: `CLAUDE.md §9.6` (firmware side) and
  `sbc/docs/fhir-mapping.md` (generated LOINC/UCUM table).
- Mirror UI boundary (raw topics, JS-side thresholds, two-namespace design):
  `CLAUDE.md §9.5`/`§9.5.1` and the module READMEs under `MagicMirror/modules/`.
- Cross-tier loss-tolerance + idempotency: `docs/sbc-failover-and-idempotency.md`.
