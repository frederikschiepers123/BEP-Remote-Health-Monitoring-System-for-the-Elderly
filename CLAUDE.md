# CLAUDE.md — Sensor Module Firmware

> Read this file completely before writing or modifying any code in this repository.
> It is the single source of truth for architecture, conventions, and constraints.
> If something in the code contradicts this file, the file is correct and the code is wrong —
> open an issue, do not "fix" silently.

---

## 1. Project context

This is the firmware for the **Advanced Sensor Module** of a Remote Medical Monitoring
System (RMMS) — a TU Delft BSc Applied Project (BAP). The system passively monitors
elderly users at home using mmWave radar, environmental sensors, and an IR camera, and
delivers vitals to a hospital-side FHIR/HL7 endpoint via a three-tier architecture:

```
Sensor Module ──► Tablet ──────────────► Radxa Dragon Q6A ──► Hospital FHIR/HL7
   (this repo)    (broker + UI client)    (aggregator)         (provider server)
```

The Android tablet serves **two roles simultaneously**:
1. **MQTT broker** — Mosquitto running under Termux, listening on `:8883` for
   mTLS connections from this firmware and from the Radxa.
2. **MagicMirror² client** — the smart-mirror UI behind the two-way acrylic.
   MM² runs a custom module, `MMM-SensorUI`, that **subscribes directly to
   the firmware's raw `rmms/<uuid>/...` topics on the tablet's own broker**
   and renders per-sensor tiles. The threshold logic (mapping numeric values
   to severity colors red/orange/yellow/green) lives in JavaScript inside
   that module — there is no intermediate "tablet UI service" process and
   no `rmms/ui/...` topic namespace (see §9.5). The mirror is a distinct
   MQTT identity with its own cert; it is not the device, the Radxa, or an
   operator. The firmware's MQTT contract knows nothing about the mirror UI;
   raw topics are the contract, and the mirror is one of several
   subscribers.

This repository implements **only** the sensor-module firmware. The tablet
broker configuration, the MagicMirror² installation and `MMM-SensorUI`
module, the USB-MQTT bridge, the Radxa aggregation service, and the FHIR
translator live in separate repositories (the MagicMirror² tree currently
sits under `MagicMirror/` in this repo as a temporary convenience; it
should be factored out before v1 is shipped).

A previous BAP group built a working MicroPython firmware on a Lilygo TQ-T Pro
(ESP32-S3). **That code is reference material only, not a starting point.** Treat
their PDFs (`BAP_report_PCB_.pdf`, `BAP_Protocol_Thesis2.pdf`) as the behavioural
specification. The MCU, language, SDK, RTOS, transport layer, and broker location
have all changed — there is no useful line-by-line port.

A technical audit of the previous codebase (`docs/technical-audit.md`) identified
serious defects in the previous registration protocol and its implementation:

- The sensor module "signature" is a nonce echo (`sig = nonce`); real ECDSA
  signing was commented out while the log line announcing key generation was
  left in place.
- The project CA private key and all device private keys were committed to
  the repository unencrypted.
- The broker-discovery UDP advertisement was unauthenticated — any LAN host
  could redirect every device with a single broadcast packet.
- TLS SNI was hardcoded to the literal string `"server"`.
- All unregistered devices shared one bootstrap credential.

**The previous registration protocol is therefore not adopted, adapted, or
extended.** It is replaced wholesale by the static factory-cert identity model
in §9.4 and §10. The parallel team's QR-code / camera-LED enrollment dance
(`BAP_Protocol_Thesis2.pdf`) is also dropped in its entirety. There is no
enrollment phase in this firmware — devices are born with their identity.

---

## 2. Architecture (authoritative)

### 2.1 Transports

> **v1 scope (current phase):** the **USB-CDC data link to the tablet is
> deferred**. The firmware connects to the tablet broker **over Wi-Fi only** for
> now; the USB-CDC transport, the tablet-side USB-MQTT bridge, and the
> USB↔Wi-Fi failover FSM (§2.2) are a later-phase addition. The `transport_usb`
> component and `tusb_config.h`'s 2-CDC layout remain in the tree but are not
> wired into the active path yet. The end-state design below (USB primary,
> Wi-Fi failover, uniform mTLS on both) is unchanged as the target; only the
> *order of implementation* is. Developer logging in this phase uses a separate
> USB-serial console (§12), which is **not** the tablet data link.

| Path        | Use         | Encryption                                  | Notes                                                       |
| ----------- | ----------- | ------------------------------------------- | ----------------------------------------------------------- |
| **USB-CDC** | Primary *(target; deferred in v1)* | mTLS, TLS 1.2/1.3, ECDSA P-256, static certs | Direct cable, MCU ↔ tablet, identical cert chain to Wi-Fi   |
| **Wi-Fi**   | Failover *(sole transport in v1)* | mTLS, TLS 1.2/1.3, ECDSA P-256, static certs | Same LAN as tablet                                          |

**Encryption is uniform across both transports.** The MQTT client and the TLS
context know nothing about which transport is carrying their bytes. There is
exactly one cert path, one trust model, and one audit story: every byte that
leaves the device is mTLS-encrypted, no exceptions.

The MCU runs an MQTT v3.1.1 client over **whichever stream is active**, with
mbedTLS layered between MQTT and the stream:

```
       ┌──────────────────────┐
       │  MQTT v3.1.1 client  │
       └──────────┬───────────┘
                  │ plaintext MQTT bytes
       ┌──────────▼───────────┐
       │  mbedTLS context     │  ← static device cert, static project CA
       └──────────┬───────────┘
                  │ TLS records
       ┌──────────▼───────────┐
       │  stream_t (abstract) │
       └────┬─────────────┬───┘
            │             │
   ┌────────▼───┐  ┌──────▼─────────────────┐
   │ TinyUSB    │  │ lwIP TCP socket over   │
   │ CDC0       │  │ CYW43 Wi-Fi            │
   └────────────┘  └────────────────────────┘
```

A **USB-MQTT bridge** runs on the tablet as a dumb byte pipe:

```
/dev/ttyACM0  ◄──byte-for-byte──►  localhost:8883 (Mosquitto mTLS listener)
```

