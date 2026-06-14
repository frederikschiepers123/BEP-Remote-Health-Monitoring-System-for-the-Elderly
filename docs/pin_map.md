# Pin Map

Human-readable mirror of `components/board/board_pico2wh.h` (updated to the v1
PoC wiring, 2026-06-03). **`board_pico2wh.h` is authoritative**; this file is the
human reference. Do not hardcode GPIO numbers in firmware — always use the
`BOARD_*` constants. Pin assignments are PCB-owned; changing the header needs an
ADR (root `CLAUDE.md §19`).

---

## I²C0 — environment + air + light + OLED

All I²C peripherals share one bus (one mutex, root `CLAUDE.md §7.2`).

| GPIO | Pin | Signal | `BOARD_*` | Notes |
|---|---|---|---|---|
| 8 | 11 | I2C0_SDA | `BOARD_I2C_SDA_PIN` | pull-up to 3V3 on PCB |
| 9 | 12 | I2C0_SCL | `BOARD_I2C_SCL_PIN` | pull-up to 3V3 on PCB |

- Bus instance `i2c0` (`BOARD_I2C_INST`); frequency 400 kHz
  (`BOARD_I2C_FREQ_HZ`, override `-DBOARD_I2C_FREQ_HZ=100000U` for a slow-mode
  diagnostic build that tolerates marginal pull-ups).
- Device addresses:

  | Device | Addr | `BOARD_*` | Notes |
  |---|---|---|---|
  | BME280 (env, default-populate) | `0x76` | `BOARD_BME280_ADDR` | `0x77` if SDO high |
  | AHT21 (env, alt-populate) | `0x38` | `BOARD_AHT21_ADDR` | fixed address |
  | ENS160 (air quality) | `0x53` | `BOARD_ENS160_ADDR` | `0x52` if ADDR pulled low |
  | BH1750 (light, advanced module) | `0x23` | `BOARD_BH1750_ADDR` | `0x5C` if ADDR pulled high |
  | SH1106 OLED | `0x3C` | `BOARD_OLED_ADDR` | `0x3D` if SA0 high |

  The env footprint takes BME280 **or** AHT21; the light footprint takes BH1750
  (here) **or** the GL5516 LDR (ADC, below). Selection is via `/cfg/sensors.json`
  (root `CLAUDE.md §3.2`, ADR-0001).

---

## UART1 — mmWave radar

| GPIO | Pin | Direction | Signal | `BOARD_*` | Notes |
|---|---|---|---|---|---|
| 5 | 7 | Output | UART1_TX | `BOARD_RADAR_TX_PIN` | MCU TX → radar RX |
| 4 | 6 | Input | UART1_RX | `BOARD_RADAR_RX_PIN` | MCU RX ← radar TX |

- Instance `uart1` (`BOARD_RADAR_UART_INST`); 115200 baud (`BOARD_RADAR_BAUD`).
- The `radar_driver_t` v-table carries the **Seeed MR60BHA2** (advanced module,
  `"radar":"bha2"`) and the **24 GHz HMMD** module (generic module,
  `"radar":"hmmd"`, [ADR-0007](adr/0007-hmmd-radar-second-driver.md)); a
  **DFRobot C1001** driver is in development (a third driver behind the same
  seam). Selected at runtime via `/cfg/sensors.json` `"radar"` (root
  `CLAUDE.md §7.4`) — no rebuild to swap.
- Radar runs from its own 5 V rail (high RF draw — never power from MCU 3V3).

### Radar discrete GPIO indicators (in addition to UART data)

The UART carries the full payload; these discrete lines are useful for fast
presence-edge detection and bring-up sanity checks.

| GPIO | Pin | `BOARD_*` | Notes |
|---|---|---|---|
| 2 | 4 | `BOARD_MR60BHA2_PRESENCE_PIN` | HMMD presence line |
| 3 | 5 | `BOARD_MR60BHA2_LED_PIN` | onboard RGB-LED indicator |
| 6 | 9 | `BOARD_C1001_PRESENCE_PIN` | DFRobot C1001 (driver in development) — see note |
| 7 | 10 | `BOARD_C1001_FALL_PIN` | DFRobot C1001 fall-detection (driver in development) — see note |

