# Handover — project state, verification status, and known gotchas

Written at project end (July 2026) for the supervisor and the next group.
This is the honest ledger of what is *proven on hardware*, what is *built but
not hardware-verified*, and the operational traps that cost this group bench
time. Architecture questions belong in `CLAUDE.md`; this file is about state.

## What is proven on real hardware

- **End-to-end secure pipeline** — Pico 2 W → Wi-Fi → mTLS (ECDSA P-256) →
  Mosquitto on the tablet → MagicMirror² tiles, with live radar vitals
  (heart/breath), presence/distance, AHT21 env, ENS160 air quality, BH1750
  lux. Demonstrated repeatedly with the `bringup_sensors` /
  `bringup_sensors_slow` images (the demo firmware).
- **MR60BHA2 radar driver** — Seeed SOF-`0x01` framing, checksums, vitals
  decode, plus the MCU-side plausibility filtering (ADR-0005,
  `docs/radar_filtering_explained.md`). Heart-rate offset validated against a
  pulse oximeter.
- **SH1122 OLED** — driver + 7-page render path, used in every demo.
- **Broker ACL** — `scripts/test_broker_acl.sh`: 7/7 assertions against the
  live tablet broker (`CLAUDE.md §16 Q9`); evidence: `docs/report/evidence_mqtt_mtls.md` + the captured logs in `docs/report/mqtt_mtls_proof/`.
- **Tablet self-heal** — reboot-proof stack via Termux:Boot →
  `scripts/tablet_boot.sh` (broker, sshd, MagicMirror, wake app), IP-independent
  thanks to mDNS (`tablet.local`).
- **Presence → screen coupling** — radar presence wakes/locks the tablet
  screen (wake app + `scripts/tablet_presence_screen.py`, ADR-0004).
- **SBC aggregator** — full pipeline against throwaway SQLite + HAPI FHIR in
  Docker per `sbc/RUNBOOK.md`; 68 tests green (28 pure + 16 unit + 24
  integration). Not soak-tested on the real ROCKPro64 for extended periods.

## Built and unit-tested, but NOT hardware-verified end-to-end

- **The production firmware image** (`sensor_module.uf2`: `app_main` +
  `transport_task` + NV flash spool + watchdog). It builds, host tests pass
  (56), and it shares its entire driver/transport code with the proven
  bring-up images — but the demos ran `bringup_sensors`, and the production
  image has not had its own long soak (spool drain across outages, watchdog
  behaviour, 48 h run per `CLAUDE.md §14.3`). **This is the first thing the
  next group should verify.**
- **Waveshare HMMD radar driver** (`radar_hmmd.c`, ADR-0007) — protocol
  implemented from the previous group's working reference + host tests; not
  exercised against a live HMMD module by this group.
- **DFRobot C1001 radar** — pins wired in `board_pico2wh.h`, driver *not
  written* (was in development; the v-table seam is the extension point).

## Known hardware gotchas (each cost real bench time)

| Symptom | Cause / fix |
|---|---|
| All I²C sensors dead on the PCB (fine on a bare Pico) | PCB I²C bus lacks pull-ups — fit **4.7 kΩ from GP8/GP9 to 3V3**. |
| ENS160 reads all zeros forever | Driver used to re-kick OPMODE after 2 STATAS=0 reads, restarting a genuinely-warming chip in a loop. Fixed (debounce 15); if you see it again, use `bringup_ens160_diag`. ENS160 also legitimately needs ~5–10 min warm-up (`q=2` meanwhile). |
| `breath_bpm` stuck at 0 while heart rate works, checksum errors every ~5–10 s | Marginal radar UART/GND wiring — reseat TX/RX/GND/5V. |
| Breath rate won't drop below ~high-teens | Subject too far: chest-motion SNR needs **≤ ~0.7 m**. Under **0.35 m** the distance gate blanks all vitals — there is a floor *and* a ceiling. |
| `presence:true, distance:null, q:2` with nobody there; screen never sleeps | A wall ~2.5 m in the radar cone is a ghost target. Clear the field, or gate on `distance_mm != null` (the presence bridge currently gates on raw presence — see `HealthMonitorWakeTest/README.md`). |
| Device joins Wi-Fi only after ~5 retries (`rc=-8`) | Some routers do this transiently — the firmware retries 15–20×; not a bug. On an iPhone hotspot, `rc=-2` stuck at "joining" = hotspot is gating new joins: open the hotspot UI, enable *Maximize Compatibility*. |
| Device ↔ tablet can't talk on campus | eduroam blocks client-to-client traffic. **Use a phone hotspot for demos.** |
| Broker/sshd die mid-demo, hotspot IP drifts | Android Doze when the tablet sleeps. Keep the tablet awake (the wake app / presence coupling handles the screen; the tablet must stay powered and un-dozed). |
| Mirror font/layout reverts after a tablet re-setup | `config/custom.css` + Tiresias font are gitignored by upstream MagicMirror; `scripts/deploy_home_kit.sh` now pushes them on every deploy. |

## Environment sensor: it is a BMP280

The env footprint part long documented as "BME280" is a **BMP280** (chip ID
`0x58`): temperature + pressure, **no humidity**. The firmware, config value
(`"bmp280"`, legacy `"bme280"` accepted), wire schema (`hum_pct: null`) and
SBC decoder all reflect this now. The AHT21 (temp + humidity, no pressure) is
the other populate option; the demo board runs an AHT21 (its provisioned
`/cfg/sensors.json` says `"env":"aht21"` — confirm via the `bringup_provision`
LIST command; note a naive re-provision would write the `"bmp280"` default).

## Credentials and CA custody — action required at handover

- The **project CA private key** lives only in `~/rmms-ca/` on the departing
  student's provisioning workstation. It is not in this repo, by design.
  Transfer or regenerate per **`docs/provisioning.md §4`** — until then the
  next group cannot issue any certificates.
- Deployed artifacts (broker certs on the tablet, device bundles in `out/`)
  are LAN-specific and gitignored; regenerate per `docs/provisioning.md`.
- Leaf certs expire **2 years** after issue (`LEAF_DAYS=730`); the CA after
  10 years. Devices provisioned mid-2026 need re-provisioning mid-2028.

## Open items inherited by the next group

1. Hardware-verify + 48 h soak the production `sensor_module` image (above).
2. Fit the PCB I²C pull-ups properly (board rev or rework).
3. Clinical sign-off on the mirror severity thresholds — the ranges in
   `MMM-SensorUI.js` carry no citations yet (`CLAUDE.md §9.5` requires them
   before any deployment beyond the project review).
4. C1001 radar driver behind the `radar_driver_t` v-table (pins already wired).
5. SBC: real-hospital OAuth (SMART backend services JWT path) — dev path uses
   HAPI without auth; and the placeholder codes flagged `confirmed=False` in
   `sbc/src/rmms_aggregator/fhir/codes.py` need clinical LOINC/SNOMED
   assignment.
6. CLAUDE.md `§16` Q7/Q8 are operationally answered in `docs/provisioning.md`;
   fold them into §16 as resolved once the CA transfer actually happens.