The bridge does **not** terminate TLS. It does not parse MQTT. It does not look
at the bytes. It is a transparent stream proxy whose only job is to bridge the
CDC character device into a loopback TCP socket. Mosquitto sees one mTLS
connection from the bridge (with the device's client cert) and treats it
identically to a connection arriving over Wi-Fi. **The firmware never knows
which path it is on; Mosquitto never knows either.**

### 2.2 Transport selection FSM

```
                     ┌────────────────────┐
                     │  BOOT              │
                     │  init peripherals  │
                     └─────────┬──────────┘
                               │
                               ▼
                     ┌────────────────────┐
              ┌──────┤  USB_PROBE         │  Wait ≤ 8 s for:
              │      │  1. CDC enumerated │   1. tud_cdc_connected()
              │      │  2. TLS handshake  │   2. mbedTLS handshake over CDC
              │      │  3. MQTT CONNACK   │   3. MQTT CONNACK from broker
              │      └─────────┬──────────┘
              │ any step fails │ all OK
              ▼                ▼
   ┌────────────────────┐ ┌────────────────────┐
   │  WIFI_ACTIVE       │ │  USB_ACTIVE        │  preferred
   │  TLS over TCP to   │ │  TLS over CDC to   │
   │  tablet:8883       │ │  tablet:8883       │
   │  (via Wi-Fi)       │ │  (via bridge)      │
   └─────────┬──────────┘ └─────────┬──────────┘
             │ link lost            │ USB unplug
             │ + USB returns        │ + 3× missed keepalive
             └──────────► swap ◄────┘
```

USB is **always preferred** when present. The 8-second probe budget covers TLS
handshake (~80 ms typical, but allow for retries) plus MQTT CONNACK round-trip.
On swap, the dormant transport keeps its mbedTLS session context warm for fast
resumption via session tickets (RFC 5077).

### 2.3 Component boundary

The firmware owns:
- Sensor sampling and per-sample QoS metadata.
- MQTT publication of all sensor topics.
- Transport selection and link health.
- Local OLED UI (status pages).
- Button/LED interaction.
- Registration handshake (per parallel-team protocol — see §9.4).

The firmware does **not** own:
- Threshold logic for clinical alerts (lives on Radxa).
- Filtering of mmWave "ghost" readings (lives on Radxa — publish raw + quality field).
- Face recognition (lives on Radxa, gated by radar presence).
- Any FHIR translation (Radxa).
- Persistent vitals storage (Radxa, with delivery-buffer semantics; firmware
  retains only an in-RAM ring buffer for retry over the active transport).

---

## 3. Hardware

### 3.1 MCU
- **Raspberry Pi Pico 2 WH** (RP2350, dual Cortex-M33, 520 KB SRAM, 4 MB QSPI flash,
  CYW43439 Wi-Fi + BT, pre-soldered headers).
- USB-FS device peripheral via TinyUSB.

### 3.2 Peripherals & buses

| Block            | Part                   | Bus  | Notes                                          |
| ---------------- | ---------------------- | ---- | ---------------------------------------------- |
| Environment      | **AHT21** (default) **OR** Bosch BME280 | I²C0 | One I²C footprint, two populate options. AHT21 = temp + humidity (no pressure → `pres_hpa` emitted as `null` per §9.2.3). BME280 = temp + humidity + pressure. Selection via `/cfg/sensors.json` `"env"` field (`"aht21"` \| `"bme280"`); default `"bme280"` for back-compat with pre-AHT21 provisioned devices. AHT21 fixed addr `0x38`; BME280 `0x76` (SDO low) or `0x77`. |
| Air quality      | ScioSense ENS160       | I²C0 | eCO₂ + TVOC + UBA AQI (1–5); default addr `0x53` (no conflict with BME280 `0x76/0x77` or AHT21 `0x38`). Compensation: whichever env driver is active feeds its last temp/hum into the ENS160 TEMP_IN / RH_IN registers every read cycle. |
| mmWave radar (A) | **Seeed MR60BHA2** (60 GHz, heart + breath) | UART | 5 V mains, Seeed binary protocol — `[0x01][SEQ][LEN][TYPE][~XOR hdr cksum]` header, see §3.2 framing note. |
| mmWave radar (B) | **DFRobot C1001** (24 GHz, presence + vitals) | UART | 5 V mains, DFRobot AT-style command set |
| Light            | **Rohm BH1750FVI** (advanced module, on MR60BHA2 breakout, I²C0 `0x23`) **OR** GL5516 LDR (generic module, ADC0/GPIO26 + 1 kΩ divider to GND) | I²C0 / ADC0 | Two product variants share the same PCB. Selection via `/cfg/sensors.json` `"light"` field (`"bh1750"` \| `"gl5516"`); default `"bh1750"`. BH1750 reports calibrated lux directly (16-bit raw / 1.2). GL5516 needs a per-board power-law calibration (`lux = (A / R_LDR)^(1/B)`, defaults `A=50000, B=0.7`). Both publish the same §9.2.2 payload `{"lux": ...}` on `rmms/<uuid>/light`. See ADR-0001 for the two-variant rationale. |
| OLED             | 1.3″ 128×64 (**likely SH1106**) | I²C0 | Confirm controller before driver work |
| Buttons          | 2× momentary           | GPIO | Internal pull-up, hardware debounce optional   |
| LEDs             | Status indicators      | GPIO | Plus CYW43 onboard LED for net-state           |

**Radar driver architecture:** Both radars are supported by separate drivers
behind a common `radar_driver_t` v-table. Selection is by config flag at
`/cfg/sensors.json` set during provisioning — *not* by runtime UART probing
(probing is fragile on noisy UART lines and the two protocols share no magic
bytes). See §7.4.

**Note on framing (bench-resolved):** The DFRobot C1001 uses Andar/AI-Thinker
framing — `[0x53][0x59][con][cmd][len_H][len_L][payload][cksum][0x54][0x43]`
at 115200 baud. **The Seeed MR60BHA2 does NOT share that framing.** Bring-up
of the live module showed a different layout:

  `[0x01][SEQ_H][SEQ_L][LEN_H][LEN_L][TYPE_H][TYPE_L][HDR_CKSUM]`
  ` ── payload (LEN bytes) ── [DATA_CKSUM]`

with `~XOR` checksums over the 7-byte pre-checksum header and over the
payload respectively. The drivers therefore stay independent (no shared
`radar_andar_frame.c`); each one parses its own protocol. The
`radar_driver_t` v-table (§7.4) is still the right abstraction — it just
sits one layer above the framing, on the parsed `RadarSample`.

### 3.3 Power
- **5 V mains → MCU; MCU 3V3 rail powers BME280, OLED, LDR, buttons, LEDs, IR camera.**
- **mmWave radar runs from its own 5 V mains rail** (high RF draw, isolated).
- Always-on, no battery, no sleep modes in the v1 firmware.

### 3.4 Pin map
Authoritative pin assignments live in `include/board_pico2wh.h`. Do not hardcode
GPIO numbers anywhere else. PCB layout owns this file in spirit — the firmware
reads it.

---

## 4. Toolchain

- **pico-sdk** ≥ 2.1.0 (RP2350 support).
- **arm-none-eabi-gcc** ≥ 13.2 (C11, `-Wall -Wextra -Werror -Wpedantic -Wshadow
  -Wconversion`).
- **CMake** ≥ 3.22.
- **TinyUSB** (bundled with pico-sdk).
- **lwIP** (bundled, used in `NO_SYS=0` mode under FreeRTOS).
- **mbedTLS** (bundled, used by lwIP for TLS 1.2/1.3 with ECDSA P-256).
- **FreeRTOS Kernel** (SMP port via `pico_cyw43_arch_lwip_sys_freertos`).
- **littlefs** for on-flash KV (creds, certs, registration state).
- **picotool** for `.uf2` build verification and metadata inspection.
- **OpenOCD + picoprobe** (or a second Pico flashed as `debugprobe`) for SWD.

Do **not** introduce additional dependencies without justifying them in an ADR
under `docs/adr/`. The pico-sdk + lwIP + FreeRTOS + littlefs stack is the
sanctioned baseline.

### 4.1 Environment
- Development host: Linux or WSL2 on Windows. Avoid native Windows toolchains —
  the pico-sdk works there but is poorly tested by this project.
- `PICO_SDK_PATH` must point at a checked-out `raspberrypi/pico-sdk` (v2.x branch).
- `PICO_BOARD=pico2_w` is mandatory — do not default to `pico2`.

---

## 5. Build, flash, debug

```bash
# First-time setup
git submodule update --init --recursive
mkdir build && cd build
cmake -DPICO_BOARD=pico2_w -DCMAKE_BUILD_TYPE=Debug ..

# Build
cmake --build . -j$(nproc)

# Flash via BOOTSEL (hold BOOTSEL while plugging in, then):
cp sensor_module.uf2 /run/media/$USER/RP2350/

# Flash via SWD (picoprobe attached):
openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg \
        -c "adapter speed 5000" \
        -c "program sensor_module.elf verify reset exit"

# Live logs (stdio over CDC1 — see §12)
minicom -D /dev/ttyACM1 -b 115200
```

Release builds: `-DCMAKE_BUILD_TYPE=RelWithDebInfo` and `-DNDEBUG=1`. Never ship
`Debug` to a deployment.

---

## 6. Repository layout

```
.
├── CLAUDE.md                     # this file
├── CMakeLists.txt                # top level
├── pico_sdk_import.cmake         # standard SDK shim
├── FreeRTOSConfig.h              # FreeRTOS tuning
├── lwipopts.h                    # lwIP tuning
├── mbedtls_config.h              # mbedTLS feature set (ECDSA P-256, TLS 1.2/1.3)
│
├── main/
│   ├── CMakeLists.txt
│   ├── app_main.c                # FreeRTOS task creation, event loop, watchdog
│   └── app_config.h
│
├── components/
│   ├── board/                    # board_pico2wh.h, pin map, hardware init
│   ├── sensor_env/               # env_driver_t vtable + drivers + env_task
│   │   ├── env_driver.h          # vtable: init/read_sample/name/ctx
│   │   ├── env_select.c          # reads /cfg/sensors.json "env", returns driver
│   │   ├── bme280.c              # BME280 (temp+hum+pres)
│   │   └── aht21.c               # AHT21 (temp+hum, pres null)
│   ├── sensor_air/               # ENS160 driver + air_task (consumes env's
│   │   │                         #   temp/hum for TEMP_IN/RH_IN compensation)
│   ├── sensor_radar/             # radar_driver_t interface + radar_task
│   │   ├── radar_bha2.c          # Seeed MR60BHA2 driver (Seeed framing)
│   │   ├── radar_c1001.c         # DFRobot C1001 driver (Andar framing)
│   │   └── radar_select.c        # Reads /cfg/sensors.json, instantiates one
│   ├── sensor_light/             # BH1750 (I²C lux) driver + light_task
│   ├── cfg/                      # /cfg/{wifi,broker,sensors}.json loaders
│   ├── ui_oled/                  # SH1122 driver + 4-page UI state machine
│   ├── ui_input/                 # buttons + LEDs
│   ├── transport_usb/            # TinyUSB CDC, stream wrapper
│   ├── transport_wifi/           # CYW43 init, lwIP, DHCP, mDNS resolver
│   ├── transport_selector/       # FSM from §2.2
│   ├── mqtt_client/              # MQTT v3.1.1 over an abstract stream
│   ├── tls_context/              # mbedTLS ECDSA P-256 setup for Wi-Fi path
│   ├── identity/                 # loads cert+key from littlefs, exposes UUID
│   ├── storage/                  # littlefs mount + KV API
│   ├── json/                     # snprintf encoders + jsmn-style tokenizer
│   └── log/                      # tagged logging over CDC1
│
├── third_party/
│   ├── pico-sdk/                 # submodule
│   ├── FreeRTOS-Kernel/          # submodule
│   └── littlefs/                 # submodule
│
├── test/
│   ├── host/                     # native unit tests (CMocka)
│   └── hil/                      # hardware-in-the-loop scripts
│
└── docs/
    ├── adr/                      # architecture decision records
    ├── mqtt_topics.md            # authoritative topic + payload schema
    ├── pin_map.md                # human-readable mirror of board_pico2wh.h
    └── bring_up.md               # bench bring-up procedure
```

**One driver per sensor, one component per concern, one task per producer.** Do
not create a `utils/` or `helpers/` component. If it belongs nowhere it probably
should not exist.

---

## 7. Concurrency model

FreeRTOS SMP, both M33 cores enabled, tickless idle off (we are always mains-powered).

### 7.1 Tasks

| Task              | Core affinity | Priority | Stack  | Notes                                |
| ----------------- | ------------- | -------- | ------ | ------------------------------------ |
| `app_main`        | Any           | 2        | 2 KB   | Watchdog kick, supervisor            |
| `env_task`        | Any           | 3        | 2 KB   | Polls the env_driver_t (BME280 or AHT21 per `/cfg/sensors.json`) at 1 Hz; pushes its temp/hum into the ENS160 compensation regs each cycle |
| `air_task`        | Any           | 3        | 2 KB   | Polls ENS160 at 1 Hz (warm-up gated) |
| `radar_task`      | Core 1        | 4        | 4 KB   | UART RX, frame parsing               |
| `light_task`      | Any           | 3        | 1 KB   | BH1750 I²C read at 0.2 Hz (continuous H-res; lux barely moves at 1 Hz) |
| `ui_task`         | Core 0        | 2        | 4 KB   | OLED redraw on button or 1 Hz        |
| `transport_task`  | Core 0        | 5        | 6 KB   | Owns the active stream + MQTT client |
| `selector_task`   | Core 0        | 6        | 2 KB   | Transport FSM, highest priority      |

Priorities: higher numeric = higher priority. The transport selector outranks
everything because losing the link is the only failure mode that matters.

### 7.2 IPC
- One **FreeRTOS queue per producer→transport edge**, named `q_env`, `q_air`,
  `q_radar`, `q_light`.
- No global mutable state. Period. If two tasks need to share a value, it goes
  through a queue or a mutex-protected struct.
- Inter-core comms use FreeRTOS primitives, **not** the raw `multicore_fifo`.

### 7.3 Task watchdog
Every task calls `wdt_task_alive(task_id)` once per iteration. `app_main` polls
the table at 1 Hz; if any task misses two consecutive intervals, `app_main`
panics into a known reset path with the offending task ID logged.

### 7.4 Radar driver abstraction

```c
typedef struct {
    err_t (*init)(void *ctx, uart_inst_t *uart);
    err_t (*read_sample)(void *ctx, radar_sample_t *out, uint32_t timeout_ms);
    err_t (*close)(void *ctx);
    const char *name;          // "MR60BHA2" or "C1001"
    void *ctx;
} radar_driver_t;

radar_driver_t *radar_select_from_config(void);   // reads /cfg/sensors.json
```

The `radar_task` calls `read_sample()` in a loop and is **completely unaware**
of which radar is attached. New radars in the future require a new
`radar_*.c` file and a new entry in `radar_select.c`. They do **not** require
changes to the task, to MQTT topics, or to the payload schema.

`radar_sample_t` is the lowest-common-denominator of what both radars produce:
presence (bool), distance (mm, nullable), breath rate (BPM, nullable),
heart rate (BPM, nullable). Quality flag (`q` in §9.2) is set by the driver
when the radar reports an obvious garbage value — including the C1001 "ghost
reading" case the previous group documented.

---

## 8. Transport layer details

### 8.1 USB-CDC (primary)
- TinyUSB configured for **two CDC interfaces**:
  - **CDC0** → mTLS-encrypted MQTT byte stream (binary, no line discipline,
    no echo, no character translation).
  - **CDC1** → stdio for logs (`log` component writes here; line-buffered).
- Mandatory in `tusb_config.h`: `CFG_TUD_CDC = 2`, `CFG_TUD_CDC_RX_BUFSIZE ≥ 512`,
  `CFG_TUD_CDC_TX_BUFSIZE ≥ 1024`.
- The TLS stack (mbedTLS) sits **above** CDC0 via custom send/recv callbacks
  installed with `mbedtls_ssl_set_bio`. These callbacks read/write the
  TinyUSB CDC ring buffers. TLS records may span multiple USB transfers; the
  callbacks handle short reads by returning `MBEDTLS_ERR_SSL_WANT_READ`.
- On USB suspend, MQTT client tears down cleanly (DISCONNECT packet best-effort,
  then `mbedtls_ssl_close_notify`) and the selector swings to Wi-Fi.

### 8.2 Wi-Fi (failover)
- `cyw43_arch_init_with_country(CYW43_COUNTRY_NETHERLANDS)`.
- Credentials in littlefs at `/cfg/wifi.json`. Provisioned at factory or over
  USB-CDC at first boot — no SoftAP, no captive portal.
- Broker address resolution (production target):
  1. `/cfg/broker.json` `ip` field, if present and parseable → use directly.
  2. `/cfg/broker.json` `host` field → DNS lookup via lwIP `dns_gethostbyname`.
  3. If the host is a `*.local` mDNS name → mDNS query (separate lwIP module,
     **future work**). The current bring-up does steps 1 and 2 only.

  The previous group's UDP-broadcast discovery scheme is dead and **must not
  be reintroduced**. For hotspot/mDNS-only LANs where no DNS resolves
  `tablet.local`, `demo_start.sh` writes the literal tablet IP into
  `/cfg/broker.json` each session, so step 1 always succeeds.
- TLS stack: **identical** to the USB-CDC path — same mbedTLS configuration,
  same cert chain, same cipher suites. The only difference is the underlying
  `stream_t` backend (lwIP TCP instead of TinyUSB CDC).
- Reconnect: exponential backoff 1 s → 32 s, then 32 s constant. Do not flood
  the broker on outage.

### 8.3 MQTT client
- Implement against an **abstract `stream_t` interface**, not against TLS or USB
  directly. The MQTT client speaks to mbedTLS, mbedTLS speaks to `stream_t`,
  `stream_t` is backed by either CDC0 or a TCP socket:

  ```c
  typedef struct {
      int  (*read)(void *ctx, uint8_t *buf, size_t len, uint32_t timeout_ms);
      int  (*write)(void *ctx, const uint8_t *buf, size_t len);
      void (*close)(void *ctx);
      void *ctx;
  } stream_t;
  ```

- Two `stream_t` implementations: `stream_cdc.c` (TinyUSB CDC0) and
  `stream_tcp.c` (lwIP TCP socket). Both expose identical semantics.
- One `tls_context.c` wraps mbedTLS with the static cert chain. It accepts a
  `stream_t *` as its byte transport. Adding a future transport means writing
  one new `stream_t`, nothing else.
- QoS 1 for vitals; QoS 0 acceptable for the OLED-debug heartbeat topic.
- Keepalive 30 s on USB, 60 s on Wi-Fi.
- Last-Will-and-Testament on `device/<uuid>/status` with payload `"offline"`,
  retained.

---

## 9. MQTT contract

### 9.1 Topic schema
All topics are rooted at `rmms/<uuid>/...` where `<uuid>` is the device UUID
from registration. **No spaces, no Dutch diacritics, no PII in topics.**

| Topic                              | Direction | Payload         | QoS |
| ---------------------------------- | --------- | --------------- | --- |
| `rmms/<uuid>/env`                  | pub       | JSON env sample (BME280) | 1 |
| `rmms/<uuid>/air`                  | pub       | JSON air-quality sample (ENS160) | 1 |
| `rmms/<uuid>/radar`                | pub       | JSON radar sample (raw + quality flag) | 1 |
| `rmms/<uuid>/light`                | pub       | JSON light sample | 1 |
| `rmms/<uuid>/status`               | pub (retained) | `"online"`/`"offline"` | 1 |
| `rmms/<uuid>/cmd`                  | sub       | JSON command    | 1   |
| `rmms/<uuid>/log`                  | pub       | text log line   | 0   |

**Downlink topics** that live in the per-device tree but are **not** handled
by the firmware (it neither publishes nor subscribes to them — they exist
because the per-device tree is the natural place for per-device data flowing
toward consumers on that device's network, in particular the mirror):

| Topic                | Publisher                                              | Subscriber | Payload |
| -------------------- | ------------------------------------------------------ | ---------- | ------- |
| `rmms/<uuid>/info`   | operator cert (PoC: laptop; production: Radxa relay)    | mirror     | `{"text":"...","wall_ms":...}` |
| `rmms/<uuid>/screen` | operator cert (PoC: laptop; production: Radxa relay)    | mirror     | `{"page":1..4,"wall_ms":...}` |

The firmware **never** publishes to these, **never** subscribes to them, and
**must not** parse them. They are documented here so the topic tree is fully
described in one place.

### 9.2 Payload format

**JSON everywhere** — wire payloads (this section), commands (§9.2.4), time
sync (§9.2.5), and on-device storage (§11). UTF-8, RFC 8259, no extensions.
JSON was rejected in an earlier revision in favour of CBOR; that decision is
reversed. CBOR's ~50% bandwidth advantage is irrelevant at this project's
scale (~1 KB/s aggregate), and the ecosystem costs (FHIR is JSON-native,
MagicMirror² expects JSON, `mosquitto_sub -v` reads JSON directly, storage
files are human-debuggable via `cat`) outweigh the marginal compactness.

One serializer (snprintf templates), one parser (jsmn tokenizer), one
mental model. No CBOR library is linked into this firmware.

The firmware encodes outbound JSON via **`snprintf` into a fixed buffer with
per-sensor templates**. No JSON library, no allocator, no DOM. Templates are
hand-rolled per sensor type:

```c
// env_sample.c — encoder pattern
int env_sample_encode(char *buf, size_t cap, const env_sample_t *s) {
    return snprintf(buf, cap,
        "{\"ts_us\":%llu,\"wall_ms\":%lld,\"seq\":%u,\"q\":%u,"
        "\"v\":{\"temp_c\":%.3f,\"hum_pct\":%.3f,\"pres_hpa\":%.3f}}",
        (unsigned long long)s->ts_us,
        s->wall_ms_valid ? (long long)s->wall_ms : -1,   // -1 sentinel; Radxa interprets
        s->seq, s->quality,
        s->temp_c, s->hum_pct, s->pres_hpa);
}
```

The firmware decodes **inbound JSON only for commands and time-sync** on
`rmms/<uuid>/cmd` and `rmms/<uuid>/time/set`. Both have a closed payload set;
parsing uses a tiny tokenizer (jsmn-style, ~600 lines) committed to
`components/json/` — *not* cJSON, *not* anything with allocation. If a future
inbound topic needs structured parsing beyond what jsmn handles, that's an ADR.

#### 9.2.1 Common envelope

Every sensor sample carries this envelope:

| Field | Type | Notes |
|---|---|---|
| `ts_us` | uint64 (JSON number) | Monotonic microseconds since device boot. Always present, always valid. |
| `wall_ms` | int64 (JSON number) | Wall-clock milliseconds since Unix epoch. `-1` if RTC not synced yet. **Do not omit the field**; use the sentinel. |
| `seq` | uint32 (JSON number) | Per-topic monotonic counter. Persisted across reboots in `/state/last_seq.json`. |
| `q` | uint8 (JSON number) | Quality. `0`=ok, `1`=stale, `2`=degraded, `3`=invalid. |
| `v` | object | Sensor-specific body. See per-sensor schemas below. |

The `q` field is the quality marker. **The radar driver sets `q=2` for any
sample where ghost-detection heuristics fire.** The Radxa filters on `q`; the
firmware never silently drops samples.

#### 9.2.2 Per-sensor `v` bodies

**env** (`rmms/<uuid>/env`):
```json
{"temp_c":21.500,"hum_pct":55.000,"pres_hpa":1013.250}
```
- `pres_hpa` is `null` when the AHT21 env driver is active (it has no
  pressure sensor); the BME280 driver always emits a numeric value. This
  is consistent with §9.2.3's "encode `null` for unavailable values" rule
  — receivers should not assume `pres_hpa` is always numeric.

**air** (`rmms/<uuid>/air`):
```json
{"co2_ppm":600,"tvoc_ppb":300,"aqi":2}
```
- `co2_ppm` — ENS160's equivalent CO₂ estimate, integer ppm.
- `tvoc_ppb` — total volatile organic compound estimate, integer ppb.
- `aqi` — ENS160's UBA-style air quality index, integer 1–5
  (1=excellent, 2=good, 3=moderate, 4=poor, 5=unhealthy).

The ENS160's outputs are coupled (TVOC is the raw measurement; eCO₂ and AQI
are computed by the chip from TVOC + the optional temp/hum compensation
written via I²C). The driver writes BME280's last temp/hum to the ENS160
compensation registers every cycle. The chip needs an undocumented warm-up
period (~5–10 min) after power-up; readings during that window have `q=2`.

**radar** (`rmms/<uuid>/radar`):
```json
{"presence":true,"distance_mm":2400,"breath_bpm":16.5,"heart_bpm":72.0}
```
Any field except `presence` may be `null` if the radar driver did not produce
a reading in this cycle (e.g., heart rate is only sampled every Nth frame).

**light** (`rmms/<uuid>/light`):
```json
{"lux":120.5}
```

#### 9.2.3 JSON discipline

Rules enforced at code review:

- **UTF-8, no BOM, no comments, no trailing commas, no NaN, no Infinity.**
  Vanilla RFC 8259. Reject JSON5, NDJSON, JSONL.
- **Compact** — no pretty-printing, no extra whitespace. `snprintf` templates
  control this.
- **Numeric precision** — floats use `%.3f` (3 decimal places) unless the
  sensor's accuracy demands different. Document per-field precision in the
  encoder header. Integers as-is.
- **Field naming** — `snake_case`. No CamelCase, no kebab-case.
- **Nesting depth ≤ 2** — top-level envelope plus one `v` object. Deeper
  nesting complicates parsing on receivers without buying anything.
- **No optional fields.** If a value is unavailable, encode `null` or the
  documented sentinel (`-1` for `wall_ms`). Receivers should never need to
  handle "field is sometimes there, sometimes not."
- **Payload size budget: ≤ 256 bytes per sample.** Current schemas come in
  well under this. If a future schema exceeds 256 bytes, that's an ADR.

#### 9.2.4 Commands (inbound on `rmms/<uuid>/cmd`)
```json
{"cmd":"activate"}
{"cmd":"deactivate"}
{"cmd":"deregister"}
{"cmd":"shutdown"}
```
Closed set per §9.3.

#### 9.2.5 Time sync (inbound on `rmms/<uuid>/time/set`)
```json
{"epoch_ms":1716210000000}
```
See §16 question 6 for the RTC sync mechanism. If adopted, the tablet
publishes this topic retained per-device when the broker observes a device
connect.

### 9.3 Commands (subscriber side)
The closed set of commands the firmware responds to on `rmms/<uuid>/cmd`:

- `activate` — start publishing sensor data.
- `deactivate` — stop publishing, keep connection.
- `deregister` — clear all littlefs config (not certs), drop network, factory
  reset to provisioning-but-unconfigured state. Certs remain — they are
  identity, not configuration.
- `shutdown` — drop links and idle. Recover on power cycle.

`deregister_sbc` from the previous group's command set is **removed** — the
SBC is no longer the broker, so the concept is meaningless. Do not reintroduce
it under a new name. Adding new commands requires updating the Radxa team's
contract first.

### 9.4 Identity (replaces "registration")

The firmware has **no registration phase** in the protocol sense. The previous
group's enroll/verify dance is dropped. Identity comes from the cert.

**At factory provisioning:**
1. A UUID is assigned to the device (UUIDv4, stored at `/cfg/device.json`).
2. An ECDSA P-256 keypair is generated **off-device** on the provisioning
   workstation.
3. A CSR is generated with `CN=<uuid>` and signed by the project CA.
4. The signed cert (`/certs/dev.crt`), private key (`/certs/dev.key`), CA
   cert (`/certs/ca.der`), and UUID are written to littlefs.
5. The device is now permanently bound to that identity.

**At boot:**
1. Firmware loads cert and key from littlefs.
2. Opens mTLS connection to broker (USB-CDC primary, Wi-Fi failover).
3. Sends MQTT CONNECT with `client_id = <uuid>`.
4. Broker validates the client cert against the project CA, reads CN from the
   cert, enforces `client_id == CN`.
5. Broker's ACL permits this client to publish to `rmms/<uuid>/#` and subscribe
   to `rmms/<uuid>/cmd`. The pattern is configured once on Mosquitto with
   `pattern write rmms/%c/#` and `pattern read rmms/%c/cmd` — no per-device
   broker config required.
6. Firmware publishes `rmms/<uuid>/status = "online"` (retained, QoS 1) and
   begins normal operation.

That is the whole flow. There is no:
- bootstrap port (no `:1884`),
- shared bootstrap credential,
- enroll request/response sequence,
- nonce challenge,
- two-stage reconnection,
- UDP broker advertisement,
- camera-based label assignment,
- QR code anywhere in the system.

If a future requirement re-introduces any of these, write an ADR justifying it
and explaining what threat model it addresses. The audit's findings about the
previous protocol (`docs/technical-audit.md` §D.1–D.2) are the baseline
counter-argument; the new requirement must materially defeat them.

### 9.5 Mirror UI boundary

MagicMirror² runs on the tablet and **subscribes directly to the firmware's
raw `rmms/<uuid>/...` topics**. The threshold logic — mapping numeric values
to severity colors `green` / `yellow` / `orange` / `red` — lives in
JavaScript inside a custom MM² module, `MMM-SensorUI`, that consumes those
topics via an MQTT bridge module (currently `MMM-CustomMQTTBridge`, a
project-internal bridge that subscribes to the broker and forwards messages
to other MM² modules — the choice is mirror-side and may change without
firmware impact). There is no intermediate "tablet UI service" process,
no `rmms/ui/...` topic namespace, and no JSON translation layer between
firmware and mirror.

```
firmware ──JSON──►  rmms/<uuid>/env    ──┐
firmware ──JSON──►  rmms/<uuid>/air    ──┤──►  tablet broker (Mosquitto)  ──┐
firmware ──JSON──►  rmms/<uuid>/radar  ──┤                                  │
firmware ──JSON──►  rmms/<uuid>/light  ──┘                                  │
                                                                            │
                       (the Radxa is also a subscriber here for FHIR — §9.6)│
                                                                            ▼
                                                   MagicMirror² on tablet:
                                                   MQTT bridge → MMM-SensorUI
                                                   • parse raw JSON
                                                   • apply per-tile severity
                                                     thresholds in JS
                                                   • render numeric value
                                                     + colored check sign

operator ──JSON──► rmms/<uuid>/info     ──►  broker  ──► MMM-SensorUI (text tile)
operator ──JSON──► rmms/<uuid>/screen   ──►  broker  ──► MMM-SensorUI (page select)
```

**Per-tile rendering rules:**

| Tile | Source field(s) | Display |
|---|---|---|
| Heart rate | `radar.v.heart_bpm` | numeric BPM + severity check |
| Breath rate | `radar.v.breath_bpm` | numeric RPM + severity check |
| Temperature | `env.v.temp_c` | numeric °C + severity check |
| Humidity | `env.v.hum_pct` | numeric % + severity check |
| Air quality | `air.v.aqi` (1–5) | severity check only (no number) |
| Info | `info.v.text` | text message panel |
| Screen | `screen.v.page` | switches between configured layouts (1–4) |

The four severity strings are literal: `"green"` / `"yellow"` / `"orange"`
/ `"red"`. MMM-SensorUI maps them to CSS classes directly; there is no
abstraction layer. Threshold ranges live in `MMM-SensorUI.js` with comments
citing the source (WHO, AHA, ASHRAE) per range. Clinical advisor sign-off
is required before any deployment beyond the project review.

**Mirror identity:** the mirror is a distinct MQTT client with its own
cert (CN format `mirror-<short-id>`), issued by the project CA by
`scripts/provision_ca.sh` alongside the device cert. The Mosquitto ACL
grants the mirror role `topic read rmms/+/+` (read everything in the
device-rooted tree). The mirror does not publish sensor topics.

**Operator identity:** the PoC laptop (and, eventually, the Radxa relay
for hospital-sourced messages) uses an `operator-<short-id>` cert with
ACL `topic write rmms/+/info` and `topic write rmms/+/screen`. Nothing
else.

**Rules:**
- The firmware publishes **only** the raw topics listed in §9.1
  (`env`, `air`, `radar`, `light`, `status`, `log`). It does not publish
  `info`, `screen`, or anything in a `ui` namespace (none exists).
- The firmware subscribes **only** to `rmms/<uuid>/cmd` (plus
  `rmms/<uuid>/time/set` when §16 Q6 is resolved).
- The Radxa never touches the mirror UI. Its v1 jobs are raw-vitals
  aggregation, store-and-forward buffering, and FHIR translation
  (§9.6); in deployment it will additionally relay hospital-sourced
  messages onto the `info`/`screen` topics, but that path is not
  exercised in this project's PoC (the laptop stands in).
- If MM² needs a derived UI value that isn't a function of the raw
  topics, the fix lives in `MMM-SensorUI.js` (compute it on the
  mirror); it does **not** go in the firmware. New raw topics require
  a sensor on the board.

#### 9.5.1 Mirror-internal `sensors/*` notification namespace

The mirror uses **two distinct namespaces** by design:

| Namespace | Layer | Where it lives |
|---|---|---|
| `rmms/<uuid>/...` | MQTT wire contract | Firmware ↔ broker ↔ subscribers (Radxa, mirror) |
| `sensors/<field>` | MM² in-process notification bus | `MMM-CustomMQTTBridge` ↔ `MMM-SensorUI` |

`MMM-CustomMQTTBridge` is the adapter between the two: it does mTLS to the
broker, parses the §9.2 JSON envelope, and re-broadcasts each logical
field as a `MQTT_SENSOR_UPDATE` notification with payload
`{topic: "sensors/<field>", message: "<string>"}` so MMM-SensorUI's
existing per-field handlers don't need to know about wire-level details
(JSON shape, envelope, quality flag, multi-field topics like `radar`).

The bridge is the **single source of truth** for this translation. The
canonical mapping table lives at the top of
`MagicMirror/modules/MMM-CustomMQTTBridge/node_helper.js`.

| Broker topic + field | Mirror notification | Value (string) |
|---|---|---|
| `rmms/<uuid>/env.v.temp_c` | `sensors/temperature` | float, 1 decimal |
| `rmms/<uuid>/env.v.hum_pct` | `sensors/humidity` | int, rounded |
| `rmms/<uuid>/air.v.aqi` | `sensors/airquality` | UBA label ("GOOD" …) |
| `rmms/<uuid>/air.v.co2_ppm` | `sensors/co2` | int |
| `rmms/<uuid>/air.v.tvoc_ppb` | `sensors/tvoc` | int |
| `rmms/<uuid>/radar.v.heart_bpm` | `sensors/heartrate` | int, rounded |
| `rmms/<uuid>/radar.v.breath_bpm` | `sensors/respiratoryrate` | int, rounded |
| `rmms/<uuid>/info.v.text` | `sensors/infomessage` | text |
| `rmms/<uuid>/status` (retained) | `sensors/status` | `"online"`/`"offline"` |

**Rules:**

- The firmware **only** knows the `rmms/...` namespace. It does not emit
  `sensors/...` anything.
- The MMM-SensorUI tile code **only** consumes `sensors/...`
  notifications. It does not parse JSON, does not know the wire
  envelope, does not know about `q` / `wall_ms` / `seq`.
- Translation, quality-flag handling, and stringification all live in
  the bridge's node_helper. Adding a new sensor field is a 3-line
  change in one place (one `send("name", val)` call in the right `if`
  branch) — nowhere else.
- Both namespaces appear in this file deliberately: §9.1 / §9.2 / §9.5
  describe the **broker contract** the firmware and other subscribers
  see; this §9.5.1 describes the **mirror-internal contract** the
  bridge and `MMM-SensorUI` agree on. They are not synonyms.

### 9.6 FHIR contract (firmware ↔ Radxa data format)

**The firmware emits sensor JSON (per §9.2). The Radxa emits FHIR JSON.
There is no FHIR code on the MCU.**

The two formats are *both* JSON but serve different purposes:

- **Sensor JSON** (firmware → broker): compact, schema-stable, ~120–200 bytes
  per sample, no clinical terminology. Built by `snprintf` templates.
- **FHIR JSON** (Radxa → hospital): FHIR R4 `Observation` resources with
  LOINC codes, UCUM units, `Patient` and `Device` references, and full FHIR
  envelope. ~400–600 bytes per resource. Built by `fhir.resources` Pydantic
  models.

This boundary is non-negotiable. FHIR on a microcontroller fails on every
axis: bandwidth (FHIR resources are 3–5× larger than the sensor JSON
samples), clinical context (FHIR Observations need `subject` and
`effectiveDateTime` that the MCU cannot produce), versioning (R4 → R5 or
`nl-core` profile adoption would require reflashing every device), and
validation surface (FHIR cardinality / value set / terminology checks have
no business running in C on a Cortex-M33).

The sensor JSON schema (§9.2) was designed so that every field needed for a
FHIR Observation is either present in the JSON payload or derivable from
context the Radxa already has. The mapping:

| Sensor JSON field / topic | FHIR Observation field | Resolution |
|---|---|---|
| Topic `rmms/<uuid>/<sensor>` | `code.coding[]` (LOINC) | Radxa config table: `temp_c→8310-5`, `hum_pct→19736-7`, `pres_hpa→3140-1`, `heart_bpm→8867-4`, `breath_bpm→9279-1`, `presence→76689-9`. Indoor air-quality fields (`co2_ppm`, `tvoc_ppb`, `aqi`) have no widely-adopted LOINC codes; the Radxa team picks codes (likely SNOMED or custom URN until standardized) when wiring the `/air` topic. |
| `<uuid>` (from topic) | `device.reference` → `Device/<uuid>` | Radxa provisions the `Device` FHIR resource once at deployment |
| (Radxa local DB) | `subject.reference` → `Patient/<id>` | Radxa holds the `device_uuid → patient_id` binding; never on the MCU |
| `v.<field>` (sensor value) | `valueQuantity.value` + `unit` + `system` + `code` | Radxa applies UCUM mapping per sensor field |
| `q` (quality flag) | `status` | `0→final`, `1→preliminary`, `2→preliminary`, `3→entered-in-error` (or drop) |
| `wall_ms` (if ≠ `-1`) | `effectiveDateTime` | If `-1` (no RTC sync), Radxa substitutes `received_at − estimated_latency` and sets `status: preliminary` regardless of `q` |
| `seq` | `identifier[]` with system `urn:rmms:seq` | For deduplication / idempotency on resubmit |
| Implicit | `category[]` | Radxa decides: `vital-signs` for cardiopulmonary; ambient measurements may need a custom category |

**Rules enforced by this boundary:**

- The firmware **never** emits FHIR resources. Not `Observation`, not
  `Device`, not anything.
- The firmware **never** references LOINC codes, UCUM units, FHIR resource
  names, or any clinical terminology. If you find a LOINC code as a string
  literal anywhere in this repo, delete it.
- The firmware **never** knows the patient's identity, name, identifier, or
  any PII. It only knows its own UUID.
- The firmware **never** knows the hospital endpoint URL, FHIR server
  location, authentication credentials for the hospital, or any other
  downstream configuration.
- Adding a new sensor field requires: (1) adding it to the JSON schema in
  §9.2, (2) adding a topic if it's a new sensor, (3) the Radxa team adding
  the LOINC/UCUM mapping. No coordination with the hospital team is needed
  at the firmware level for new sensors.

**Validation chain (Radxa-side, documented here for cross-team clarity):**

```
sensor JSON → typed domain object → FHIR Observation builder
                                          ↓
                          fhir.resources / HAPI validator
                                          ↓
                              valid ────► local SQLite
                                          ↓
                              invalid → dead-letter store + raw JSON retained
                                          ↓
                              POST Bundle (transaction) to hospital FHIR endpoint
```

The Radxa **never silently drops** an invalid sample. This matches §13.6 — the
previous group's `except: pass` pattern is rejected at every layer of the
system, not only the MCU.

---

## 10. Security model

### 10.1 Threat model
- **Both transports are treated as hostile.** The cable is short and inside a
  sealed enclosure, but we apply the same mTLS scheme to USB and Wi-Fi for
  uniformity, auditability, and defense in depth. A swapped cable, a malicious
  bridge process on the tablet, or a wrong-tablet plug-in are all defeated by
  cert mismatch.
- **Wi-Fi** path also faces network-level observation and impersonation —
  handled by the same mTLS that protects USB.

### 10.2 Static certificate provisioning
- **ECDSA P-256 only.** TLS 1.2 minimum, TLS 1.3 preferred. Cipher suites
  restricted to `ECDHE-ECDSA-AES128-GCM-SHA256` and
  `ECDHE-ECDSA-AES256-GCM-SHA384` (TLS 1.2) or `TLS_AES_128_GCM_SHA256` and
  `TLS_AES_256_GCM_SHA384` (TLS 1.3). **No RSA, no CBC, no SHA-1 anywhere.**
- **Keys are generated off-device, at a controlled provisioning station, and
  written to littlefs at factory time.** The device does not generate keys, does
  not produce CSRs, does not own the project CA private key, and cannot rotate
  certs without re-provisioning. This is deliberate.
- Provisioning produces three artifacts per device:
  - `/certs/dev.key` — device ECDSA P-256 private key (DER).
  - `/certs/dev.crt` — device cert (DER), signed by the project CA.
  - `/certs/ca.der` — project CA cert (DER), identical across all devices.
- Two **non-device** identities are also issued by the same CA via
  `scripts/provision_ca.sh`, for consumers and operators of the topic tree
  (the firmware never sees these — they live only on consuming hosts):
  - **Mirror cert** (CN `mirror-<short-id>`): used by MagicMirror²'s MQTT
    bridge module to subscribe to the device tree. Mosquitto ACL: `topic read rmms/+/+`.
  - **Operator cert** (CN `operator-<short-id>`): used by the PoC laptop (and,
    eventually, by the Radxa relay) to publish the downlink topics `info` and
    `screen`. Mosquitto ACL: `topic write rmms/+/info`, `topic write rmms/+/screen`,
    nothing else.
- The same cert chain is presented on both USB-CDC and Wi-Fi paths. Mosquitto
  validates the device cert against the project CA. The device validates the
  Mosquitto server cert against the same CA (mutual auth, same trust anchor).
- **CA private key never touches a deployed device or a developer laptop.** It
  lives on a single offline provisioning workstation. Compromise of that
  workstation invalidates the entire fleet.

### 10.3 Session lifecycle
- TLS session tickets (RFC 5077) are enabled. On transport switch (USB ⇄ Wi-Fi)
  the dormant context retains its ticket for fast resumption (~10 ms vs ~80 ms
  full handshake).
- mbedTLS configured with `MBEDTLS_SSL_RENEGOTIATION` **disabled**. Sessions
  are torn down and re-established rather than renegotiated.

### 10.4 What we are not doing in v1
- No on-device key generation, no CSR flow, no over-the-air cert enrollment.
  Re-provisioning requires physical access and a factory tool.
- No automatic cert rotation. If a cert is compromised, the device is
  re-provisioned. Document expiry dates and have a re-provisioning procedure.
- No OTP-based key storage. Keys live in littlefs in plaintext relative to a
  physical attacker with flash access. This is a known v1 limitation; v2 should
  evaluate RP2350 OTP and encrypted flash regions.
- No signed/encrypted OTA. BOOTSEL reflash + SWD is the update path. Document
  this as a known limitation; do not pretend otherwise.

### 10.5 What we never do
- Hardcoded credentials. Anywhere. Including test fixtures — use environment
  variables or generated fixtures.
- The previous group's `USER=new_dev` / `PASSWORD=new123` MQTT auth: **not
  reproduced**. mTLS authenticates the client cryptographically; stacked
  password auth on top of mTLS is security theater and broadens the attack surface.
- `printf` of secret material to **either** CDC interface.
- Any code path that disables cert validation, even temporarily for development.
  Use a separate development CA and dev-only device certs if you need to test
  against a non-production broker.

---

## 11. Persistent storage

`littlefs` mounted on a 256 KB region at the top of QSPI flash. **All
firmware-managed config and state files are JSON** — same format as the wire
payloads (§9.2). Cert artefacts retain their cryptographic encodings
(DER/PEM) because those are dictated by X.509, not by us.

```
/cfg/
  wifi.json      # {"ssid":"...","psk":"...","country":"NL"}
  broker.json    # {"host":"tablet.local","ip":"192.168.1.50","port":8883}
  device.json    # {"uuid":"...","label":"...","location":"..."}
  sensors.json   # {"radar":"bha2"|"c1001", ...}
/certs/
  ca.der         # project CA (factory-written, treated as read-only)
  dev.crt        # device cert, factory-written
  dev.key        # device private key (DER, ECDSA P-256), factory-written
/state/
  last_seq.json  # {"env":1234,"radar":5678,"light":910}
  boot_count.json  # {"count":42}
```

All reads/writes through `components/storage`. **No file I/O elsewhere.**
All JSON encoding via the snprintf templates in `components/json/`; all JSON
parsing via the jsmn tokenizer there. **No second JSON library, no CBOR
library, no ad-hoc parsers.**

### 11.1 Why JSON for storage as well as wire

Earlier revisions of this document specified CBOR for on-device storage and
JSON for the wire — two formats for what is conceptually the same data
shape. That inconsistency is removed. Reasons:

- **One serialization library.** snprintf + jsmn covers wire encoding,
  wire command parsing, storage write, storage read, and OLED debug
  rendering. CBOR added a second library with its own bugs to know about.
- **Human-debuggable storage.** Mount littlefs via picotool over USB,
  `cat /cfg/wifi.json` — operator can read what the device thinks its
  config is, without any decoder.
- **Smaller code footprint than CBOR + JSON together.** jsmn is ~600 lines;
  a full CBOR library is several thousand.
- **No "which encoding is this?" question** when reading a litlefs file
  during incident response.

The bandwidth/size argument for CBOR was about wire traffic, not about
storage. Storage is read once at boot — the size difference (a few hundred
bytes per file) is irrelevant against 4 MB of available flash.

### 11.2 Atomic writes

Every storage write uses the **rename-on-close** pattern:

1. Write new content to `path.json.tmp`.
2. `fsync` the file.
3. `rename(path.json.tmp, path.json)` — atomic on littlefs.

`components/storage` exposes a `storage_write_atomic(path, bytes, len)`
helper. All callers use it. Direct `fopen("w")` on a config path is a code
review reject — partial writes from power loss during config update
otherwise produce corrupt JSON on next boot.

### 11.3 Schema versioning

Each storage file's top-level object includes a `"_v"` integer field:

```json
{"_v":1,"ssid":"...","psk":"...","country":"NL"}
```

On read, mismatch between expected and actual `_v` triggers a documented
migration path. For v1 firmware, only `_v=1` exists. Migrations beyond that
are ADR territory.

The `/certs/` directory is populated at factory provisioning time and is never
modified by firmware in the field. Re-provisioning is the only supported way
to change a device's identity.

---

## 12. Logging & telemetry

- `components/log` exposes `LOG_E / LOG_W / LOG_I / LOG_D / LOG_V` macros and a
  per-module tag.
- **Target:** output routed to **CDC1**, not CDC0 (CDC0 is data only).
  **v1 (current phase, USB tablet link deferred — §2.1):** logs go to a
  standalone **USB-serial dev console** (`pico_stdio_usb`) that enumerates as a
  plain COM port on the developer's PC. This is a debug aid only, separate from
  the tablet data path; when the 2-CDC USB tablet link lands, logging moves to
  CDC1 per the target design. A bring-up validation of this console lives at
  `test/bringup/usb_console.c`.
- Compile-time `LOG_LEVEL` filter. Default `LOG_LEVEL_INFO` for release,
  `LOG_LEVEL_DEBUG` for dev.
- Critical errors also publish a single line to `rmms/<uuid>/log` (QoS 0,
  best-effort).
- Never log a secret, a key, or a cert. Audit on PR.

---

## 13. Coding standards

### 13.1 Language
- **C11.** No GNU extensions except `__attribute__((packed))` for wire structs
  and `__builtin_expect` for fast paths.
- No C++. If a future contributor wants C++, write an ADR first.

### 13.2 Style
- 4-space indent, no tabs. 100-column soft limit, 120 hard.
- `snake_case` for functions and variables, `SCREAMING_SNAKE_CASE` for macros
  and enum members, `PascalCase` for typedef'd structs (`SensorSample` not
  `sensor_sample_t` — but use a `_t` suffix for primitive typedefs only).
