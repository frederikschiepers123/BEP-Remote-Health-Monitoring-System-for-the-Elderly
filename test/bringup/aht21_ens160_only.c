/*
 * Bring-up — AHT21 + ENS160 combined I²C diagnostic, no network.
 *
 * Continuously RE-SCANS I²C0 (GP8/GP9) once a second and, for whichever of the
 * combo-board sensors is present, initialises it and prints a reading:
 *   AHT21 (0x38)  → temperature + humidity
 *   ENS160 (0x53) → AQI + eCO2 + TVOC (+ validity, so warm-up is obvious)
 * The AHT21's temp/hum is fed into the ENS160 TEMP_IN/RH_IN compensation each
 * cycle, exactly as the real firmware does.
 *
 * The scan uses timeout-bounded reads (i2c_read_timeout_us) so a marginal or
 * stuck bus can NEVER hang the loop — wiggle wires / add pull-ups and watch the
 * addresses appear. Runs at 100 kHz (forgiving); 400 kHz needs solid pull-ups.
 *
 * Wiring (combo board → Pico 2 W):
 *   VCC → 3V3 OUT (pin 36) — NOT 5V     GND → GND (e.g. pin 38)
 *   SDA → GP8 (pin 11)                  SCL → GP9 (pin 12)
 *   AHT21 = 0x38, ENS160 = 0x53. Add 4.7 kΩ SDA→3V3 / SCL→3V3 if the module
 *   has no on-board pull-ups.
 *
 * NOTE: the ENS160 needs ~3 min after power-up before AQI/eCO2 are meaningful
 * (validity = warmup, possibly "not-operating" briefly) — that is normal.
 */
#include "FreeRTOS.h"
#include "task.h"

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include "board_pico2wh.h"
#include "aht21.h"
#include "ens160.h"
#include "env_driver.h"   /* ENV_TEMP_SELF_HEAT_OFFSET_C */
#include "err.h"

#include "hardware/i2c.h"
#include "hardware/gpio.h"

#include <stdio.h>
#include <stdbool.h>

#define DIAG_I2C_FREQ_HZ      100000u   /* forgiving; 400 kHz needs good pull-ups */
#define DIAG_SCAN_TIMEOUT_US  1200      /* per-address; bounds a stuck/marginal bus */

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName);
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    printf("[combo] STACK OVERFLOW in '%s'\n", pcTaskName ? pcTaskName : "?");
    for (;;) { tight_loop_contents(); }
}
void vApplicationMallocFailedHook(void);
void vApplicationMallocFailedHook(void)
{
    printf("[combo] malloc failed\n");
    for (;;) { tight_loop_contents(); }
}

