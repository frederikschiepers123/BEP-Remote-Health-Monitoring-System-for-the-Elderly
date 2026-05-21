#define LOG_TAG "APP_MAIN"
#include "log.h"

#include "app_config.h"

/* Component headers */
#include "board_pico2wh.h"
#include "storage.h"
#include "identity.h"
#include "sensor_env.h"
#include "radar_driver.h"
#include "sensor_light.h"
#include "ui_oled.h"
#include "ui_input.h"
#include "transport_selector.h"

/* External task entry points — declared in their respective component headers,
 * but we need to create them here. */

/* FreeRTOS */
#include "FreeRTOS.h"
#include "task.h"

/* pico-sdk */
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/spi.h"
#include "hardware/uart.h"
#include "hardware/adc.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ── Global identity (read by ui_oled.c) ─────────────────────────────────── */

Identity g_identity;

/* ── Watchdog heartbeat table ────────────────────────────────────────────── */

/*
 * Each task calls wdt_task_alive(task_id) once per loop iteration.
 * app_main checks the table at 1 Hz and panics if any task misses two
 * consecutive intervals.
 *
 * The table stores a 2-bit miss counter per task (reset to 0 on heartbeat,
 * incremented on each app_main poll).  Reaching 2 triggers a panic.
 *
 * No mutex: each task writes its own slot (single-word store is atomic on
 * ARMv8-M); app_main reads all slots from a single core.  The worst case
 * is a missed write being counted as a miss — safe because 2 misses = 2 s
 * timeout, well above any realistic scheduling jitter.
 */
static volatile uint8_t s_wdt_miss[WDT_TASK_COUNT];

void wdt_task_alive(WdtTaskId id)
{
    if (id < WDT_TASK_COUNT) {
        s_wdt_miss[id] = 0;
    }
}

/* ── Button callback (invoked from timer daemon task) ────────────────────── */

static void button_handler(Button btn, void *user)
{
    (void)user;
    if (btn == BTN_A || btn == BTN_B) {
        ui_oled_next_page();
    }
}

/* ── app_main task ───────────────────────────────────────────────────────── */

