/*
 * Bring-up — ENS160 driver verification, no network.
 *
 * Inits I²C0 on GP8/GP9, scans the bus, initialises the ENS160, then prints
 * AQI / TVOC / eCO2 + validity-flag interpretation every second to PuTTY.
 * No Wi-Fi, no MQTT.
 *
 * Note: ENS160 needs ~3 minutes of warmup after entering standard mode before
 * the validity flag transitions to NORMAL. During warmup the driver reports
 * "warmup" and the values are not yet meaningful.
 */
#include "FreeRTOS.h"
#include "task.h"

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include "board_pico2wh.h"
#include "ens160.h"
#include "err.h"

#include "hardware/i2c.h"
#include "hardware/gpio.h"

#include <stdio.h>

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName);
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    printf("[ens160] STACK OVERFLOW in '%s'\n", pcTaskName ? pcTaskName : "?");
    for (;;) { tight_loop_contents(); }
}
void vApplicationMallocFailedHook(void);
void vApplicationMallocFailedHook(void)
{
    printf("[ens160] malloc failed\n");
    for (;;) { tight_loop_contents(); }
}

static void i2c_bus_init(void)
{
    i2c_init(BOARD_I2C_INST, BOARD_I2C_FREQ_HZ);
    gpio_set_function(BOARD_I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(BOARD_I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(BOARD_I2C_SDA_PIN);
    gpio_pull_up(BOARD_I2C_SCL_PIN);
    printf("[ens160] I²C0 init on SDA=GP%d SCL=GP%d @ %u Hz\n",
           BOARD_I2C_SDA_PIN, BOARD_I2C_SCL_PIN, (unsigned)BOARD_I2C_FREQ_HZ);
}

static void i2c_scan(void)
{
    printf("[ens160] I²C scan:");
    int count = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        uint8_t dummy = 0;
        int rc = i2c_read_blocking(BOARD_I2C_INST, addr, &dummy, 1, false);
        if (rc >= 0) {
            printf(" 0x%02x", addr);
            count++;
        }
    }
    printf("  (%d device%s found)\n", count, count == 1 ? "" : "s");
}

static const char *validity_str(Ens160Validity v)
{
    switch (v) {
    case ENS160_VALIDITY_NORMAL:          return "normal";
    case ENS160_VALIDITY_WARMUP:          return "warmup (~3min)";
    case ENS160_VALIDITY_INITIAL_STARTUP: return "initial-startup (~1h)";
    case ENS160_VALIDITY_INVALID:         return "INVALID";
    default:                              return "?";
    }
}

static const char *aqi_str(uint8_t aqi)
{
    switch (aqi) {
    case 1: return "excellent";
    case 2: return "good";
    case 3: return "moderate";
    case 4: return "poor";
    case 5: return "unhealthy";
    default: return "?";
    }
}

static void ens160_task(void *arg)
{
    (void)arg;

    /* stdio_init_all() ran in main(); just wait for the host to attach. */
    while (!stdio_usb_connected()) vTaskDelay(pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(500));

    printf("\n[ens160] driver bring-up — pico-sdk + ens160.c, no network\n");

    i2c_bus_init();
    i2c_scan();

    Ens160 dev;
    err_t e = ens160_init(&dev, BOARD_I2C_INST, BOARD_ENS160_ADDR);
    if (e != ERR_OK) {
        for (;;) {
            printf("[ens160] init FAILED rc=%d (addr 0x%02x). "
                   "Check wiring: VCC=3V3, GND, SDA=GP8, SCL=GP9. "
                   "If ADDR pin is grounded, the chip is at 0x52.\n",
                   (int)e, BOARD_ENS160_ADDR);
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }
    printf("[ens160] init OK at addr 0x%02x\n", BOARD_ENS160_ADDR);
    printf("[ens160] heater stabilises over ~3 min — readings during warmup are not meaningful\n");

    for (uint32_t n = 0; ; n++) {
        Ens160Sample s;
        e = ens160_read_sample(&dev, &s);
        if (e != ERR_OK) {
            printf("[ens160] sample %u FAILED rc=%d\n", (unsigned)n, (int)e);
        } else {
            Ens160Validity v = ens160_validity(s.status);
            printf("[ens160] %4u  status=0x%02X (%s)  AQI=%u (%s)  TVOC=%u ppb  eCO2=%u ppm\n",
                   (unsigned)n, s.status, validity_str(v),
                   s.aqi, aqi_str(s.aqi),
                   s.tvoc_ppb, s.co2_ppm);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

int main(void)
{
    /* See bme280_only.c — stdio_init_all() before the scheduler is what makes
     * CDC enumeration reliable on this laptop / Windows. */
    stdio_init_all();
    (void)cyw43_arch_init();
    xTaskCreate(ens160_task, "ens160", 2048, NULL, 2, NULL);
    vTaskStartScheduler();
    for (;;) { tight_loop_contents(); }
}
