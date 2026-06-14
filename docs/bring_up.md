# Bring-Up Procedure

Step-by-step hardware bring-up checklist. **Do not skip steps.** Each step
assumes the previous one works. This mirrors the bring-up ladder in root
`CLAUDE.md §15`; that section is authoritative.

> **v1 scope reminders (root `CLAUDE.md`):** Wi-Fi is the **sole transport**
> ([ADR-0002](adr/0002-wifi-sole-transport.md)) — there is no USB-CDC data link,
> no USB-MQTT bridge, and no transport-selector FSM. The wire and on-device
> storage format is **JSON** (`§9.2`/§11) — config/state files are `*.json`. The
> radar is the **MR60BHA2** (advanced module) or the **24 GHz HMMD** module
> (generic module, [ADR-0007](adr/0007-hmmd-radar-second-driver.md)), selected by
> `/cfg/sensors.json` `"radar"`; the advanced module is what this guide demos.
> There is **no IR camera**. Developer logs go to a single **USB-serial dev console**
> (`pico_stdio_usb`, `§12`), which is *not* a tablet data link.

---

## Prerequisites

- Raspberry Pi Pico 2 WH flashed via BOOTSEL or SWD.
- `PICO_SDK_PATH` set to a v2.x checkout of `raspberrypi/pico-sdk`.
- `arm-none-eabi-gcc` ≥ 13.2, CMake ≥ 3.22 on PATH.
- A second Pico flashed as `debugprobe` (or a Picoprobe) on SWD for step 1+.
- Host PC: Linux or WSL2. Native Windows toolchain not recommended.

### Build setup (run once)

```bash
git submodule update --init --recursive
mkdir build && cd build
cmake -DPICO_BOARD=pico2_w -DCMAKE_BUILD_TYPE=Debug ..
cmake --build . -j$(nproc)
```

Release builds: `-DCMAKE_BUILD_TYPE=RelWithDebInfo -DNDEBUG=1`. `scripts/build.sh`
wraps dependency resolution and produces `sensor_module.uf2` (real firmware) plus
the bring-up images under `test/bringup/`.

---

## Step 1 — Blink (bare metal, no FreeRTOS)

**Goal:** verify toolchain, BOOTSEL flash, and SWD connectivity.
(`test/bringup/blink.c`.)

Flash via BOOTSEL:

```bash
# Hold BOOTSEL while plugging in USB, then:
cp build/sensor_module.uf2 /run/media/$USER/RP2350/
```

Flash via SWD (picoprobe attached):

```bash
openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg \
        -c "adapter speed 5000" \
        -c "program build/sensor_module.elf verify reset exit"
```

**Pass:** onboard LED blinks at 1 Hz.

---

## Step 2 — FreeRTOS hello (one task)

**Goal:** verify SMP boot, scheduler starts, no startup hang.
(`test/bringup/freertos_hello.c`.) One task blinks at 1 Hz via `vTaskDelay`.

**Pass:** LED blinks; no hard fault; scheduler runs.

---

## Step 3 — Dev-console logs

**Goal:** `LOG_I` output visible over the standalone USB-serial dev console
(`pico_stdio_usb`, root `CLAUDE.md §12`). (`test/bringup/usb_console.c`.)

```bash
minicom -D /dev/ttyACM0 -b 115200    # check `dmesg | tail` for the actual node
```

Add `LOG_I("hello tick %lu", (unsigned long)tick)` to the task loop.

**Pass:** log lines appear at 1 Hz. (This is a debug console, not a data link —
there is no CDC0/CDC1 split in v1.)

---

## Step 4 — littlefs mount

**Goal:** verify flash mount and KV read/write.
(`test/bringup/storage_counter.c`.)

```c
err_t err = storage_mount();
assert(err == ERR_OK);

uint32_t count = 0; size_t len = 0;
storage_read("/state/boot_count.json", &count, sizeof(count), &len);
count++;
storage_write_atomic("/state/boot_count.json", &count, sizeof(count));
LOG_I("Boot count: %lu", (unsigned long)count);
```

**Pass:** boot count increments each power cycle; no FS corruption over 10 cycles.

---

## Step 5 — Environment (AHT21 or BME280)

**Goal:** `env_task` drives the `env_driver_t` v-table and publishes JSON samples
to a local debug sink (not yet MQTT). Selected via `/cfg/sensors.json` `"env"`
(`"aht21"` | `"bme280"`). (`test/bringup/bme280_only.c`.)

- Verify I²C0 wiring (GP8 SDA / GP9 SCL) and pull-ups.
- Log each sample; validate the `snprintf` encoder, **including the
  `"pres_hpa": null` path when the AHT21 is selected** (it has no pressure
  sensor).
- Expected: temp ~20–30 °C, humidity ~20–80 %, pressure ~950–1050 hPa (BME280).