- One public header per component, named after the component. Internal headers
  live in the component's `private/` subdirectory.
- `clang-format` config at repo root. CI rejects non-formatted PRs.

### 13.3 Errors
- All fallible functions return `err_t` (defined in `components/board/err.h`).
  `ERR_OK = 0`. Never use `errno`. Never use `bool` to signal failure.
- `goto cleanup` is allowed and encouraged for resource unwinding. The Linux
  kernel uses it for a reason.
- `assert()` is for invariants that must hold by code construction; it
  compiles out in release builds. Use `panic()` for invariants that should
  halt the device.

### 13.4 Memory
- **No `malloc` in steady state.** All buffers, queues, and TLS contexts
  allocated at init. `malloc` is permitted only during boot before tasks start.
- Stack sizes per task are declared in §7.1 and audited with `uxTaskGetStackHighWaterMark`
  in `app_main`'s supervisor loop.
- No VLAs, no `alloca`.

### 13.5 Concurrency
- Shared state requires a `SemaphoreHandle_t` mutex with a documented lock order.
  Document the order in the component's header.
- ISRs use `*FromISR` API variants only, and `portYIELD_FROM_ISR` when waking
  a higher-priority task.
- No `taskDISABLE_INTERRUPTS()` outside the BSP layer.

### 13.6 Anti-patterns
Do not do any of these. They are common in pico-sdk example code and bad for
production:
- `printf` for status output (use `LOG_*`).
- `sleep_ms` in application code (use `vTaskDelay`).
- `multicore_launch_core1` directly (FreeRTOS owns both cores).
- `cyw43_arch_lwip_begin/end` around long-running ops (it is a hint, not a lock —
  use proper synchronization).
