#ifndef BOARD_PICO2WH_H
#define BOARD_PICO2WH_H

/* Authoritative pin map for the Pico 2 WH sensor-module PCB.
 * PCB layout owns this file in spirit — do not hardcode GPIO numbers elsewhere.
 *
 * Updated 2026-06-03 to match the hardware actually wired for the v1 PoC.
 * The previous placeholder values are gone; if anything here is wrong, fix
 * here, not at the call site.
 */

#include "hardware/i2c.h"
#include "hardware/uart.h"

/* ── I²C0 — BME280 + ENS160 + SH1106 OLED ───────────────────────────────── */
#define BOARD_I2C_INST          i2c0
#define BOARD_I2C_SDA_PIN       8     /* GP8, pin 11 */
#define BOARD_I2C_SCL_PIN       9     /* GP9, pin 12 */
/* I²C bus speed. Default 400 kHz fast-mode; can be overridden at build time
 * with -DBOARD_I2C_FREQ_HZ=100000U for a diagnostic slow-mode build that
 * tolerates marginal pull-ups (e.g. after removing a sensor breakout that
 * carried onboard pull-up resistors). */
#ifndef BOARD_I2C_FREQ_HZ
#define BOARD_I2C_FREQ_HZ       400000U
#endif

#define BOARD_BME280_ADDR       0x76U   /* SDO low; 0x77 if SDO high */
#define BOARD_AHT21_ADDR        0x38U   /* AHT21 has a fixed I²C address */
#define BOARD_ENS160_ADDR       0x53U   /* default; 0x52 if ADDR pin pulled low */
#define BOARD_OLED_ADDR         0x3CU   /* SA0 low; 0x3D if SA0 high */
#define BOARD_BH1750_ADDR       0x23U   /* default; 0x5C if ADDR pin pulled high */

/* ── UART1 — mmWave radar (shared UART for MR60BHA2 OR C1001; selection
 *           via /cfg/sensors.json per §3.2 / §7.4) ─────────────────────── */
/* GP4/GP5 are UART1 on RP2350. The hardware perspective: MCU TX = GP5,
 * MCU RX = GP4. */
#define BOARD_RADAR_UART_INST   uart1
#define BOARD_RADAR_TX_PIN      5     /* GP5, pin 7 — MCU TX → radar RX */
#define BOARD_RADAR_RX_PIN      4     /* GP4, pin 6 — MCU RX ← radar TX */
#define BOARD_RADAR_BAUD        115200U

/* ── Radar — discrete GPIO indicators (in addition to UART data) ─────────── */
/* MR60BHA2 (HMMD) exposes a presence signal on a discrete GPIO line and an
 * RGB-LED indicator pin. The C1001 exposes presence + fall-detection on
 * two separate GPIO lines. None of these are strictly needed for sensor
 * sampling (the UART carries the full payload) but they're useful for
 * fast presence-edge detection and as bring-up sanity checks. */
#define BOARD_MR60BHA2_PRESENCE_PIN  2   /* GP2, pin 4  — HMMD presence */
#define BOARD_MR60BHA2_LED_PIN       3   /* GP3, pin 5  — onboard RGB LED */
#define BOARD_C1001_PRESENCE_PIN     6   /* GP6, pin 9  */
#define BOARD_C1001_FALL_PIN         7   /* GP7, pin 10 — fall detection */

/* ── GPIO — buttons ──────────────────────────────────────────────────────── */
/* v1 PoC: one user button (display-cycle), plus the on-board RUN pin for reset. */
#define BOARD_BTN_DISPLAY_PIN   16    /* GP16, pin 21 — active-low, internal pull-up */

/* ── GPIO — LEDs ──────────────────────────────────────────────────────────── */
/* CYW43 onboard LED is driven via pico_cyw43_arch (not a bare GPIO). */
#define BOARD_LED_POWER_PIN     14    /* GP14, pin 19 — "system on" indicator */
#define BOARD_LED_WIFI_PIN      15    /* GP15, pin 20 — "wifi associated" indicator */

/* ── ADC0 — GL5516 LDR (generic module variant only; advanced module leaves
 *           this pin unpopulated, BH1750 over I²C handles light there) ──── */
/* GP26 = ADC channel 0 on RP2350. Wired as:
 *   3V3 ── R6 (LDR, GL5516) ── ADC_NODE ── R7 (1 kΩ to GND) ── GND
 *                              │
 *                              └─ GP26 / ADC0 / pin 31
 * So the ADC sees:  V = VCC × R_fixed / (R_LDR + R_fixed). See ADR-0001. */
#define BOARD_LDR_ADC_INPUT     0     /* hardware_adc input index (= ADC0)  */
#define BOARD_LDR_ADC_GPIO      26    /* GP26, pin 31                       */
#define BOARD_LDR_VCC_V         3.3f
#define BOARD_LDR_FIXED_OHM     1000.0f  /* R7 in the divider                */

/* ── Generic / reserved GPIO labels ──────────────────────────────────────── */
/* The PCB exposes D0, D2, D8, D9, D10 as generic breakouts; not bound to a
 * sensor in v1. Listed here for traceability against the schematic. */
#define BOARD_GPIO_D0           1     /* pin 2  */
#define BOARD_GPIO_D2           13    /* pin 17 */
#define BOARD_GPIO_D8           10    /* pin 14 */
#define BOARD_GPIO_D9           11    /* pin 15 */
#define BOARD_GPIO_D10          12    /* pin 16 */

/* ── USB ──────────────────────────────────────────────────────────────────── */
/* TinyUSB owns the USB peripheral. No GPIO pin constants needed here;
 * CDC0 = data, CDC1 = logs (see CLAUDE.md §8.1). */

/* ── Power note ──────────────────────────────────────────────────────────── */
/* All sensor modules except the mmWave radars are powered from the Pico's
 * 3V3 (OUT) pin 36. Radars run from 5 V on V-SYS pin 39 (high RF draw,
 * isolated rail per CLAUDE.md §3.3). Common ground on pin 38. */

#endif /* BOARD_PICO2WH_H */