static void app_main_task(void *arg)
{
    (void)arg;

    /* Initialise button input now that the scheduler is running. */
    err_t err = ui_input_init(button_handler, NULL);
    if (err != ERR_OK) {
        LOG_W("Button input init failed: %d", err);
        /* Non-fatal — buttons are a convenience, not safety-critical */
    }

    LOG_I("app_main_task running");

    for (;;) {
        wdt_task_alive(WDT_TASK_APP_MAIN);

        /* Check watchdog table */
        for (int i = 0; i < (int)WDT_TASK_COUNT; i++) {
            if (i == (int)WDT_TASK_APP_MAIN) { continue; }

            s_wdt_miss[i]++;
            if (s_wdt_miss[i] >= 2) {
                LOG_E("Task %d missed 2 consecutive watchdog intervals — PANIC",
                      i);
                /*
                 * Halt into a known reset path.  We cannot call panic() here
                 * without including the panic module, so we assert a hard fault
                 * via an invalid memory write that the Cortex-M33 will escalate.
                 * In production this should invoke the hardware watchdog.
                 */
                volatile uint32_t *invalid = (volatile uint32_t *)0xDEADBEEFU;
                *invalid = (uint32_t)i;
                /* NOT REACHED */
            }
        }

        /* Log stack high-water marks once per minute (~60 iterations). */
        static uint32_t tick = 0;
        tick++;
        if (tick % 60 == 0) {
            LOG_D("Stack HWM check at tick %lu", (unsigned long)tick);
            /* Individual task HWM logging would go here; tasks are not
             * stored in a table yet — add handles as tasks are registered. */
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ── Board hardware init (called before scheduler) ───────────────────────── */

static void board_hw_init(void)
{
    stdio_init_all();   /* early init only; TinyUSB takes over CDC later */

    /* I²C0: BME280 + SH1106 OLED */
    i2c_init(BOARD_I2C_INST, BOARD_I2C_FREQ_HZ);
    gpio_set_function(BOARD_I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(BOARD_I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(BOARD_I2C_SDA_PIN);
    gpio_pull_up(BOARD_I2C_SCL_PIN);

    /*
     * UART0 for radar: initialised by the radar driver (radar_bha2.c /
     * radar_c1001.c) because the driver owns the UART configuration.
     * We do NOT call uart_init here to avoid double-init.
     */

    /*
     * SPI0 for IR camera: TODO(spec) — driver initialises SPI when the
     * part is confirmed (CLAUDE.md §16 Q1).  Not initialised here.
     */

    /* ADC for LDR: initialised by sensor_light.c */

    LOG_I("Board hardware init complete");
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    board_hw_init();

    /* Mount filesystem — required before identity_load and config reads */
    err_t err = storage_mount();
    if (err != ERR_OK) {
        /* Cannot recover without flash; spin with LED blink */
        while (true) {
            /* Intentional halt — no LOG available before storage */
            tight_loop_contents();
        }
    }

    /* Load device identity (cert chain + UUID) */
    err = identity_load(&g_identity);
    if (err != ERR_OK) {
        LOG_E("identity_load failed: %d — device not provisioned?", err);
        /* Allow boot to continue for development/bring-up; in production
         * this should halt and light an error LED. */
    } else {
        LOG_I("Device UUID: %s", g_identity.uuid);
    }

    /* Initialise component queues (must happen before tasks start) */
    err = env_task_init();
    if (err != ERR_OK) { LOG_E("env_task_init: %d", err); }

    err = radar_task_init();
    if (err != ERR_OK) { LOG_E("radar_task_init: %d", err); }

    err = light_task_init();
    if (err != ERR_OK) { LOG_E("light_task_init: %d", err); }

    err = ui_oled_init();
    if (err != ERR_OK) { LOG_E("ui_oled_init: %d", err); }

    /* Initialise watchdog table to zero (all alive) */
    for (int i = 0; i < (int)WDT_TASK_COUNT; i++) {
        s_wdt_miss[i] = 0;
    }

    /* ── Create FreeRTOS tasks ─────────────────────────────────────────────
     *
     * Core affinity per CLAUDE.md §7.1:
     *   Core 0: app_main, ui_task, transport_task, selector_task
     *   Core 1: radar_task
     *   Any:    env_task, light_task
     *
     * xTaskCreateAffinitySet: affinity mask bit 0 = Core 0, bit 1 = Core 1.
     * 0x3 = both cores (any), 0x1 = Core 0 only, 0x2 = Core 1 only.
     */
    TaskHandle_t h;

    xTaskCreateAffinitySet(app_main_task, "app_main",
                           TASK_STACK_APP_MAIN, NULL,
                           TASK_PRI_APP_MAIN,
                           0x1U,   /* Core 0 */
                           &h);

    xTaskCreateAffinitySet(env_task, "env",
                           TASK_STACK_ENV, NULL,
                           TASK_PRI_ENV,
                           0x3U,   /* Any */
                           &h);

    xTaskCreateAffinitySet(radar_task, "radar",
                           TASK_STACK_RADAR, NULL,
                           TASK_PRI_RADAR,
                           0x2U,   /* Core 1 */
                           &h);

    xTaskCreateAffinitySet(light_task, "light",
                           TASK_STACK_LIGHT, NULL,
                           TASK_PRI_LIGHT,
                           0x3U,   /* Any */
                           &h);

    xTaskCreateAffinitySet(ui_task, "ui",
                           TASK_STACK_UI, NULL,
                           TASK_PRI_UI,
                           0x1U,   /* Core 0 */
                           &h);

    /*
     * Transport selector: initialise, then create selector_task.
     * Must be last — it needs all queues and identity loaded.
     */
    err = transport_selector_init(&g_identity);
    if (err != ERR_OK) {
        LOG_E("transport_selector_init: %d", err);
    }

    xTaskCreateAffinitySet(transport_selector_task, "selector",
                           TASK_STACK_SELECTOR, NULL,
                           TASK_PRI_SELECTOR,
                           0x1U,   /* Core 0 */
                           &h);

    /* Start the scheduler — does not return */
    LOG_I("Starting FreeRTOS scheduler");
    vTaskStartScheduler();

    /* NOT REACHED */
    while (true) {
        tight_loop_contents();
    }
    return 0;
}
