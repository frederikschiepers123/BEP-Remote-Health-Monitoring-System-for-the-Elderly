/*
 * Bring-up — ALL on-board sensors, NO network. The bench tool for verifying
 * the full sensor set without depending on Wi-Fi/MQTT (the full
 * bringup_sensors build only prints the I²C reads AFTER it connects to the
 * broker, so a flaky hotspot hides the sensors).
 *
 *   I²C task  : AHT21 (0x38) + ENS160 (0x53) + BH1750 (0x23) on I²C0 @ 100 kHz
 *   radar task: MR60BHA2 on UART1, printed ~1 Hz
 *
 * AHT21 temp shows the production −5 °C self-heat offset; ENS160 carries the
 * patient-recovery driver fix and is compensated with the AHT21 temp/hum.
 *
 * Wiring:
 *   I²C0 : SDA=GP8(pin11) SCL=GP9(pin12)  — AHT21 0x38, ENS160 0x53, BH1750 0x23
 *          (4.7 kΩ SDA/SCL→3V3 pull-ups if the breakouts don't carry them)
 *   UART1: radar TX→GP4(pin6), radar RX→GP5(pin7), radar 5V→VSYS(pin39)
 *   Common GND (pin38).
 */
#include "FreeRTOS.h"
#include "task.h"

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include "board_pico2wh.h"
#include "aht21.h"
#include "ens160.h"
#include "env_driver.h"        /* ENV_TEMP_SELF_HEAT_OFFSET_C */
#include "bh1750.h"
#include "radar_driver.h"
#include "err.h"

#include "hardware/i2c.h"
#include "hardware/gpio.h"

#include <stdio.h>
#include <stdbool.h>

#define DIAG_I2C_FREQ_HZ      100000u
#define DIAG_SCAN_TIMEOUT_US  1200

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName);
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    printf("[bench] STACK OVERFLOW in '%s'\n", pcTaskName ? pcTaskName : "?");
    for (;;) { tight_loop_contents(); }
}
void vApplicationMallocFailedHook(void);
void vApplicationMallocFailedHook(void)
{
    printf("[bench] malloc failed\n");
    for (;;) { tight_loop_contents(); }
}