- Mixing pico-sdk's `queue.h` with FreeRTOS queues (pick one — we picked FreeRTOS).

---

## 14. Testing

### 14.1 Host unit tests
- CMocka, built natively with the host's compiler against the same source files.
- Drivers must be split into a "logic" half (testable on host) and a "HAL" half
  (Pico-only). The previous group put I²C transactions in the same function as
  sample averaging — do not.
- CI runs host tests on every push.

### 14.2 Hardware-in-the-loop
- A scripted bring-up harness drives:
  - USB enumeration → TLS handshake → MQTT CONNACK timing.
  - Wi-Fi failover within 5 s of USB unplug (including TLS handshake on Wi-Fi).
  - mTLS handshake on first connection of each transport.
  - Session ticket resumption on transport swap (<20 ms).
  - Sample rate verification per sensor (env 1 Hz, radar at native rate,
    light 1 Hz).
- Targets to beat (relative to the previous group's plaintext-MQTT baseline,
  with TLS overhead budgeted in):
  - First-connect handshake (TLS + MQTT CONNECT/CONNACK): **< 500 ms** on USB,
    **< 1 s** on Wi-Fi.
  - Steady-state sense-to-broker latency: **< 1 s** (same as previous baseline —
    TLS only adds handshake cost, not per-message cost).
  - MQTT publish round-trip (post-handshake): **< 150 ms** on USB, **< 250 ms**
    on Wi-Fi.
  - Sustained throughput: **≥ 700 B/s** without backpressure.