**Pass:** 1 Hz samples with plausible values; `q=0`; no `ERR_IO`.

### Step 5b — ENS160 (air quality)

**Goal:** `air_task` publishes JSON `air` samples; writes the env driver's last
temp/hum into the ENS160 TEMP_IN/RH_IN compensation registers each cycle.
(`test/bringup/ens160_only.c`.) Confirm the ~5–10 min warm-up: during it the
driver reports `q=2` and AQI sits low until the gas heaters stabilise.

### Step 5c — BH1750 (light)

**Goal:** `light_task` publishes `{"lux":…}` at ~0.2 Hz (continuous H-res,
~120 ms/measurement; init does power-on + initial ~180 ms wait). I²C0 addr `0x23`.

---

## Step 6 — OLED

**Goal:** SH1106 shows page 1 (status) and page 2 (last env sample); the button
cycles pages. (`test/bringup/oled_only.c`.)

- I²C0 address `0x3C` (SA0 low) / `0x3D`. Confirm the controller is SH1106 (root
  `CLAUDE.md §16 Q1`).
- One user button on GP16 (active-low, internal pull-up); `ui_input_init()`.

**Pass:** pages cycle on press; env values update at 1 Hz; no I²C errors.

---

## Step 7 — mmWave radar (MR60BHA2)

**Goal:** Seeed binary protocol parsed; `presence`, `breath_bpm`, `heart_bpm`,
`distance_mm` decode into `RadarSample`. (`test/bringup/radar_only.c`.)

- Verify UART1 wiring (MCU TX GP5 → radar RX; MCU RX GP4 ← radar TX), 115200 baud,
  radar on its own 5 V rail.
- `/cfg/sensors.json` `"radar": "bha2"`.
- Wave/sit in front of the radar to trigger presence; log each `RadarSample`.

**Pass:**
- `presence=true` when a person is nearby.
- `breath_bpm` / `heart_bpm` plausible with a person present.
- `q=0` on good samples; `q=2` on ghost readings (presence without vitals).
- Framing confirmed on the bench (root `CLAUDE.md §3.2`): SOF-`0x01` 8-byte
  header + `~XOR` checksums — **not** Andar `0x53 0x59` framing.

The plausibility/median/breath-hold post-processing on top of the driver is
[ADR-0005](adr/0005-mcu-side-radar-filtering.md) / [ADR-0006](adr/0006-phase-based-breath-hold-detection.md).

### Step 7b — HMMD radar variant + selection (generic module, ADR-0007)

**Goal:** the same firmware drives the 24 GHz HMMD module with no rebuild —
just a config flag. Only relevant on a board populated with the HMMD part.

- Set `/cfg/sensors.json` `"radar": "hmmd"`; power-cycle.
- On boot the log shows `Radar driver selected: 24 GHz HMMD` and
  `init OK … (HMMD protocol)` — confirms `radar_select_from_config()` picked the
  right driver with no code change (same `RadarSample` → same `rmms/<uuid>/radar`).
- Wave/sit in front of the radar; `presence=true`; `breath_bpm` plausible.

**Pass:**
- Correct driver name in the startup `LOG_I`; `"bha2"` vs `"hmmd"` swaps drivers
  with no rebuild.
- `resp_motion` is `null` on the wire (HMMD has no breath-phase stream — graceful
  degradation, ADR-0007); `heart_bpm` may be `null` if this module variant does
  not report it.
- **TODO(spec):** the HMMD `(CTRL, CMD)` report codes in `radar_hmmd.c` are
  firmware-revision-dependent — confirm them against the live module's datasheet
  /UART dump and strip the marker (same step the MR60BHA2 went through for §3.2).
  The frame envelope (`0x53 0x59 … 0x54 0x43`, sum checksum) is already
  host-tested (`test/host/test_radar_hmmd.c`).

---

## Step 8 — Light sensor (GL5516 LDR variant)

**Goal:** ADC sampling + power-law calibration for the *generic*-module variant
(`/cfg/sensors.json` `"light": "gl5516"`).

- GP26 / ADC0 wired to the divider midpoint: `3V3 ─ LDR(GL5516) ─ node ─ 1 kΩ ─ GND`.
- Cover/uncover the LDR; verify lux changes in the `light` sample.
- Same `{"lux":…}` payload as the BH1750 path (ADR-0001).

**Pass:** ~0.2 Hz samples with plausible lux.

---

## Step 9 — CYW43 + lwIP

**Goal:** Wi-Fi DHCP, ping, and mDNS resolve of `tablet.local`.
(`test/bringup/wifi_connect.c`.)

- Provision `/cfg/wifi.json` (SSID, PSK, country) and `/cfg/broker.json`
  (`{"host":"tablet.local","ip":"","port":8883}`).
