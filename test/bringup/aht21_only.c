/*
 * Bring-up — AHT21-only I²C diagnostic, no network.
 *
 * Continuously RE-SCANS I²C0 (GP8/GP9) once a second and, when the AHT21
 * (0x38) appears, initialises it and prints temp/humidity. The scan uses
 * timeout-bounded reads (i2c_read_timeout_us) so a marginal or stuck bus can
 * NEVER hang the loop — you can wiggle wires / add pull-ups and watch 0x38
 * pop in and out live. Runs at 100 kHz deliberately (forgiving); 400 kHz
 * needs solid external pull-ups, which is exactly what we're isolating.
 *
 * Wiring (AHT21 → Pico 2 W):
 *   VCC → 3V3 OUT (pin 36)   — NOT 5V        GND → GND (e.g. pin 38)
 *   SDA → GP8  (pin 11)                      SCL → GP9 (pin 12)
 *   addr 0x38 (fixed). If the module has no on-board pull-ups, add
 *   4.7 kΩ from SDA→3V3 and SCL→3V3.
 */
#include "FreeRTOS.h"
#include "task.h"

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include "board_pico2wh.h"
#include "aht21.h"
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
    printf("[aht21] STACK OVERFLOW in '%s'\n", pcTaskName ? pcTaskName : "?");
    for (;;) { tight_loop_contents(); }
}
void vApplicationMallocFailedHook(void);
void vApplicationMallocFailedHook(void)
{
    printf("[aht21] malloc failed\n");
    for (;;) { tight_loop_contents(); }
}

static void i2c_bus_init(void)
{
    i2c_init(BOARD_I2C_INST, DIAG_I2C_FREQ_HZ);
    gpio_set_function(BOARD_I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(BOARD_I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(BOARD_I2C_SDA_PIN);
    gpio_pull_up(BOARD_I2C_SCL_PIN);
    printf("[aht21] I²C0 init on SDA=GP%d (pin 11) SCL=GP%d (pin 12) @ %u Hz\n",
           BOARD_I2C_SDA_PIN, BOARD_I2C_SCL_PIN, (unsigned)DIAG_I2C_FREQ_HZ);
}

/* Timeout-bounded scan: prints every ACKing address, returns true if `want`
 * answered. i2c_read_timeout_us never blocks forever, so a held-low bus shows
 * as "(0 devices)" instead of freezing the diagnostic. */
static bool i2c_scan(uint8_t want)
{
    printf("[aht21] I²C scan:");
    int count = 0;
    bool found_want = false;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        uint8_t dummy = 0;
        int rc = i2c_read_timeout_us(BOARD_I2C_INST, addr, &dummy, 1, false,
                                     DIAG_SCAN_TIMEOUT_US);
        if (rc >= 0) {
            printf(" 0x%02x", addr);
            count++;
            if (addr == want) found_want = true;
        }
    }
    printf("  (%d device%s)\n", count, count == 1 ? "" : "s");
    return found_want;
}

static void aht21_task(void *arg)
{
    (void)arg;

    while (!stdio_usb_connected()) vTaskDelay(pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(500));

    printf("\n[aht21] AHT21-only I²C diagnostic — no network.\n"
           "[aht21] Wiring: VCC=3V3(pin36), GND, SDA=GP8(pin11), SCL=GP9(pin12), addr 0x38.\n");

    i2c_bus_init();

    Aht21 dev;
    bool inited = false;

    for (uint32_t n = 0; ; n++) {
        bool present = i2c_scan(BOARD_AHT21_ADDR);

        if (!present) {
            inited = false;   /* re-init if it comes back */
            printf("[aht21] 0x38 NOT on bus — check VCC=3V3(pin36), GND, "
                   "SDA→GP8(pin11), SCL→GP9(pin12), pull-ups (4.7k→3V3).\n");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (!inited) {
            err_t e = aht21_init(&dev, BOARD_I2C_INST, BOARD_AHT21_ADDR);
            if (e != ERR_OK) {
                printf("[aht21] 0x38 ACKs but init FAILED rc=%d "
                       "(bus marginal — better pull-ups?)\n", (int)e);
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            inited = true;
            printf("[aht21] init OK at 0x%02x\n", BOARD_AHT21_ADDR);
        }

        Aht21Sample s;
        err_t e = aht21_read_sample(&dev, &s);
        if (e != ERR_OK) {
            printf("[aht21] read FAILED rc=%d\n", (int)e);
            inited = false;
        } else {
            float t_corr = s.temp_c - ENV_TEMP_SELF_HEAT_OFFSET_C;
            printf("[aht21] %4u  T=%6.2f °C (raw %.2f, -%.1f self-heat)   H=%5.2f %%\n",
                   (unsigned)n, (double)t_corr, (double)s.temp_c,
                   (double)ENV_TEMP_SELF_HEAT_OFFSET_C, (double)s.humidity_pct);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

int main(void)
{
    /* stdio first, then cyw43 (needed for the USB-stdio path), then task +
     * scheduler — same order as the other bring-up programs. */
    stdio_init_all();
    (void)cyw43_arch_init();
    xTaskCreate(aht21_task, "aht21", 2048, NULL, 2, NULL);
    vTaskStartScheduler();
    for (;;) { tight_loop_contents(); }
}
