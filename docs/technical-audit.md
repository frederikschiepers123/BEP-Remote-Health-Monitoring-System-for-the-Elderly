# Technical Audit — Remote Health Monitoring System (Previous BAP)

> Source tree: `bestanden_vorige_BAP/`  
> Audit date: 2026-05-20

---

## A. I²C / UART / SPI Transaction Sequences

### A.1 BME280 — Temperature, Humidity, Pressure (I²C)

**Address:** `0x76` (default) / `0x77` (alternate)  
**File:** `sensor_module/microcontroller_files/lib/BME280.py`

**Init sequence:**
1. Read factory calibration registers in order:
   - Temp: `0x88` (dig_T1 U16LE), `0x8A` (dig_T2 S16LE), `0x8C` (dig_T3 S16LE)
   - Pressure: `0x8E`–`0x9E` (dig_P1–P9, nine S/U16LE reads)
   - Humidity: `0xA1` (dig_H1 U8), `0xE1` (dig_H2 S16LE), `0xE3` (dig_H3 U8), compound reads of `0xE4`/`0xE5`/`0xE6`/`0xE7` for dig_H4–H6
2. `write8(0xF2, meas)` — set humidity oversampling (`REGISTER_CONTROL_HUM`)
3. `write8(0xF4, 0x3F)` — set temp+pressure 16× oversampling + normal mode (`REGISTER_CONTROL`)

**Per-measurement read sequence:**
1. `write8(0xF2, meas)` — set humidity oversampling
2. `write8(0xF4, meas)` — set temp+pressure oversampling + forced mode
3. `sleep_us(computed)` — `1250 + 3×(2300×(1<<mode)) + 2×575` µs
4. Read `0xFA`, `0xFB`, `0xFC` — raw temperature (3 bytes MSB-first, right-shift 4)
5. Read `0xF7`, `0xF8`, `0xF9` — raw pressure (same layout)
6. Read `0xFD`, `0xFE` — raw humidity (2 bytes big-endian)

**Known issue:** `read_raw_pressure` and `read_raw_humidity` do not re-trigger a measurement; they return stale ADC results if called without first calling `read_raw_temperature`.

---

### A.2 AHT20 / AHT10 — Temperature and Humidity (I²C)

**Address:** `0x38`  
**File:** `sensor_module/microcontroller_files/lib/ahtx0.py`

**Init sequence:**
1. `sleep_ms(20)` — power-on stabilisation
2. Write `[0xBA]` — soft reset (`AHTX0_CMD_SOFTRESET`)
3. `sleep_ms(20)`
4. AHT10: write `[0xE1, 0x08, 0x00]`; AHT20: write `[0xBE, 0x08, 0x00]` — calibrate
5. Poll status byte: wait until bit 7 (`BUSY`) clears and bit 3 (`CALIBRATED`) is set

**Per-measurement read sequence:**
1. Write `[0xAC, 0x33, 0x00]` — trigger measurement
2. Poll busy bit until clear (`_wait_for_idle`)
3. `readfrom_into(0x38, buf)` — read 6 bytes; humidity in `buf[1:4]` (20 bits), temperature in `buf[3:6]` (20 bits), parsed via bit-masking

---

### A.3 ENS160 — AQI / TVOC / eCO2 (I²C)

**Address:** `0x53`  
**File:** `sensor_module/microcontroller_files/lib/ens160.py`

**Init sequence:**
- `write_register(0x10, bytes([0x02]))` — set operating mode to Standard (continuous)

**Register map (all reads):**

| Register | Width | Value / Parsing |
|----------|-------|-----------------|
| `0x00` | 2 B | Part ID |
| `0x02` | 2 B | Firmware version |
| `0x20` | 1 B | Status byte |
| `0x21` | 1 B | AQI (`& 0x07`) |
| `0x22` | 2 B | TVOC ppb — **little-endian** `(data[1]<<8)\|data[0]` |
| `0x24` | 2 B | eCO2 ppm — little-endian |
| `0x30` | 2 B | Temperature — **big-endian** `(data[0]<<8)\|data[1]`, formula: `raw/64.0 − 273.15` |
| `0x32` | 2 B | Humidity — big-endian, formula: `raw/512.0` |

