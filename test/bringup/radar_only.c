/*
 * Bring-up — MR60BHA2 radar verification, no network.
 *
 * Two diagnostics side by side:
 *  1. Raw UART byte stream — every frame that arrives is dumped as hex.
 *     Tells us whether the wiring + UART is healthy regardless of whether
 *     our frame parser knows the command IDs.
 *  2. Driver read_sample — same path the production firmware will use.
 *     If frames arrive but parse fails, the frame command IDs in
 *     radar_bha2.c need verification (CLAUDE.md §16 Q2 — bench-pending).
 *
 * UART: uart1 on GP4 (MCU RX) / GP5 (MCU TX) @ 115200 baud (board_pico2wh.h).
 */
#include "FreeRTOS.h"
#include "task.h"

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include "board_pico2wh.h"
#include "radar_driver.h"
#include "err.h"

#include "hardware/uart.h"
#include "hardware/gpio.h"

#include <stdio.h>

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName);
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    printf("[radar] STACK OVERFLOW in '%s'\n", pcTaskName ? pcTaskName : "?");
    for (;;) { tight_loop_contents(); }
}
void vApplicationMallocFailedHook(void);
void vApplicationMallocFailedHook(void)
{
    printf("[radar] malloc failed\n");
    for (;;) { tight_loop_contents(); }
}

/* Raw-byte dump task: prints every byte the UART produces, formatted as a
 * stream of hex pairs with frame-boundary heuristics. Helps us see what the
 * MR60BHA2 actually sends, independent of the driver's parser. */
static void raw_uart_task(void *arg)
{
    (void)arg;

    /* Init UART (driver-style — duplicated here so this task can run even if
     * the driver init fails, e.g. wrong wiring). */
    uart_init(BOARD_RADAR_UART_INST, BOARD_RADAR_BAUD);
    uart_set_format(BOARD_RADAR_UART_INST, 8, 1, UART_PARITY_NONE);
    gpio_set_function(BOARD_RADAR_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(BOARD_RADAR_RX_PIN, GPIO_FUNC_UART);
    uart_set_fifo_enabled(BOARD_RADAR_UART_INST, true);

    printf("[radar] raw UART task: uart1 GP%d/GP%d @ %u baud — dumping bytes\n",
           BOARD_RADAR_RX_PIN, BOARD_RADAR_TX_PIN, (unsigned)BOARD_RADAR_BAUD);

    int col = 0;
    bool any = false;
    TickType_t last_silence_report = xTaskGetTickCount();

    for (;;) {
        if (uart_is_readable(BOARD_RADAR_UART_INST)) {
            uint8_t b = (uint8_t)uart_getc(BOARD_RADAR_UART_INST);
            any = true;
            /* Andar-family frames start with 0x53 0x59; newline before each new
             * frame makes the stream readable in PuTTY. */
            if (b == 0x53 && col != 0) {
                printf("\n");
                col = 0;
            }
            printf("%02x ", b);
            col += 3;
            if (col >= 60) { printf("\n"); col = 0; }
        } else {
            TickType_t now = xTaskGetTickCount();
            if (!any && (now - last_silence_report) > pdMS_TO_TICKS(5000)) {
                printf("[radar] no bytes from UART in 5 s. Check: 5 V "
                       "power, MCU RX=GP4 ↔ radar TX, MCU TX=GP5 ↔ radar RX, "
                       "common ground.\n");
                last_silence_report = now;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

/* Driver task: uses radar_bha2 driver, reads samples once per second. */
static void driver_task(void *arg)
{
    (void)arg;

    /* Give the host time to attach + the raw task time to start UART. */
    vTaskDelay(pdMS_TO_TICKS(2000));

    radar_driver_t *drv = radar_bha2_driver();
    if (!drv) {
        for (;;) {
            printf("[radar] radar_bha2_driver() returned NULL\n");
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
    /* The raw_uart_task already configured the UART, but the driver init is
     * idempotent enough to call again. */
    err_t e = drv->init(drv->ctx, BOARD_RADAR_UART_INST);
    if (e != ERR_OK) {
        for (;;) {
            printf("[radar] driver init FAILED rc=%d\n", (int)e);
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
    printf("[radar] driver '%s' init OK — reading samples\n", drv->name);

    for (uint32_t n = 0; ; n++) {
        RadarSample s;
        e = drv->read_sample(drv->ctx, &s, /*timeout_ms=*/2000);
        if (e == ERR_TIMEOUT) {
            printf("[radar/drv] %4u  TIMEOUT (no parseable frame in 2 s)\n",
                   (unsigned)n);
        } else if (e != ERR_OK) {
            printf("[radar/drv] %4u  rc=%d\n", (unsigned)n, (int)e);
        } else {
            printf("[radar/drv] %4u  pres=%d  dist=%lu mm  HR=%.1f bpm  "
                   "BR=%.1f rpm  q=%u\n",
                   (unsigned)n, (int)s.presence,
                   (unsigned long)s.distance_mm,
                   (double)s.heart_bpm, (double)s.breath_rpm, s.q);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

int main(void)
{
    /* Same init order as bme280_only — stdio + cyw43 before scheduler so
     * Windows CDC enumerates cleanly. */
    stdio_init_all();
    (void)cyw43_arch_init();

    /* Wait for host attach right here so the printfs below appear cleanly. */
    while (!stdio_usb_connected()) sleep_ms(100);
    sleep_ms(500);
    printf("\n[radar] MR60BHA2 bring-up — raw UART + driver, no network\n");

    /* raw task gets priority 1 (background); driver task priority 2 (fires
     * the printf cleanly between raw bursts). */
    xTaskCreate(raw_uart_task, "raw",  2048, NULL, 1, NULL);
    xTaskCreate(driver_task,  "drv",  4096, NULL, 2, NULL);
    vTaskStartScheduler();
    for (;;) { tight_loop_contents(); }
}