The TLS handshake budget is one-time per connection, not per message. If your
steady-state latency is significantly worse than the MicroPython baseline,
something is wrong — it is not TLS.

### 14.3 Soak tests
- 48-hour continuous run on both transports before any tag is cut.
- Memory high-water-mark logged hourly; any growth is a leak until proven otherwise.

---

## 15. Bring-up order

Do not skip ahead. Each step assumes the previous works.

1. **Blink** — bare metal, no FreeRTOS. Verify toolchain, BOOTSEL flash, SWD.
2. **FreeRTOS hello** — one task, blinks at 1 Hz. Verify SMP boot.
3. **CDC1 logs** — `LOG_I` over CDC1, viewable in minicom.
4. **littlefs mount** — read-modify-write a counter at `/state/boot_count.json`.
5. **Env (AHT21 or BME280)** — env_task driving the env_driver_t v-table,
   selected via `/cfg/sensors.json` `"env"` field (default AHT21 on the
   current PCB; BME280 if that footprint is populated). Publishes JSON
   samples to a local debug sink (no MQTT yet). Validates the snprintf
   encoder pattern from §9.2 — including the `"pres_hpa": null` path when
   the AHT21 is selected.
5b. **ENS160** — air_task publishing JSON samples. Writes the env driver's
   last temp/hum to ENS160 compensation registers each cycle (works
   identically for AHT21 and BME280). Confirm the ~5–10 min warm-up:
   during it the driver reports `q=2` (degraded) and the AQI sits at 0/1
   until the gas heaters stabilise.