**Note:** byte order is little-endian for TVOC/eCO2 but big-endian for Temperature/Humidity — asymmetric within the same driver.

---

### A.4 DFRobot C1001 mmWave Radar — Presence / Heart Rate / Breath (UART)

**Baud rate:** 115200, TX=pin 16, RX=pin 48 (from `config.json`)  
**File:** `sensor_module/microcontroller_files/lib/DFRobot_HumanDetection_mpy.py`

**Frame format (both directions):**
```
[0x53][0x59][con][cmd][len_H][len_L][payload...][checksum][0x54][0x43]
```
Checksum = 8-bit sum of all preceding bytes.

**Receive state machine:** `CMD_WHITE → CMD_HEAD(0x59) → CMD_CONFIG → CMD_CMD → CMD_LEN_H → CMD_LEN_L → CMD_DATA → CMD_END_H(0x54) → CMD_END_L(0x43)`; result byte at `ret[6]`.

**Init:** send `con=0x01, cmd=0x83, payload=[0x0F]`; verify `resp[0] != 0xF5`.

**Command table:**

| Metric | con | cmd | Polling interval |
|--------|-----|-----|-----------------|
| Human presence | `0x80` | `0x81` | Every call |
| Human movement | `0x80` | `0x82` | Every call |
| Moving range | `0x80` | `0x83` | Every call |
| Distance | `0x80` | `0x84` | Every call |
| Heart rate bpm | `0x85` | `0x82` | Every 20 calls (~2 s) |
| Breathe state | `0x81` | `0x81` | Every 20 calls |
| Breathe value | `0x81` | `0x82` | Every 20 calls |

Timeout sentinel: `ret[0] == 0xF5`.

**Known issue:** `asyncio.sleep_ms(150)` after mode switch is called without `await` — the delay is silently skipped.

---

### A.5 Waveshare HMMD mmWave Radar — Presence / Distance / Energy (UART)

**Baud rate:** 115200  
**File:** `sensor_module/microcontroller_files/lib/hmmd_mpy.py`

**Init command (switch to Report Mode):**
```
FD FC FB FA  08 00  12 00 00 00 04 00 00 00  04 03 02 01
```

**Incoming report frame:**
```
Header:   F4 F3 F2 F1
Length:   2 bytes (little-endian)
Payload:
  [0]     presence  (0x00 = none, 0x01 = target)
  [1..2]  distance_raw  (little-endian uint16)
  [3..34] 16 × gate energy (little-endian uint16 each)
Tail:     F8 F7 F6 F5
```

**Motion classification thresholds:** `lower=20_000`, `upper=65_000` (hardcoded; author comment: *"??? tweak these values"*).  
**Distance formula:** `distance_raw × 0.7` metres per gate (approximate).

**Known issue:** `self.motion_state` is never initialised in `__init__`; first call to `compute_motion_state()` raises `AttributeError`.

---

### A.6 GL5516 LDR — Ambient Light (ADC, no serial bus)

**Pin:** configurable via `config.json` (`LDR_pin: 18`)  
**File:** `sensor_module/microcontroller_files/lib/light.py`

ADC initialised with `ATTN_11DB` (0–3.3 V) and `WIDTH_12BIT` (0–4095).  
Lux formula: `10 × (10000 / R_LDR)^(1/0.5)` where `R_LDR` is derived from the ADC ratio.

---

## B. MQTT Topic and Payload Schemas

Two broker ports: **1884** (TLS + username/password, bootstrap phase) and **8883** (mutual TLS, post-registration).  
Client libraries: `paho-mqtt` (server/SBC) and `umqtt.simple` (MicroPython sensor module).

---

### Phase 0 — Global Enrollment Broadcast (port 1884)

| Topic | Dir | Publisher | QoS | Payload (JSON) |
|-------|-----|-----------|-----|----------------|
| `enroll/response` | Server → all | RegistrationClient | 0 | `{"request": true}` |
| `enroll/response` | Server → all | RegistrationClient | 1 | `{"dev_key": "<uuid>", "common_name": "device_N"}` |
| `enroll/request` | SBC → server | SBCClient | 1 | `{"dev_key": "<ECDSA PEM pubkey>", "device_type": "SBC"}` |
| `enroll/request` | SM → server | SMClient | 0 | `{"dev_key": "<uuid_str>", "device_type": "sensor_module"}` |