> **C1001 note:** a DFRobot C1001 (24 GHz) driver is **in development** as a
> third radar behind the `radar_driver_t` v-table (alongside the MR60BHA2 and the
> HMMD module, ADR-0007). The header wires its presence/fall lines on the generic
> module footprint; as with the MR60BHA2's discrete lines, the UART carries the
> full payload — these GPIOs are for fast presence-edge detection / bring-up
> sanity. Until the driver lands and a `"radar":"c1001"` branch exists in
> `radar_select.c` (today it knows `"bha2"` and `"hmmd"`), selecting it is a no-op.

---

## ADC0 — GL5516 LDR (generic-module light variant)

Populated only on the generic module; the advanced module leaves this unpopulated
and uses the BH1750 over I²C instead.

| GPIO | Pin | Signal | `BOARD_*` | Notes |
|---|---|---|---|---|
| 26 | 31 | ADC0 | `BOARD_LDR_ADC_GPIO` | ADC input index 0 (`BOARD_LDR_ADC_INPUT`) |

- Divider: `3V3 ── LDR (GL5516) ── ADC_NODE ── 1 kΩ ── GND`, so
  `V = VCC × R_fixed / (R_LDR + R_fixed)`. Constants: `BOARD_LDR_VCC_V = 3.3`,
  `BOARD_LDR_FIXED_OHM = 1000.0` (ADR-0001).
- GP26 must not be used for digital I/O while the ADC is active.

---

## GPIO — button

v1 PoC has **one** user button (display-cycle); reset is the on-board RUN pin.

| GPIO | Pin | Signal | `BOARD_*` | Notes |
|---|---|---|---|---|
| 16 | 21 | BTN_DISPLAY | `BOARD_BTN_DISPLAY_PIN` | active-low, internal pull-up; advances the OLED page |

Software debounce: 50 ms one-shot FreeRTOS timer (`ui_input.c`).

---

## GPIO — LEDs

| GPIO | Pin | Signal | `BOARD_*` | Notes |
|---|---|---|---|---|
| 14 | 19 | LED_POWER | `BOARD_LED_POWER_PIN` | "system on" indicator |
| 15 | 20 | LED_WIFI | `BOARD_LED_WIFI_PIN` | "wifi associated" indicator |
| — | — | CYW43 onboard LED | (via `pico_cyw43_arch`) | net-state; not a bare GPIO |

---

## Generic / reserved GPIO breakouts

Exposed on the PCB, not bound to a sensor in v1 (traceability against the
schematic): `BOARD_GPIO_D0` = GP1 (pin 2), `BOARD_GPIO_D2` = GP13 (pin 17),
`BOARD_GPIO_D8` = GP10 (pin 14), `BOARD_GPIO_D9` = GP11 (pin 15),
`BOARD_GPIO_D10` = GP12 (pin 16).

---

## USB

TinyUSB owns the USB peripheral; no GPIO constants are needed (D+/D− are fixed in
silicon). In v1, USB enumerates a **single USB-serial dev console**
(`pico_stdio_usb`, root `CLAUDE.md §12`) for developer logs — it is **not** a
tablet data link.

> The header's comment still reads "CDC0 = data, CDC1 = logs" — that describes
> the **removed** dual-CDC USB-CDC transport ([ADR-0002](adr/0002-wifi-sole-transport.md)).
> v1 has no USB data path; only the single dev console. (The header is PCB-owned;
> correcting that comment is an ADR-gated change, so it is annotated here instead.)

---

## Power

| Rail | Source | Consumers |
|---|---|---|
| 5 V mains | External PSU | MCU (V-SYS pin 39), mmWave radar (isolated rail) |
| 3V3 (from MCU) | Pico 2 WH onboard LDO (OUT pin 36) | BME280/AHT21, ENS160, BH1750, OLED, LDR, button, LEDs |
| Radar 5 V | Separate mains rail | Seeed MR60BHA2 (high RF draw) |

Common ground on pin 38. The radar must not share its 5 V supply with the MCU
rail — keep them isolated to avoid RF noise coupling (root `CLAUDE.md §3.3`).