5c. **BH1750** — light_task publishing JSON samples (`{"lux": ...}`) at
   ~0.2 Hz. Continuous H-resolution mode (~120 ms per measurement);
   driver-side init does power-on + mode + initial 180 ms wait.
6. **mbedTLS + stream_cdc** — TLS handshake over CDC0 against a host-side
   `openssl s_server` configured with the project CA. This validates the cert
   chain and the BIO callbacks before any MQTT code.
7. **MQTT over TLS over CDC0** — connect to a host-side mosquitto via the byte
   bridge; verify CONNACK and a first PUBLISH.
8. **OLED** — page 1 (status) and page 2 (last env sample). Buttons cycle.
9. **mmWave radar driver A (MR60BHA2)** — implement Seeed binary protocol,
   verify presence + breath + heart rate samples.
10. **mmWave radar driver B (C1001)** — implement DFRobot AT protocol, verify
    same sample fields. Both drivers must produce identical `radar_sample_t`.
11. **Radar selection** — `/cfg/sensors.json` flag swaps drivers without rebuild.
12. **Light sensor** — ADC sampling, publish to CDC0 + MQTT.
13. **CYW43 + lwIP** — DHCP, ping, mDNS resolve of `_mqtt._tcp.local`.
14. **mbedTLS over TCP** — TLS handshake to Mosquitto via Wi-Fi. Reuse the
    exact mbedTLS config from step 6.