---

### Phase 1 — Per-Device Enrollment (port 1884)

| Topic | Dir | QoS | Payload (JSON) |
|-------|-----|-----|----------------|
| `enroll/{common_name}/response` | Server → device | 1 | `{"nonce": "<hex32>", "message": "LED on"}` |
| `enroll/{common_name}/response` | Server → device | 1 | `{"label": "<4char>", "certificate": "<PEM>"}` (SM) or + `"server_certificate": "<PEM>"` (SBC) |
| `enroll/{common_name}/request` | Device → server | 1 | SBC: `{"signature": "<hex>", "uuid": "<str>", "mac": "<6-byte hex>", "add_key": "<ECDSA PEM pubkey>"}` |
| `enroll/{common_name}/request` | SM → server | 0 | `{"signature": "<nonce echoed>", "LED": <bool>, "uuid": "<str>", "mac": "<machine.unique_id() str>", "add_key": "<ED25519 PEM pubkey>"}` |
| `enroll/{common_name}/request` | Device → server | 1 | Checkup: `{"checkup": true, "common_name": "<str>"}` |

---

### Phase 2 — Verification (port 8883)

| Topic | Dir | QoS | Payload (JSON) |
|-------|-----|-----|----------------|
| `verification/{label}/request` | Device → server | 1 | `{"checkup": true, "common_name": "<str>"}` |
| `verification/{label}/response` | Server → device | 1 | `{"verify": true}` (LED on signal) |
| `verification/{label}/response` | Server → SM | 1 | `{"shutdown": true}` |
| `verification/{label}/response` | Server → SBC | 1 | `{"allowed_labels": ["<label>", ...], "shutdown": true}` |

---

### Deployment (port 8883, post-registration)

| Topic | Dir | QoS | Payload (JSON) |
|-------|-----|-----|----------------|
| `deployment/{label}/data` | SM → SBC | 1 | See schema below |
| `deployment/{label}/response` | SBC → SM | 1 | `{"ping": true}` or `{"action": "deregister"\|"deactivate"\|"activate"}` |
| `deployment/response` | SBC → all SMs | 1 | `{"ping": true, "timestamp": [year,mon,day,h,m,s,wd,yd]}` or `{"shutdown": true}` or `{"action": "deregister_sbc"}` |

**Sensor data payload schema** (`deployment/{label}/data`):
```json
{
  "device_id": "<4-char label>",
  "timestamp": [year, month, day, hour, min, sec, weekday, yearday],
  "environment": {
    "source": "BME280",
    "temp_c": float,
    "rh_pct": float,
    "pressure_hpa": float
  },
  "radar": {
    "source": "DFRobot",
    "presence": int,
    "movement": int,
    "heart_bpm": int,
    "breath_rpm": int
  },
  "light": float
}
```
Alternative `environment` schema (ENS160 + AHT20 variant):
```json
{
  "source": "ENS160_AHT20",
  "temp_c": float,
  "rh_pct": float,
  "aqi": int,
  "tvoc_ppb": int,
  "co2_ppm": int
}
```
Alternative `radar` schema (HMMD variant):
```json
{
  "source": "HMMD",
  "presence": int,
  "movement": int,
  "motion": int,
  "distance_m": float,
  "distance_raw": int,
  "existance_energy": int
}
```

**Server subscriptions on connect:**
- `RegistrationClient`: `enroll/request`, `enroll/+/request`, `verification/+/request`
- `DepClient (SBC)`: `deployment/+/data`

---

## C. Registration FSM

### C.1 State Enumeration
**File:** `registration_software/models/enums.py`

| Enum | Value | Meaning |
|------|-------|---------|
| `RegistrationStatus.SEARCHING` | `"SEARCHING"` | QR detected; LED correlation in progress |
| `RegistrationStatus.IDENTIFIED` | `"IDENTIFIED"` | LED blink frequency verified (5 Hz default) |
| `RegistrationStatus.UNAUTHORIZED` | `"UNAUTHORIZED"` | QR present but label not in expected set |
| `RegistrationStatus.NOT_DECODED` | `"NOT_DECODED"` | QR detected but unreadable |
| `RegistrationStatus.REGISTERED` | `"REGISTERED"` | Device already in device registry |
| `RegistrationStatus.OUT_OF_RANGE` | `"OUT_OF_RANGE"` | Device outside near/far distance bounds |
| `IdentificationEvent.DEVICE_FOUND` | `"DEVICE_FOUND"` | New QR added to tracker |
| `IdentificationEvent.IDENTIFIED` | `"IDENTIFIED"` | LED signal verified |

