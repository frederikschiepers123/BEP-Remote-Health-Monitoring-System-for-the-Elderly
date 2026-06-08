# Bring-Up Procedure

Step-by-step hardware bring-up checklist.  **Do not skip steps.**  Each step assumes the previous one works.

---

## Prerequisites

- Raspberry Pi Pico 2 WH flashed via BOOTSEL or SWD.
- `PICO_SDK_PATH` set to a v2.x checkout of `raspberrypi/pico-sdk`.
- `arm-none-eabi-gcc` ≥ 13.2 on PATH.
- CMake ≥ 3.22 on PATH.
- A second Pico flashed as `debugprobe` (or a Picoprobe) connected via SWD for step 1+.
- Host PC: Linux or WSL2.  Native Windows toolchain not recommended.

### Build setup (run once)

```bash
git submodule update --init --recursive
mkdir build && cd build
cmake -DPICO_BOARD=pico2_w -DCMAKE_BUILD_TYPE=Debug ..
```

For release builds:

```bash
cmake -DPICO_BOARD=pico2_w -DCMAKE_BUILD_TYPE=RelWithDebInfo -DNDEBUG=1 ..
```

---

## Step 1 — Blink (bare metal, no FreeRTOS)

**Goal:** verify toolchain, BOOTSEL flash procedure, and SWD connectivity.

Write a minimal `main()` that blinks the onboard LED via `cyw43_arch`:

```c
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
int main(void) {
    cyw43_arch_init();
    while (true) {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
        sleep_ms(500);
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
        sleep_ms(500);
    }
}
```

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

**Pass criteria:** LED blinks at 1 Hz.

---

## Step 2 — FreeRTOS hello (one task)

**Goal:** verify SMP boot, FreeRTOS scheduler starts, both cores enumerated.

Write a single FreeRTOS task that blinks at 1 Hz using `vTaskDelay`.  Confirm no scheduler-start hang.

**Pass criteria:** LED blinks; no hard fault on startup; `uxTaskGetNumberOfTasks() == 1`.

---

## Step 3 — CDC1 logs

**Goal:** verify `LOG_I` output is visible over CDC1.

Connect CDC1 to the host:

```bash
minicom -D /dev/ttyACM1 -b 115200
```

If CDC1 is the first enumerated device, it may be `/dev/ttyACM0`.  Check `dmesg | tail -5` after plug-in.

Add `LOG_I("Hello from CDC1 at tick %lu", (unsigned long)tick)` to the task loop.

**Pass criteria:** log lines appear in minicom at 1 Hz.

---

## Step 4 — littlefs mount

**Goal:** verify flash mount and basic KV read/write.

```c
err_t err = storage_mount();
assert(err == ERR_OK);

uint32_t count = 0;
size_t len = 0;
storage_read("/state/boot_count.cbor", &count, sizeof(count), &len);
count++;
storage_write("/state/boot_count.cbor", &count, sizeof(count));
LOG_I("Boot count: %lu", (unsigned long)count);
```

**Pass criteria:** boot count increments on each power cycle; no FS corruption over 10 cycles.

---

## Step 5 — BME280

**Goal:** `env_task` publishes CBOR samples to a local debug sink (not yet MQTT).

- Verify I²C wiring (GPIO4/5) and 4.7 kΩ pull-ups.
- Create `env_task` only; log each `EnvSample` via `LOG_I`.
- Expected: temperature in a reasonable range (~20–30 °C), humidity 20–80 %, pressure ~950–1050 hPa.

**Pass criteria:** 1 Hz log entries with plausible values; no `ERR_IO` from driver; `q=0`.

---

## Step 6 — mbedTLS + stream_cdc (TLS handshake over CDC0)

**Goal:** validate cert chain and mbedTLS BIO callbacks before any MQTT code.

On the host, start an `openssl s_server` configured with the project CA:

```bash
# On host — replace paths with your actual provisioning artifacts
openssl s_server \
    -accept 8883 \
    -cert server.crt \
    -key server.key \
    -CAfile ca.der \
    -Verify 1 \
    -tls1_2
```

In parallel, run the USB-MQTT bridge (dumb byte pipe from `/dev/ttyACM0` to `localhost:8883`):

