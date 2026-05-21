#ifndef BOARD_PICO2WH_H
#define BOARD_PICO2WH_H

/* Authoritative pin map for the Pico 2 WH sensor-module PCB.
 * PCB layout owns this file in spirit — do not hardcode GPIO numbers elsewhere.
 *
 * TODO(spec): confirm final PCB layout before first hardware bring-up.
 *             Mark resolved by stripping this comment and the TODO(spec) tags.
 */

#include "hardware/i2c.h"
#include "hardware/spi.h"
#include "hardware/uart.h"

/* ── I²C0 — BME280 + OLED (SH1106) ──────────────────────────────────────── */
#define BOARD_I2C_INST          i2c0
#define BOARD_I2C_SDA_PIN       4
#define BOARD_I2C_SCL_PIN       5
#define BOARD_I2C_FREQ_HZ       400000U

#define BOARD_BME280_ADDR       0x76U   /* SDO low; 0x77 if SDO high */
#define BOARD_OLED_ADDR         0x3CU   /* SA0 low; 0x3D if SA0 high */

/* ── UART0 — mmWave radar (A: Seeed MR60BHA2 or B: DFRobot C1001) ────────── */
#define BOARD_RADAR_UART_INST   uart0
#define BOARD_RADAR_TX_PIN      0
#define BOARD_RADAR_RX_PIN      1
#define BOARD_RADAR_BAUD        115200U

/* ── SPI0 — IR camera (TBD part — see CLAUDE.md §16 open question 1) ──────── */
/* TODO(spec): confirm IR camera part and bus before bring-up step 12. */
#define BOARD_IR_SPI_INST       spi0
#define BOARD_IR_SCK_PIN        18
#define BOARD_IR_MOSI_PIN       19
#define BOARD_IR_MISO_PIN       16
#define BOARD_IR_CS_PIN         17
#define BOARD_IR_SPI_FREQ_HZ    10000000U   /* 10 MHz — adjust per datasheet */

/* ── ADC — GL5516 LDR ────────────────────────────────────────────────────── */
#define BOARD_LDR_ADC_PIN       26   /* GPIO26 = ADC0 */
#define BOARD_LDR_ADC_CHANNEL   0

/* ── GPIO — buttons ──────────────────────────────────────────────────────── */
#define BOARD_BTN_A_PIN         20   /* active-low, internal pull-up */
#define BOARD_BTN_B_PIN         21

/* ── GPIO — LEDs ──────────────────────────────────────────────────────────── */
/* CYW43 onboard LED is driven via pico_cyw43_arch, not a bare GPIO. */
#define BOARD_LED_STATUS_PIN    25   /* optional external status LED */

/* ── USB ──────────────────────────────────────────────────────────────────── */
/* TinyUSB owns the USB peripheral.  No GPIO pin constants needed here;
 * CDC0 = data, CDC1 = logs (see CLAUDE.md §8.1). */

#endif /* BOARD_PICO2WH_H */