---

### C.2 Server-Side FSM (`RegistrationClient`)
**File:** `registration_software/registration/registration_client.py`

#### Phase 1 — Enrollment (`verify_enroll`)

| Trigger | Action |
|---------|--------|
| `start()` | Publish `{"request": true}` to `enroll/response` (QoS 0) + UDP broadcast on port 5005; wait `1.0 s` |
| New `enroll/request` received | Assign name `device_N`; publish `{"dev_key": uuid, "common_name": "device_N"}` (QoS 1) to `enroll/response`; add to `enrolling_devices` |
| No new device for 3 consecutive 1-second ticks (`enroll_idle_threshold`) | Exit to Phase 2 |
| Checkup missing for any device | `retry("enroll")` — republish name mapping |

**Timeout:** 1 s per tick, 3 idle ticks required.

#### Phase 2 — Label Assignment (`verify_label`)

| Trigger | Action |
|---------|--------|
| Entry | For each enrolled device: generate `nonce = os.urandom(16).hex()`; publish `{"nonce": nonce, "message": "LED on"}` (QoS 1) to `enroll/{common_name}/response` |
| `enroll/{common_name}/request` with `signature` | Verify signature; on success: assign label; call `present_certificate()` → sign cert with CAManager; publish `{"label": ..., "certificate": ...}` (QoS 1) |
| No response within `label_timeout` (5 s) | `retry("label")` — republish nonce or certificate |
| `ActiveIdentificationTracker` mode | Poll `get_events()` up to 50 s for LED blink confirmation before assigning label |

**Signature verification:**
- SBC: ECDSA SECP256R1 / SHA-256 (real cryptographic check)
- Sensor module: `sig == nonce` (nonce echo — **not a real signature**)

#### Phase 3 — Verification & Shutdown (`finish`)

| Trigger | Action |
|---------|--------|
| Entry | Publish `{"verify": true}` (QoS 1) to `verification/{label}/response` for all devices (LED-on signal) |
| Manual mode | Block on `input("Press Enter...")` |
| Auto mode | `sleep(0.5)` |
| SBC present | Operator prompted per SM: assign SM labels to SBC `allowed_labels` list |
| Shutdown | Publish `{"allowed_labels": [...], "shutdown": true}` (SBC) or `{"shutdown": true}` (SM) to `verification/{label}/response` (QoS 1) |
| Complete | `registry.add_registered_device(mac, uuid, device_type, label)` → `registry.persist_to_json()` |

---

### C.3 SBC Device-Side FSM
**File:** `SBC/device/Client.py`

| State | Entry condition | Key actions |
|-------|----------------|-------------|
| **Bootstrap** | Power-on; port 1884, `common_name="new_dev"` | Subscribe `enroll/response` |
| **Announcing** | Receive `{"request": true}` | Publish ECDSA pubkey + `"device_type": "SBC"` to `enroll/request` |
| **Named** | Receive `{"dev_key": match, "common_name": "device_N"}` | Disconnect; reconnect as `device_N`; subscribe `enroll/{common_name}/response`; send checkup |
| **Signing** | Receive `{"nonce": ...}` | ECDSA-sign nonce; blink LED; publish signature + credentials |
| **Certified** | Receive `{"label": ..., "certificate": ..., "server_certificate": ...}` | Save certs; reconnect port 8883; subscribe `verification/{label}/response`; send checkup |
| **Verified** | Receive `{"verify": true}` | Turn LED on |
| **Done** | Receive `{"shutdown": true}` | LED off; `write_out()` (write `status.json` + ACL file); `finished = True` |

---

### C.4 Sensor Module Device-Side FSM
**File:** `sensor_module/microcontroller_files/lib/Client.py`