static void i2c_bus_init(void)
{
    i2c_init(BOARD_I2C_INST, DIAG_I2C_FREQ_HZ);
    gpio_set_function(BOARD_I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(BOARD_I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(BOARD_I2C_SDA_PIN);
    gpio_pull_up(BOARD_I2C_SCL_PIN);
    printf("[combo] I²C0 init on SDA=GP%d (pin 11) SCL=GP%d (pin 12) @ %u Hz\n",
           BOARD_I2C_SDA_PIN, BOARD_I2C_SCL_PIN, (unsigned)DIAG_I2C_FREQ_HZ);
}

/* Timeout-bounded scan: prints every ACKing address; reports which of the two
 * combo sensors answered. Never blocks forever. */
static void i2c_scan(bool *aht_present, bool *ens_present)
{
    printf("[combo] I²C scan:");
    int count = 0;
    *aht_present = false;
    *ens_present = false;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        uint8_t dummy = 0;
        int rc = i2c_read_timeout_us(BOARD_I2C_INST, addr, &dummy, 1, false,
                                     DIAG_SCAN_TIMEOUT_US);
        if (rc >= 0) {
            printf(" 0x%02x", addr);
            count++;
            if (addr == BOARD_AHT21_ADDR)  *aht_present = true;
            if (addr == BOARD_ENS160_ADDR) *ens_present = true;
        }
    }
    printf("  (%d device%s)\n", count, count == 1 ? "" : "s");
}

static const char *ens160_validity_str(uint8_t status)
{
    switch (ens160_validity(status)) {
        case ENS160_VALIDITY_NORMAL:          return "normal";
        case ENS160_VALIDITY_WARMUP:           return "warmup(~3min)";
        case ENS160_VALIDITY_INITIAL_STARTUP:  return "startup(~1h)";
        default:                               return "invalid";
    }
}

static void combo_task(void *arg)
{
    (void)arg;

    while (!stdio_usb_connected()) vTaskDelay(pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(500));

    printf("\n[combo] AHT21 + ENS160 I²C diagnostic — no network.\n"
           "[combo] Wiring: VCC=3V3(pin36) GND SDA=GP8(pin11) SCL=GP9(pin12); "
           "AHT21=0x38 ENS160=0x53.\n");

    i2c_bus_init();

    Aht21  aht;  bool aht_ok = false;
    Ens160 ens;  bool ens_ok = false;
    float  last_t = 25.0f, last_h = 50.0f; bool have_th = false;

    for (uint32_t n = 0; ; n++) {
        bool aht_present, ens_present;
        i2c_scan(&aht_present, &ens_present);

        /* ── AHT21 ──────────────────────────────────────────────────────── */
        if (!aht_present) {
            aht_ok = false;
            printf("[combo] AHT21  0x38 absent\n");
        } else {
            if (!aht_ok) {
                err_t e = aht21_init(&aht, BOARD_I2C_INST, BOARD_AHT21_ADDR);
                if (e == ERR_OK) { aht_ok = true; printf("[combo] AHT21  init OK\n"); }
                else printf("[combo] AHT21  0x38 ACKs but init FAILED rc=%d\n", (int)e);
            }
            if (aht_ok) {
                Aht21Sample s;
                err_t e = aht21_read_sample(&aht, &s);
                if (e == ERR_OK) {
                    /* Apply the same fixed self-heat offset the real firmware
                     * uses (env_driver vtable) so this matches the mirror; the
                     * ENS160 compensation gets the corrected temp too, as in
                     * production (env_task pushes the offset-corrected sample). */
                    float t_corr = s.temp_c - ENV_TEMP_SELF_HEAT_OFFSET_C;
                    last_t = t_corr; last_h = s.humidity_pct; have_th = true;
                    printf("[combo] AHT21  T=%6.2f °C (raw %.2f, -%.1f self-heat)  H=%5.2f %%\n",
                           (double)t_corr, (double)s.temp_c,
                           (double)ENV_TEMP_SELF_HEAT_OFFSET_C, (double)s.humidity_pct);
                } else { printf("[combo] AHT21  read FAILED rc=%d\n", (int)e); aht_ok = false; }
            }
        }

        /* ── ENS160 (compensated by the AHT21 temp/hum when available) ────── */
        if (!ens_present) {
            ens_ok = false;
            printf("[combo] ENS160 0x53 absent\n");
        } else {
            if (!ens_ok) {
                err_t e = ens160_init(&ens, BOARD_I2C_INST, BOARD_ENS160_ADDR);
                if (e == ERR_OK) { ens_ok = true; printf("[combo] ENS160 init OK\n"); }
                else printf("[combo] ENS160 0x53 ACKs but init FAILED rc=%d (part-ID?)\n", (int)e);
            }
            if (ens_ok) {
                if (have_th) ens160_set_compensation(&ens, last_t, last_h);
                Ens160Sample s;
                err_t e = ens160_read_sample(&ens, &s);
                if (e == ERR_OK) {
                    printf("[combo] ENS160 AQI=%u  eCO2=%u ppm  TVOC=%u ppb  "
                           "status=0x%02x %s%s\n",
                           s.aqi, s.co2_ppm, s.tvoc_ppb, s.status,
                           ens160_validity_str(s.status),
                           ens160_is_operating(s.status) ? "" : " [not-operating]");
                } else { printf("[combo] ENS160 read FAILED rc=%d\n", (int)e); ens_ok = false; }
            }
        }

        printf("\n");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

int main(void)
{
    stdio_init_all();
    (void)cyw43_arch_init();
    xTaskCreate(combo_task, "combo", 3072, NULL, 2, NULL);
    vTaskStartScheduler();
    for (;;) { tight_loop_contents(); }
}