15. **Transport selector FSM** — full failover and recovery cycle, including
    session ticket resumption.
16. **End-to-end identity check** — boot firmware against the real tablet
    Mosquitto with the production ACL pattern. Verify client cert is accepted,
    client_id matches CN, and only `rmms/<uuid>/#` writes are permitted.
    No handshake protocol to test — just TLS + MQTT CONNECT + a publish + a
    subscribe.
17. **48-hour soak.**

---

## 16. Open questions (BLOCKING — resolve before claiming v1)

The diagram and conversations to date leave the following unspecified. The
firmware contains **TODO(spec):** markers wherever an assumption was baked in.
Resolve each, then strip the TODO.

1. **OLED controller.** Almost certainly SH1106 at 1.3″, but confirm by reading
   the bare silkscreen on the actual part.
2. ~~**Radar framing parity.**~~ **Resolved.** Bench bring-up of a live
   MR60BHA2 confirmed it does NOT use Andar `0x53 0x59` / `0x54 0x43` framing.
   It uses its own 8-byte SOF-`0x01` header with `~XOR` checksums; see §3.2.
   The two drivers stay fully separate; no shared framing helper.
3. **Tablet bridge ownership.** Who writes the `/dev/ttyACMx ↔ localhost:8883`
   transparent byte pipe on the tablet? Not this repo, but it blocks
   integration testing. Likely a small Python or Go service in the tablet repo.
   Needs to handle Android USB host permissions (`UsbManager.requestPermission`),
   Termux USB plugin, and Termux:Boot for auto-start.
4. ~~**MagicMirror² run mode.**~~ **Resolved.** Termux + Node.js running
   `npm run server` (server-only mode, no Electron) on :8080; Chrome on the
   tablet opens `http://localhost:8080` for the viewer. Auto-start via
   Termux:Boot (`~/.termux/boot/start_magicmirror` → `scripts/tablet_start_magicmirror.sh`).
   Launcher script restarts MM² on death with backoff. Production-grade
   kiosk lock (URL-bar / status-bar hiding) is a separate, optional install
   ("Fully Kiosk Browser" or similar) — not needed for v1 PoC.
5. ~~**MMM-MQTT module choice.**~~ **Resolved** — the mirror team picked a
   project-internal `MMM-CustomMQTTBridge` (lives in
   `MagicMirror/modules/MMM-CustomMQTTBridge/`). The firmware does not care
   which bridge the mirror uses; the bridge just needs to honour the mTLS
   contract (mirror cert from §10.2) and forward the raw `rmms/+/...`
   topics to `MMM-SensorUI`.
6. **RTC source.** None on the Pico 2; either get time from the tablet during
   connect (custom MQTT topic, e.g. `rmms/<uuid>/time/set`, or out-of-band over
   CDC1), or accept that `wall_ms` is null until first sync. Pick one. The
   audit notes the previous group's NTP-from-WAN approach failed catastrophically
   when NTP was unreachable — do not repeat that pattern. **This also affects
   FHIR (§9.6):** without a real `wall_ms`, every Observation the Radxa builds
   will be `status: preliminary`. Resolve early.
7. **Project CA hosting.** Where does the CA private key live? Air-gapped
   workstation, HSM, or YubiKey? Who has access? What is the procedure for
   re-signing if a device cert is lost or compromised? **The audit shows the
   previous group committed an unencrypted CA private key to the repo — this
   must not recur.** Document the operational procedure before first
   provisioning run.
8. **Cert lifetimes.** Device certs need an expiry date. 1 year? 5 years? The
   shorter the more frequent re-provisioning; the longer the more damage from
   a compromise. Recommend 2 years for v1; revisit for v2.
9. **Mosquitto ACL pattern verification.** Confirm `use_identity_as_username true`
   + `pattern write rmms/%c/#` works as intended on the Mosquitto build that
   ships with Termux. ACL test harness must run on the tablet before any
   firmware integration test.

**Resolved since last revision:**
- mmWave radar choice: dual support for Seeed MR60BHA2 and DFRobot C1001 via
  the `radar_driver_t` interface, with selection via `/cfg/sensors.json`.