| State | Entry condition | Key actions |
|-------|----------------|-------------|
| **Bootstrap** | Power-on; port 1884, `common_name="new_dev"` | Subscribe `enroll/response` |
| **Announcing** | Receive `{"request": true}` | Publish `uuid_val` + `"device_type": "sensor_module"` to `enroll/request` (QoS 0) |
| **Named** | Receive `{"dev_key": uuid_val match, "common_name": "device_N"}` | Set `common_name`; disconnect; reconnect; subscribe `enroll/{common_name}/response`; send checkup |
| **Signing** | Receive `{"nonce": ...}` | Set `sig = nonce` (no real signing); blink LED via timer; publish nonce echo + pubkey |
| **Certified** | Receive `{"label": ..., "certificate": ...}` | Save cert; reconnect port 8883 as `"reg_dev"`; subscribe `verification/{label}/response`; send checkup |
| **Verified** | Receive `{"verify": true}` | LED on |
| **Done** | Receive `{"shutdown": true}` | LED off; `write_out()` (update `config.json`); `machine.soft_reset()` |

---

## D. Known Bugs and Limitations

### D.1 — Hardcoded Credentials (committed to repo)

| File | Value | Risk |
|------|-------|------|
| `sensor_module/.../config.json` | WiFi SSID `"DeviceRegistrationNetwork"`, password `"Device2025"` | Network credential exposure |
| `sensor_module/.../config.json` | MQTT user `"new_dev"`, password `"new123"` | Any device can register |
| `registration_software/common/config.py:78-79` | MQTT `"registration_server_user"` / `"supersecurepassword123"` | Broker auth bypass |
| `registration_software/common/config.py:45-46` | ONVIF camera `"admin"` / `"Edisonpro!"` | Camera credential exposure |
| `registration_software/common/config.py:58` | `MAIN_CAMERA_HOST = "192.168.0.101"` | Lab-specific; breaks elsewhere |
| `registration_software/common/config.py:73` | Broadcast `"192.168.0.255"` | Subnet-specific; breaks on different /24 |
| `resources/creds/` | `ca.key`, `server.key`, `client.key`, `qr_private.pem` | All private keys committed in plaintext |
| `SBC/device/config.py:27-29` | Shared bootstrap `"new_dev"` / `"new123"` | All unregistered devices share one credential |

---

### D.2 — Security Vulnerabilities

**Sensor module "signature" is a nonce echo, not a cryptographic proof**  
(`sensor_module/.../lib/Client.py:146`)
```python
sig = nonce   # real signing is commented out
```
The server accepts `sig == nonce` for sensor modules. Any node that intercepts the nonce on `enroll/{common_name}/response` can impersonate a sensor module with no knowledge of a private key.

The commented-out implementation shows the intended fix:
```python
# sig = sign_message_ed25519(nonce_bytes, self.priv_key, self.pub_key)
```
Key generation is also commented out (`Client.py:36-37`), and the `print("Generated ed25519 keypair …")` statement was left in, falsely implying a keypair was created.

**CA and all device private keys stored without encryption**  
`ca_manager.py:99`: `encryption_algorithm=serialization.NoEncryption()`. Unencrypted PEM files committed to the repository mean the CA is fully compromised.

**Broker UDP advertisement is unauthenticated**  
`SBC/device/utils.py`: any host on the LAN that sends `"Hello from MQTT Broker"` over UDP causes all devices to redirect to that address. Full man-in-the-middle is possible at zero cost.

**TLS SNI hardcoded**  
`mqtt_manager.py:98`: `"server_hostname": "server"` (comment says *"change to self.broker"* but was never changed). Affects TLS handshake correctness against any host other than `"server"`.

---

### D.3 — Crash-Inducing Logic Errors

**`registration_client.py` — `verify_all` does not exist**  
`_check_enroll` (line 131) and `_check_label` (line 139) both call `self.verify_all(...)` which is never defined. These methods are dead code with a working `check_enroll` / `check_label` pair alongside them, but `retry()` calling the underscore variants will raise `AttributeError`.

**`light.py` — `self.light_cfg` never assigned**  
Constructor takes `light_cfg` as a parameter but does not store it as `self.light_cfg`. The `read()` method references `self.light_cfg` and will raise `AttributeError` at runtime.