```bash
# Example with socat:
socat FILE:/dev/ttyACM0,raw,echo=0 TCP:localhost:8883
```

Firmware side: call `tls_context_init()` + `tls_context_handshake()` over `stream_cdc`.  Log the result.

**Pass criteria:**
- `openssl s_server` reports a successful TLS handshake.
- `tls_context_handshake()` returns `ERR_OK`.
- SNI matches the hostname from `/cfg/broker.cbor` (not the literal string `"server"`).
- Client certificate is accepted by the server CA.

---

## Step 7 — MQTT over TLS over CDC0

**Goal:** full MQTT CONNECT + first PUBLISH via the USB bridge.

On the host, start Mosquitto with mTLS:

```bash
# /tmp/mosquitto-test.conf:
# listener 8883
# cafile /path/to/ca.der
# certfile /path/to/server.crt
# keyfile /path/to/server.key
# require_certificate true
# use_identity_as_username true

mosquitto -c /tmp/mosquitto-test.conf -v
```

Firmware: call `mqtt_client_connect()`, then `mqtt_client_publish()` to `rmms/<uuid>/status` with payload `"online"`.

**Pass criteria:**
- Mosquitto log shows `CONNACK` sent to the device.
- Device log shows `"MQTT connected"`.
- `mosquitto_sub -h localhost -p 8883 --cafile ca.der --cert client.crt --key client.key -t "rmms/#"` receives the `"online"` publish.

---

## Step 8 — OLED

**Goal:** SH1106 displays page 1 (status) and page 2 (last env sample); buttons cycle pages.

- Verify I²C address: `0x3C` (default) or `0x3D`.  Read the controller silkscreen — confirm SH1106, not SSD1306 (CLAUDE.md §16 Q2).
- Init SH1106 via `sh1106_init()`; draw `"RMMS SENSOR MODULE"` on page 0.
- Wire buttons to GPIO20/21; verify `ui_input_init()` triggers on press.

**Pass criteria:** 4 pages cycle on button press; env values update at 1 Hz; no I²C errors.

---

## Step 9 — mmWave radar driver A (MR60BHA2)

**Goal:** Seeed binary protocol parsed; `presence`, `breath_rpm`, `heart_bpm` in samples.

- Verify UART wiring (GPIO0/1), 115200 baud.
- Set `/cfg/sensors.cbor` = `"bha2"`.
- Log each `RadarSample` via `LOG_I`.
- Wave hand in front of radar to trigger presence.

**Pass criteria:**
- `presence=true` when a person is nearby.
- `breath_rpm` and `heart_bpm` non-zero with person present.
- `q=0` on good samples; `q=2` on ghost readings (no vitals with presence).
- TODO(spec): verify actual frame structure against datasheet once hardware is on bench (CLAUDE.md §16 Q3).

---

## Step 10 — mmWave radar driver B (C1001)

**Goal:** DFRobot AT protocol parsed; same `RadarSample` fields as step 9.

- Set `/cfg/sensors.cbor` = `"c1001"`.
- Repeat step 9 verification.

**Pass criteria:** identical `RadarSample` output for same physical scenario as MR60BHA2.

---

## Step 11 — Radar selection

**Goal:** config flag swaps drivers without rebuild.

- Flash the same firmware.
- Write `"bha2"` to `/cfg/sensors.cbor` via the provisioning tool; power cycle; verify BHA2 driver logs.
- Write `"c1001"`; power cycle; verify C1001 driver logs.

**Pass criteria:** correct driver name in `LOG_I` on startup; no code change required.

---

## Step 12 — Light + IR camera

**Goal:** ADC light sampling works; IR camera stub acknowledged.

**Light:**
- Verify GPIO26 wired to LDR voltage divider midpoint.
- Cover/uncover the LDR; verify lux changes in `LightSample` log.

**IR camera:**
- TODO(spec): IR camera part must be confirmed before real driver work (CLAUDE.md §16 Q1).
- Current stub publishes `q=3`; verify this appears in `q_ir_meta` without crashing.

**Pass criteria:** light task publishes at 1 Hz with plausible lux values; IR task runs without hard fault.

---

## Step 13 — CYW43 + lwIP

**Goal:** Wi-Fi DHCP, ping, mDNS resolve of `_mqtt._tcp.local`.