- TLS scope: mTLS on both transports (USB-CDC and Wi-Fi), static cert chain.
- Face recognition: descoped from v1 entirely (see §17).
- **IR camera: dropped entirely from v1.** No SPI sensor remains.
- **Registration protocol:** dropped wholesale (audit findings + decision to
  use static factory-cert identity model — see §9.4).
- **QR codes / camera-LED enrollment:** dropped wholesale.
- **Air quality sensor added (§3.2):** ScioSense ENS160 over I²C0 supplies
  eCO₂, TVOC, and UBA AQI (1–5). New raw topic `rmms/<uuid>/air` (§9.1, §9.2.2),
  new `air_task` (§7.1), new `components/sensor_air/` (§6). Mirror displays it
  as a qualitative tile with severity check (no number).
- **Mirror UI architecture (replaces the earlier "tablet UI service" plan):**
  MagicMirror² subscribes to the firmware's raw topics directly via an
  MQTT bridge module (currently `MMM-CustomMQTTBridge`); threshold logic
  lives in JavaScript inside the custom `MMM-SensorUI` module. The
  `rmms/ui/...` namespace is retired entirely.
  Mirror and operator identities are separate certs from the same project
  CA (§10.2). See rewritten §9.5.
- **FHIR contract:** firmware emits sensor JSON, Radxa emits FHIR JSON.
  Mapping locked in §9.6.
- **Wire format choice:** JSON (RFC 8259), reversed from earlier CBOR
  decision. See §9.2 for the encoder pattern.
- **Storage format unification:** on-device config and state files are also
  JSON (formerly CBOR). One serializer, one parser, human-debuggable via
  `cat`. See §11.

---

## 17. Non-goals (v1)

State these explicitly so reviewers stop asking:

- Battery operation, sleep modes, low-power tuning.
- BLE (the CYW43 supports it; we do not use it).
- Multi-device coordination (each module talks only to its own tablet broker).
- Web UI on the device.
- Audio capture.
- Any health-data processing on the MCU (lives on Radxa).
- **Face recognition** — descoped from v1 entirely. Any future image processing
  lives on the Radxa Dragon Q6A, which has a Qualcomm Hexagon NPU (12 TOPS)
  for that workload. The MCU is not in this pipeline at all.
- **IR camera and any other image sensor** — no image sensor exists on the
  v1 sensor module. The peripheral set is fixed: environment (BME280) +
  air quality (ENS160) + radar + light + OLED.
- Encrypted OTA.
- On-device cert rotation, on-device CSR generation, on-device CA hosting.
- HTTPS, REST, or any non-MQTT protocol on the device side.

---

## 18. References

- `docs/technical-audit.md` — independent audit of the previous BAP repository.
  **Authoritative on what not to do.** Section D enumerates concrete defects;
  this firmware is constructed to defeat each one.
- `docs/BAP_report_PCB_.pdf` — previous group's PCB + firmware thesis.
  Behavioural reference for sensor protocols (BME280, C1001, HMMD) and the
  4-page OLED UI concept. **Not authoritative on registration, security, or
  architecture.**
- `docs/BAP_Protocol_Thesis2.pdf` — parallel team's QR-code / camera-LED
  enrollment protocol design. **Historical only — not implemented in this
  firmware.** Retained for context on what the system used to do.
- pico-sdk: <https://github.com/raspberrypi/pico-sdk>
- FreeRTOS-Kernel SMP port: <https://github.com/FreeRTOS/FreeRTOS-Kernel>
- littlefs: <https://github.com/littlefs-project/littlefs>
- MQTT 3.1.1: OASIS standard.
- CBOR: RFC 8949 (not used for wire format — see §9.2).
- JSON: RFC 8259 (wire format).
- jsmn (minimal JSON tokenizer): <https://github.com/zserge/jsmn>
- Mosquitto ACL patterns: <https://mosquitto.org/man/mosquitto-conf-5.html>
  (sections on `acl_file` and `use_identity_as_username`).

---

## 19. Instructions for Claude Code specifically

When working in this repository:

- **Always read this file before proposing a change.** If a proposal contradicts
  any section here, point at the section and ask before proceeding.
- **Do not add libraries.** The dependency list in §4 is closed.
- **Do not edit `board_pico2wh.h` without an ADR** — pin assignments are PCB-owned.
- **Do not invent MQTT topics.** §9.1 is the closed set.
- **Do not invent commands.** §9.3 is the closed set; new commands need the
  Radxa team's sign-off.
- **Do not invent UI-presentation topics.** The mirror subscribes to raw
  firmware topics directly and does its own thresholding in
  `MMM-SensorUI.js` (§9.5). There is no `rmms/ui/...` namespace; do not
  reintroduce one. New mirror-renderable values require either a new raw
  topic (= new sensor) or a JS-side derivation, not a firmware republish.
- **Do not stub or fake security primitives.** If a TLS feature is missing,
  surface it; do not write a no-op. The audit shows the previous group did
  exactly this (`sig = nonce`) — do not repeat it.
- **Do not introduce code paths that bypass TLS.** mTLS is uniform on both
  transports. There is no "USB is local, skip TLS" optimization — that
  shortcut was rejected. If you find yourself wanting to skip TLS for
  performance, profile first; the steady-state cost is negligible.
- **Do not add an on-device cert/key generation path.** Certs are static and
  factory-provisioned (§10.2). On-device keygen is an explicit non-goal.
- **Do not commit any private key, cert, password, SSID, or LAN-specific
  address to this repository — ever.** The audit catalogues a dozen such
  leaks in the previous repo (`docs/technical-audit.md` §D.1). Provisioning
  artifacts go in a separate, access-controlled location.
- **Do not reintroduce UDP broker discovery.** mDNS or static config only (§8.2).
- **Treat the previous group's MicroPython code as a spec, not a source.** Do
  not translate line by line.
- **Treat both radar drivers as peers.** Do not optimize for one and treat the
  other as a stub. Both must produce identical `radar_sample_t` payloads.
- **Prefer deleting code to adding it.** This repo grows slowly on purpose.
- When asked to "make it work", first answer: does it match the architecture
  in §2? If not, the answer is no.

### 19.1 Anti-patterns from the audit (do not reintroduce)

The audit (`docs/technical-audit.md`) found these recurring failure modes in
the previous codebase. Each maps to a C-equivalent prohibition for this repo:

| Previous failure | This repo's rule |
|---|---|
| `sig = nonce` (fake signing with print statement claiming real keys) | All crypto primitives are real or compile out. Logging that implies an operation occurred must match the operation. No comments claiming a TODO that "is fine for now". |
| Bare `except Exception: pass` silencing sensor errors → `None` in payload | Sensor read failures return a typed `err_t`; the sample's `q` field is set to `invalid`. Never silently substitute zero or a previous value. |
| `self.motion_state` referenced before init in `__init__` | Every struct field has an explicit initial value at construction. No "set on first use" attribute patterns. CI runs `-Wuninitialized -Werror`. |
| `gc.collect()` after `from gc import collect` (NameError at runtime) | All `#include` and symbol uses are checked by the compiler; this class of error cannot occur in C, but the principle — *the import and the use must match* — applies to component header design. |
| `pub_q` defined twice at module level with different sizes (silent shadow) | Globals defined once. CI builds with `-Wredundant-decls -Wshadow -Werror`. |
| Code dedented out of `async def main()`, executing at module import | C has no equivalent, but: no work happens before `main()` returns from init. No constructor-side-effect patterns in static initialisers. |
| Busy-wait spins on flags set by other threads (`while not connected: pass`) | Cross-task synchronization uses FreeRTOS primitives (semaphore, queue, event group). Spinning on a shared variable is a code review reject. |
| `asyncio.sleep_ms(150)` called without `await` (delay silently skipped) | C has no equivalent, but: any call that schedules work must have its return value checked. Function signatures encode whether they're synchronous. |
| Bootstrap creds hardcoded (`new_dev` / `new123`) | All credentials live in `/cfg/` or `/certs/` in littlefs, populated at provisioning. Source contains no credential strings, not even as defaults. |
| TLS SNI hardcoded to wrong value (`"server"`) with a stale comment to fix it | SNI is set from the broker hostname in `/cfg/broker.json`. No string literal for SNI anywhere in `tls_context.c`. |
| Unencrypted CA / device keys committed to repo | `.gitignore` blocks `*.key`, `*.pem`, `*.der`, `*.crt`, `creds/`, `certs/`. Pre-commit hook scans for ASN.1 / PEM headers. |
| BME280 stale-read bug (reads return previous ADC if measurement not re-triggered) | All sensor `read_sample` calls are self-contained: they trigger, wait, and read in one call. No "must call this other function first" preconditions in the driver API. |
| `light_cfg` parameter accepted but never stored as `self.light_cfg` | Constructor/init functions store every parameter or document explicitly why they don't. `-Wunused-parameter -Werror`. |
| 1-day-backdated CA cert "for testing purposes" left in production code | Provisioning artifacts have no test-vs-prod toggles. The factory tool produces one kind of cert. Test certs use a dedicated test CA on a separate workstation. |
| Two Mosquitto configs (port 1884 plain + port 8883 mTLS) required but only one documented | Only `:8883` mTLS. There is no second listener. If a future change needs one, it is an ADR. |

Anything matching these patterns is rejected at review, regardless of how
convenient it would be in the short term.