**`wifi.py` — `reconnect()` references bare `ssid` / `password`**  
These names are not in scope in `reconnect()`; should be `self._ssid` / `self._password`. Raises `NameError` on first reconnect attempt.

**`boot.py` — `gc.collect()` called after `from gc import collect`**  
The module imports `gc` as `from gc import collect`, then calls `gc.collect()` — `NameError` at runtime.

**`hmmd_mpy.py` — `self.motion_state` uninitialised**  
`compute_motion_state()` returns `self.motion_state` but `__init__` never sets it. First call raises `AttributeError`.

**`DFRobot_HumanDetection_mpy.py` — `asyncio.sleep_ms` not awaited**  
Lines 61, 199 call `asyncio.sleep_ms(150)` / `asyncio.sleep_ms(20)` as plain function calls inside non-`async` methods. The coroutine object is created and immediately discarded; no delay occurs.

**`main.py` — code outside `async def main()` due to dedent error**  
Lines 459–485 are dedented out of `async def main()` and execute at module import time, before `asyncio.run(main())`. Variables `CONFIG`, `WIFI`, `MQTT`, `env`, `rad`, `ldr` are `None` or undefined at that point.

**`main.py` — `pub_q` defined twice at module level** (lines 116 and 373): `maxsize=20` then `maxsize=10`. Second definition silently shadows the first.

---

### D.4 — Race Conditions

| Location | Issue |
|----------|-------|
| `DepClient.py:118–121` | `deregistering_flag` read/written from two threads (paho callback + main) without a lock |
| `DepClient.py:68–76` | `connected_labels` set shared between paho callback thread and main thread; no lock |
| `RegistrationClient` — `enrolling_devices` dict | Written in paho callback, iterated in main thread via `list(self.enrolling_devices.values())` without holding `self._lock` |
| `SBCClient.py:109,153` | `while self.connected is not False: pass` — busy-wait spin on a flag set by paho callback thread; no memory barrier |
| `SMClient.py:80` | `while self.client.connected is False: pass` — blocking spin inside MicroPython async main loop; starves other tasks |

---

### D.5 — Silent Failure / Missing Error Handling

- **`environment.py`**: all sensor read exceptions caught with bare `except Exception` and silenced — failed sensors produce `None` values published in the MQTT payload without any indication of failure.
- **`main.py:391`**: `except Exception: pass` on light sensor read — light level will be missing from payload silently.
- **`Client.py:163`** (SM): if public-key file is missing, `key_data` remains `None` and `"add_key": None` is published; server calls `.encode()` on `None` and crashes.
- **`registration_client.py:294`**: any exception during `dev_rsp_handler` silently drops the device from enrollment with only a `print()`.
- **`radar.py:165–166`**: `update_field` always returns `True`; the `return False` (unchanged value path) is unreachable dead code.
- **`registry.py` — TOCTOU on `get_labels()`**: file read happens outside the lock; concurrent writes can produce stale reads.

---

### D.6 — Deployment / Infrastructure Limitations

- **Two separate Mosquitto configurations are required simultaneously** during registration (port 1884 for bootstrap, port 8883 for mTLS post-registration) but the README only mentions one. The SBC's `base/mosquitto.conf` only configures port 8883; the registration `mosquitto/mosquitto.conf` configures neither port — TLS listeners must be added manually.
- **NTP dependency for mTLS**: `boot.py` retries NTP 5× with 1-second gaps. If NTP is unavailable, the microcontroller continues with epoch time and the TLS certificate validity check fails, preventing all subsequent MQTT connections.
- **No database persistence** on the SBC: `DepClient.py:144,217,226` contain `# persist ... to database server here` comments — sensor data is received over MQTT but never stored beyond the in-memory SBC process.
- **CA certificate backdated by 1 day** (`config.py` comment: *"Extensive validity for testing purposes"*) — left in the non-test code path.
- **Display driver dead code**: `display.py` and `tft_config.py` are present but the entire display block in `main.py` is triple-quoted out (lines 454–458).
- **`DepClient` broker-address advertisement is incompatible with SM listener**: `DepClient` sends label strings over UDP; `utils.py` `listen_for_broker_address()` expects the literal string `"Hello from MQTT Broker"`. The two sides never agree.

---

*End of audit.*