- Provision `/cfg/wifi.cbor` with SSID and PSK.
- Provision `/cfg/broker.cbor` with mDNS name and static fallback IP.
- Verify `cyw43_arch_init_with_country(CYW43_COUNTRY_NETHERLANDS)`.

```bash
# From host: verify Pico appears on the LAN
ping <pico-ip>
```

**Pass criteria:** DHCP assigned; ping succeeds; mDNS resolves broker hostname.

---

## Step 14 — mbedTLS over TCP (Wi-Fi)

**Goal:** TLS handshake to Mosquitto via Wi-Fi using the **exact same** mbedTLS config as step 6.

Re-use the same project CA and device cert.  The only difference is `stream_tcp` instead of `stream_cdc`.

**Pass criteria:**
- TLS handshake succeeds over Wi-Fi within 1 s (target per CLAUDE.md §14.2).
- Same cipher suite and cert chain accepted.
- SNI from `/cfg/broker.cbor`.

---

## Step 15 — Transport selector FSM

**Goal:** full USB↔Wi-Fi failover and recovery, including session ticket resumption.

Test sequence:
1. Boot with USB connected → verify USB_ACTIVE state.
2. Unplug USB → verify transition to WIFI_ACTIVE within 5 s.
3. Replug USB → verify transition back to USB_ACTIVE.
4. Measure TLS resumption time on second handshake (target < 20 ms per CLAUDE.md §14.2).

**Pass criteria:**
- All state transitions complete within documented timeouts.
- MQTT publishes continue without data loss across transport swap.
- Session ticket resumption observed in mbedTLS debug logs.

---

## Step 16 — End-to-end identity check

**Goal:** firmware connects to the real tablet Mosquitto with the production ACL pattern.

On the tablet (Termux), verify Mosquitto ACL config:

```
# /data/data/com.termux/files/home/mosquitto.conf
listener 8883
cafile /path/to/ca.der
certfile /path/to/server.crt
keyfile /path/to/server.key
require_certificate true
use_identity_as_username true
acl_file /path/to/acl.conf

# acl.conf:
pattern write rmms/%c/#
pattern read rmms/%c/cmd
```

Boot the firmware.  Verify:
1. Client cert accepted; `client_id` matches CN from cert.
2. Firmware can publish to `rmms/<uuid>/status` → succeeds.
3. Firmware cannot publish to `rmms/<other-uuid>/status` → rejected (ACL violation).
4. Firmware receives a `cmd` message sent to `rmms/<uuid>/cmd`.

**Pass criteria:** all four checks pass without modifying per-device broker config.

---

## Step 17 — 48-hour soak test

**Goal:** stability verification before any tag is cut.

```bash
# On host: monitor for MQTT disconnections and log errors
mosquitto_sub -h <broker-ip> -p 8883 \
    --cafile ca.der --cert client.crt --key client.key \
    -t "rmms/#" | ts '[%Y-%m-%d %H:%M:%S]' | tee soak.log
```

During the soak:
- Both USB and Wi-Fi transports must be exercised (unplug/replug USB at least once per hour).
- Log stack high-water marks hourly; any growth is a leak until proven otherwise.
- Verify sample rates: env 1 Hz, radar at native rate, light 1 Hz.
- Verify MQTT publish round-trip < 150 ms on USB, < 250 ms on Wi-Fi.

**Pass criteria:**
- No unplanned disconnections in 48 h.
- No memory growth in high-water marks.
- All sensor queues drain without overflow (check dropped-sample counter at 0 or stable).
- No `ERR_IO` errors in soak.log.

---

## Performance targets (CLAUDE.md §14.2)

| Metric | Target |
|---|---|
| First TLS + MQTT CONNECT on USB | < 500 ms |
| First TLS + MQTT CONNECT on Wi-Fi | < 1 s |
| Steady-state sense-to-broker latency | < 1 s |
| MQTT publish round-trip (USB) | < 150 ms |
| MQTT publish round-trip (Wi-Fi) | < 250 ms |
| Wi-Fi failover from USB unplug | < 5 s |
| Session ticket resumption (transport swap) | < 20 ms |
| Sustained throughput | ≥ 700 B/s |
