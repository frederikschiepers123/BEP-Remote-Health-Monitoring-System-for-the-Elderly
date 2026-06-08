/*
 * Bring-up — BME280 driver verification, no network.
 *
 * Inits I²C0 on GP8/GP9, scans the bus, initialises the BME280, then prints
 * temp/humidity/pressure every second to the USB-serial console. No Wi-Fi,
 * no MQTT — fastest path to "does the sensor work".
 */
#include "FreeRTOS.h"
#include "task.h"

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include "board_pico2wh.h"
#include "bme280.h"
#include "err.h"

#include "hardware/i2c.h"
#include "hardware/gpio.h"

#include <stdio.h>

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName);
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    printf("[bme280] STACK OVERFLOW in '%s'\n", pcTaskName ? pcTaskName : "?");
    for (;;) { tight_loop_contents(); }
}
void vApplicationMallocFailedHook(void);
void vApplicationMallocFailedHook(void)
{
    printf("[bme280] malloc failed\n");
    for (;;) { tight_loop_contents(); }
}

static void i2c_bus_init(void)
{
    i2c_init(BOARD_I2C_INST, BOARD_I2C_FREQ_HZ);
    gpio_set_function(BOARD_I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(BOARD_I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(BOARD_I2C_SDA_PIN);
    gpio_pull_up(BOARD_I2C_SCL_PIN);
    printf("[bme280] I²C0 init on SDA=GP%d SCL=GP%d @ %u Hz\n",
           BOARD_I2C_SDA_PIN, BOARD_I2C_SCL_PIN, (unsigned)BOARD_I2C_FREQ_HZ);
}

static void i2c_scan(void)
{
    printf("[bme280] I²C scan:");
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

static void bme280_task(void *arg)
{
    (void)arg;

    /* stdio_init_all() was called in main() before the scheduler started,
     * so the USB CDC has had a chance to enumerate cleanly. Just wait until
     * a host actually opens the port before spamming. */
    while (!stdio_usb_connected()) vTaskDelay(pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(500));

    printf("\n[bme280] driver bring-up — pico-sdk + bme280.c, no network\n");

    i2c_bus_init();
    i2c_scan();

    Bme280 dev;
    err_t e = bme280_init(&dev, BOARD_I2C_INST, BOARD_BME280_ADDR);
    if (e != ERR_OK) {
        for (;;) {
            printf("[bme280] init FAILED rc=%d (addr 0x%02x). "
                   "Check wiring: VCC=3V3, GND, SDA=GP8, SCL=GP9.\n",
                   (int)e, BOARD_BME280_ADDR);
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }
    printf("[bme280] init OK at addr 0x%02x\n", BOARD_BME280_ADDR);

    for (uint32_t n = 0; ; n++) {
        Bme280Sample s;
        e = bme280_read_sample(&dev, &s);
        if (e != ERR_OK) {
            printf("[bme280] sample %u FAILED rc=%d\n", (unsigned)n, (int)e);
        } else {
            printf("[bme280] %4u  T=%6.2f °C   H=%5.2f %%   P=%7.2f hPa\n",
                   (unsigned)n,
                   (double)s.temp_c,
                   (double)s.humidity_pct,
                   (double)s.pressure_hpa);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

int main(void)
{
    /* Init order that consistently enumerates a CDC port on this laptop /
     * Windows combo: stdio first, then cyw43, then create the task and start
     * the scheduler. Putting stdio_init_all() inside the task (i.e. after the
     * scheduler) raced with Windows' USB enumeration window and intermittently
     * failed to enumerate at all. */
    stdio_init_all();
    (void)cyw43_arch_init();
    xTaskCreate(bme280_task, "bme280", 2048, NULL, 2, NULL);
    vTaskStartScheduler();
    for (;;) { tight_loop_contents(); }
}
