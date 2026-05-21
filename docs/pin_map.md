# Pin Map

Human-readable mirror of `components/board/board_pico2wh.h`.

**Do not hardcode GPIO numbers in firmware source.**  Always use the `BOARD_*` constants from `board_pico2wh.h`.  This file is the human reference; the header file is authoritative.

TODO(spec): confirm final PCB layout before first hardware bring-up.  Items marked `[UNCONFIRMED]` require physical verification against the assembled board.

---

## I²C0 — BME280 + SH1106 OLED

| GPIO | Direction | Signal | Component | Notes |
|---|---|---|---|---|
| 4 | Bidirectional | I2C0_SDA | BME280, SH1106 OLED | `BOARD_I2C_SDA_PIN`; 4.7 kΩ pull-up to 3V3 on PCB |
| 5 | Bidirectional | I2C0_SCL | BME280, SH1106 OLED | `BOARD_I2C_SCL_PIN`; 4.7 kΩ pull-up to 3V3 on PCB |

- Bus frequency: 400 kHz (`BOARD_I2C_FREQ_HZ = 400000`)
- BME280 I²C address: `0x76` (SDO pin low); `0x77` if SDO high
- SH1106 OLED I²C address: `0x3C` (SA0 pin low); `0x3D` if SA0 high

---

## UART0 — mmWave Radar

| GPIO | Direction | Signal | Component | Notes |
|---|---|---|---|---|
| 0 | Output | UART0_TX | Radar (MR60BHA2 or C1001) | `BOARD_RADAR_TX_PIN` |
| 1 | Input  | UART0_RX | Radar (MR60BHA2 or C1001) | `BOARD_RADAR_RX_PIN` |

- Baud rate: 115200 (`BOARD_RADAR_BAUD`)
- Both radars use the same UART parameters; driver is selected via `/cfg/sensors.cbor`
- Radar runs from its own 5 V mains rail (high RF draw — do not power from MCU 3V3)

---

## SPI0 — IR Camera  [UNCONFIRMED]

| GPIO | Direction | Signal | Component | Notes |
|---|---|---|---|---|
| 16 | Input  | SPI0_MISO | IR camera | `BOARD_IR_MISO_PIN` — TODO(spec): confirm |
| 17 | Output | SPI0_CS   | IR camera | `BOARD_IR_CS_PIN` — active-low chip select |
| 18 | Output | SPI0_SCK  | IR camera | `BOARD_IR_SCK_PIN` |
| 19 | Output | SPI0_MOSI | IR camera | `BOARD_IR_MOSI_PIN` |

- SPI frequency: 10 MHz default (`BOARD_IR_SPI_FREQ_HZ = 10000000`); adjust per datasheet when part is confirmed
- **TODO(spec): IR camera part not yet confirmed — CLAUDE.md §16 Q1.**  Note that most thermal sensors (MLX90640, AMG8833) use I²C not SPI; the SPI assignment implies a different device type.

---

## ADC — GL5516 LDR (Light Sensor)

| GPIO | Direction | Signal | Component | Notes |
|---|---|---|---|---|
| 26 | Input (ADC) | ADC0 | GL5516 LDR | `BOARD_LDR_ADC_PIN`; ADC channel `BOARD_LDR_ADC_CHANNEL = 0` |

- Voltage divider: LDR (top) in series with 10 kΩ resistor (bottom) to GND
- ADC reads midpoint voltage; firmware converts to approximate lux
- GPIO26 must not be used for digital I/O while ADC is active

---

## GPIO — Buttons

| GPIO | Direction | Signal | Component | Notes |
|---|---|---|---|---|
| 20 | Input | BTN_A | Button A | `BOARD_BTN_A_PIN`; active-low, internal pull-up enabled by firmware |
| 21 | Input | BTN_B | Button B | `BOARD_BTN_B_PIN`; active-low, internal pull-up enabled by firmware |

- Software debounce: 50 ms one-shot FreeRTOS timer (ui_input.c)
- Both buttons advance the OLED UI page

---

## GPIO — LEDs

| GPIO | Direction | Signal | Component | Notes |
|---|---|---|---|---|
| 25 | Output | STATUS_LED | Optional external status LED | `BOARD_LED_STATUS_PIN`; 3V3 logic, add series resistor on PCB |
| — | — | CYW43 onboard LED | CYW43439 | Driven via `pico_cyw43_arch`; not a bare GPIO |

---

## USB

USB is handled entirely by TinyUSB.  No GPIO constants are needed; the USB D+/D− pins are fixed in silicon.

- **CDC0** — mTLS-encrypted MQTT byte stream (data)
- **CDC1** — log output (text, 115200-compatible, no MQTT data)

See `tusb_config.h` for TinyUSB configuration.

---

## Power

| Rail | Source | Consumers |
|---|---|---|
| 5 V mains | External PSU | MCU (VSYS), mmWave radar (isolated rail) |
| 3V3 (from MCU) | Pico 2 WH onboard LDO | BME280, SH1106 OLED, LDR, buttons, LEDs, IR camera |
| Radar 5 V | Separate mains rail | Seeed MR60BHA2 / DFRobot C1001 (high RF draw) |

The mmWave radar must not share its 5 V supply with the MCU rail; keep them isolated to avoid RF noise coupling.