static void i2c_bus_init(void)
{
    i2c_init(BOARD_I2C_INST, DIAG_I2C_FREQ_HZ);
    gpio_set_function(BOARD_I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(BOARD_I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(BOARD_I2C_SDA_PIN);
    gpio_pull_up(BOARD_I2C_SCL_PIN);
    printf("[bench] I²C0 init SDA=GP%d(pin11) SCL=GP%d(pin12) @ %u Hz\n",
           BOARD_I2C_SDA_PIN, BOARD_I2C_SCL_PIN, (unsigned)DIAG_I2C_FREQ_HZ);
}

static const char *ens160_validity_str(uint8_t status)
{
    switch ((status & 0x0CU) >> 2) {
        case 0:  return "normal";
        case 1:  return "warmup";
        case 2:  return "startup";
        default: return "invalid";
    }
}

/* ── I²C sensors task: AHT21 + ENS160 + BH1750 ───────────────────────────── */
static void i2c_sensors_task(void *arg)
{
    (void)arg;

    while (!stdio_usb_connected()) vTaskDelay(pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(500));

    printf("\n[bench] all-sensors, NO network. I²C0 @100kHz (AHT21/ENS160/BH1750), "
           "radar on UART1 GP4/GP5.\n");
    i2c_bus_init();

    Aht21  aht; bool aht_ok = false;
    Ens160 ens; bool ens_ok = false;
    Bh1750 bh;  bool bh_ok  = false;
    float  last_t = 25.0f, last_h = 50.0f; bool have_th = false;

    for (uint32_t n = 0; ; n++) {
        /* scan */
        printf("[bench] I²C scan:");
        int cnt = 0;
        for (uint8_t a = 0x08; a < 0x78; a++) {
            uint8_t d = 0;
            if (i2c_read_timeout_us(BOARD_I2C_INST, a, &d, 1, false, DIAG_SCAN_TIMEOUT_US) >= 0) {
                printf(" 0x%02x", a); cnt++;
            }
        }
        printf("  (%d)\n", cnt);

        /* AHT21 */
        if (!aht_ok && aht21_init(&aht, BOARD_I2C_INST, BOARD_AHT21_ADDR) == ERR_OK) {
            aht_ok = true; printf("[bench] AHT21  init OK\n");
        }
        if (aht_ok) {
            Aht21Sample s;
            if (aht21_read_sample(&aht, &s) == ERR_OK) {
                float tc = s.temp_c - ENV_TEMP_SELF_HEAT_OFFSET_C;
                last_t = tc; last_h = s.humidity_pct; have_th = true;
                printf("[bench] AHT21  T=%.2f °C (raw %.2f)  H=%.2f %%\n",
                       (double)tc, (double)s.temp_c, (double)s.humidity_pct);
            } else { aht_ok = false; printf("[bench] AHT21  read FAILED\n"); }
        }

        /* ENS160 (compensated by AHT21) */
        if (!ens_ok && ens160_init(&ens, BOARD_I2C_INST, BOARD_ENS160_ADDR) == ERR_OK) {
            ens_ok = true; printf("[bench] ENS160 init OK\n");
        }
        if (ens_ok) {
            if (have_th) ens160_set_compensation(&ens, last_t, last_h);
            Ens160Sample s;
            if (ens160_read_sample(&ens, &s) == ERR_OK) {
                printf("[bench] ENS160 AQI=%u eCO2=%u ppm TVOC=%u ppb status=0x%02x %s%s\n",
                       s.aqi, s.co2_ppm, s.tvoc_ppb, s.status,
                       ens160_validity_str(s.status),
                       ens160_is_operating(s.status) ? "" : " [not-operating]");
            } else { ens_ok = false; }
        }

        /* BH1750 */
        if (!bh_ok && bh1750_init(&bh, BOARD_I2C_INST, BOARD_BH1750_ADDR) == ERR_OK) {
            bh_ok = true; printf("[bench] BH1750 init OK\n");
        }
        if (bh_ok) {
            Bh1750Sample s;
            if (bh1750_read_sample(&bh, &s) == ERR_OK) {
                printf("[bench] BH1750 %.1f lux\n", (double)s.lux);
            } else { bh_ok = false; printf("[bench] BH1750 read FAILED\n"); }
        }

        printf("\n");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ── radar task: MR60BHA2 on UART1, drained continuously, printed ~1 Hz ───── */
static void bench_radar_task(void *arg)
{
    (void)arg;

    while (!stdio_usb_connected()) vTaskDelay(pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(700));

    radar_driver_t *r = radar_bha2_driver();
    err_t e = r->init(r->ctx, BOARD_RADAR_UART_INST);
    printf("[bench] radar(%s) init %s (rc=%d) — UART1 TX→pin6/GP4, RX→pin7/GP5\n",
           r->name, e == ERR_OK ? "OK" : "FAILED", (int)e);
    if (e != ERR_OK) { for (;;) vTaskDelay(pdMS_TO_TICKS(3000)); }

    TickType_t last_ok = 0, last_idle = 0;
    for (;;) {
        RadarSample s;
        err_t re = r->read_sample(r->ctx, &s, 1000);   /* drains the UART each call */
        TickType_t now = xTaskGetTickCount();
        if (re == ERR_OK) {
            if (now - last_ok >= pdMS_TO_TICKS(1000)) {  /* throttle prints to ~1 Hz */
                last_ok = now;
                printf("[bench] RADAR pres=%d dist=%lumm BR=%.1f HR=%.1f q=%u\n",
                       s.presence ? 1 : 0, (unsigned long)s.distance_mm,
                       (double)s.breath_rpm, (double)s.heart_bpm, s.q);
            }
        } else if (now - last_idle >= pdMS_TO_TICKS(3000)) {
            last_idle = now;
            printf("[bench] RADAR no frames — check radar TX→pin6(GP4), RX→pin7(GP5), "
                   "5V→pin39, common GND\n");
        }
    }
}

int main(void)
{
    stdio_init_all();
    (void)cyw43_arch_init();
    xTaskCreate(i2c_sensors_task, "i2c",   3072, NULL, 2, NULL);
    xTaskCreate(bench_radar_task, "radar", 4096, NULL, 3, NULL);
    vTaskStartScheduler();
    for (;;) { tight_loop_contents(); }
}