- `cyw43_arch_init_with_country(CYW43_COUNTRY_NETHERLANDS)`.
- `dns_gethostbyname("tablet.local")` routes to a multicast query
  (`LWIP_DNS_SUPPORT_MDNS_QUERIES`); the tablet's responder
  (`scripts/tablet_mdns_responder.py`) answers (root `CLAUDE.md §8.2`).

```bash
ping <pico-ip>            # from the host, confirm the Pico is on the LAN
```

**Pass:** DHCP assigned; ping succeeds; `tablet.local` resolves. (Campus eduroam
blocks device↔tablet P2P/multicast — use a phone hotspot, keep the tablet awake.)

---

## Step 10 — mbedTLS over TCP (Wi-Fi)

**Goal:** first TLS handshake to Mosquitto via Wi-Fi. This establishes the single
mbedTLS config (static cert chain, restricted cipher suites — root
`CLAUDE.md §10.2`) the rest of the stack reuses. (`test/bringup/mqtt_connect.c`,
`lwip` harness.)

- Uses lwIP's `altcp_tls` (mbedTLS); there is no custom `stream_t`/BIO shim.
- Cert chain from littlefs `/certs/{dev.crt,dev.key,ca.der}` (DER).

**Pass:**
- TLS handshake succeeds over Wi-Fi within ~1 s (root `CLAUDE.md §14.2`).
- Device cert accepted by the broker CA; broker cert validated by the device.

---

## Step 11 — Wi-Fi transport (`transport_task`)

**Goal:** `transport_mqtt` brings up Wi-Fi → `altcp_tls` → lwIP MQTT, gets
CONNACK, publishes, and reconnects with backoff (1 s → 32 s) on link loss. There
is **no transport FSM** in v1 (ADR-0002). (`test/bringup/sensors_publish.c`.)

- Publishes `rmms/<uuid>/status = "online"` (retained, QoS 1) on CONNACK; LWT
  `"offline"`.
- Every sample is spooled (ADR-0003) and drained QoS-1 retry-until-PUBACK.

```bash
# From the host, subscribe with the mirror (or a device) cert bundle:
mosquitto_sub -h tablet.local -p 8883 \
    --cafile ~/rmms-ca/ca.crt --cert out/mirror-*/cert.pem --key out/mirror-*/key.pem \
    -t 'rmms/#' -v
```

**Pass:** CONNACK observed; `status=online` and sensor samples arrive; a Wi-Fi
drop recovers within the backoff window (fresh TLS handshake).

---

## Step 12 — End-to-end identity check

**Goal:** boot against the real tablet Mosquitto with the production ACL pattern.
(`scripts/test_broker_acl.sh <broker-ip>` automates the assertions.)

Broker (root `CLAUDE.md §9.4`/§16 Q9): `require_certificate true`,
`use_identity_as_username true`, ACL:

```
pattern write rmms/%u/#
pattern read  rmms/%u/cmd
```

Verify:
1. Client cert accepted; `client_id` == cert CN.
2. Device can publish `rmms/<uuid>/#` → succeeds.
3. Device cannot publish `rmms/<other-uuid>/#` → ACL-denied.
4. Device receives a `cmd` sent to `rmms/<uuid>/cmd`.

**Pass:** all four hold with **no per-device broker config**. (No handshake
protocol to test — just TLS + MQTT CONNECT + a publish + a subscribe.)

---

## Step 13 — 48-hour soak test

**Goal:** stability before any tag is cut.

```bash
mosquitto_sub -h <broker-ip> -p 8883 \
    --cafile ~/rmms-ca/ca.crt --cert out/mirror-*/cert.pem --key out/mirror-*/key.pem \
    -t 'rmms/#' | ts '[%Y-%m-%d %H:%M:%S]' | tee soak.log
```

During the soak:
- Log task stack high-water marks hourly; any growth is a leak until proven
  otherwise.
- Verify sample rates: env/air 1 Hz, radar at native rate, light ~0.2 Hz.
- Verify steady-state publish round-trip < 250 ms on Wi-Fi.

**Pass:** no unplanned disconnects in 48 h; no high-water-mark growth; queues +
spool drain without overflow (dropped-sample counter stable); no `ERR_IO`.

---

## Performance targets (root `CLAUDE.md §14.2`)

| Metric | Target (Wi-Fi) |
|---|---|
| First-connect handshake (TLS + MQTT CONNECT/CONNACK) | < 1 s |
| Steady-state sense-to-broker latency | < 1 s |
| MQTT publish round-trip (post-handshake) | < 250 ms |
| Sustained throughput | ≥ 700 B/s |

The TLS handshake cost is one-time per connection, not per message. If
steady-state latency is much worse than the previous group's plaintext-MQTT
baseline, something is wrong — it is not TLS. (USB-CDC targets from earlier
drafts are gone with the transport — ADR-0002.)
