#define LOG_TAG "APP_MAIN"
#include "log.h"

#include "app_config.h"

/* Component headers */
#include "board_pico2wh.h"
#include "storage.h"
#include "identity.h"
#include "cfg.h"
#include "i2c_bus.h"
#include "sensor_env.h"
#include "sensor_air.h"
#include "radar_driver.h"
#include "sensor_light.h"
#include "ui_oled.h"
#include "ui_input.h"
#include "transport_mqtt.h"

/* External task entry points — declared in their respective component headers,
 * but we need to create them here. */

/* FreeRTOS */
#include "FreeRTOS.h"
#include "task.h"

/* pico-sdk */
#include "pico/stdlib.h"
#include "hardware/gpio.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ── Global identity (read by ui_oled.c) ─────────────────────────────────── */

Identity g_identity;

/* Per-deployment config — must outlive the transport task, which keeps
 * pointers into these (Wi-Fi creds, broker address). */
static CfgWifi   g_wifi;
static CfgBroker g_broker;

/* ── FreeRTOS application hooks (required by FreeRTOSConfig.h) ────────────── */

/* configCHECK_FOR_STACK_OVERFLOW == 2: called when a task overflows its stack.
 * Unrecoverable — log a best-effort line and halt for the hardware watchdog. */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName);
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    LOG_E("STACK OVERFLOW in task '%s' — halting", pcTaskName ? pcTaskName : "?");
    for (;;) {
        tight_loop_contents();
    }
}

/* configUSE_MALLOC_FAILED_HOOK == 1: called when pvPortMalloc fails. */
void vApplicationMallocFailedHook(void);
void vApplicationMallocFailedHook(void)
{
    LOG_E("FreeRTOS heap exhausted (malloc failed) — halting");
    for (;;) {
        tight_loop_contents();
    }
}

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

/*
 * A task is "armed" only after its first heartbeat.  The supervisor ignores
 * un-armed slots, which gives a task an unbounded grace period to reach its
 * steady-state loop — essential for transport_task, whose Wi-Fi bring-up
 * (cyw43 init + up to 30 s associate) blocks far longer than the 2 s miss
 * window.  Once a task has heartbeated once it must keep doing so within 2 s.
 */
static volatile bool s_wdt_armed[WDT_TASK_COUNT];

void wdt_task_alive(WdtTaskId id)
{
    if (id < WDT_TASK_COUNT) {
        s_wdt_miss[id]  = 0;
        s_wdt_armed[id] = true;
    }
}

/* ── Button callback (invoked from timer daemon task) ────────────────────── */

static void button_handler(Button btn, void *user)
{
    (void)user;
    if (btn == BTN_DISPLAY) {
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
            if (!s_wdt_armed[i]) { continue; }   /* not yet in steady state */

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
    stdio_init_all();   /* USB-serial dev console for LOG_* (§12 v1) */

    /*
     * I²C0 (AHT21/BME280 + ENS160 + BH1750 + SH1122 OLED) is initialised by
     * i2c_bus_init() in main(), which also creates the shared bus mutex —
     * one owner of the bus configuration (§7.2).
     *
     * UART (radar) is initialised by the radar driver (it owns the config).
     * ADC (GL5516) is initialised by sensor_light.c.
     */

    /* Discrete UI GPIO: PWR LED lit at boot, Wi-Fi LED off until associated
     * (transport_task drives it).  SW2 (page cycle) is set up by ui_input. */
    gpio_init(BOARD_LED_POWER_PIN);
    gpio_set_dir(BOARD_LED_POWER_PIN, GPIO_OUT);
    gpio_put(BOARD_LED_POWER_PIN, 1);

    gpio_init(BOARD_LED_WIFI_PIN);
    gpio_set_dir(BOARD_LED_WIFI_PIN, GPIO_OUT);
    gpio_put(BOARD_LED_WIFI_PIN, 0);

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

    /* Load per-deployment config (Wi-Fi creds, broker address). */
    err = cfg_load_wifi(&g_wifi);
    if (err != ERR_OK) { LOG_E("cfg_load_wifi: %d — /cfg/wifi.json missing?", err); }
    err = cfg_load_broker(&g_broker);
    if (err != ERR_OK) { LOG_E("cfg_load_broker: %d — /cfg/broker.json missing?", err); }

    /* Initialise the shared I²C0 bus + mutex before any sensor task runs. */
    err = i2c_bus_init();
    if (err != ERR_OK) { LOG_E("i2c_bus_init: %d", err); }

    /* Initialise component queues (must happen before tasks start) */
    err = env_task_init();
    if (err != ERR_OK) { LOG_E("env_task_init: %d", err); }

    err = air_task_init();
    if (err != ERR_OK) { LOG_E("air_task_init: %d", err); }

    err = radar_task_init();
    if (err != ERR_OK) { LOG_E("radar_task_init: %d", err); }

    err = light_task_init();
    if (err != ERR_OK) { LOG_E("light_task_init: %d", err); }

    err = ui_oled_init();
    if (err != ERR_OK) { LOG_E("ui_oled_init: %d", err); }

    /* Hand identity + config to the transport task (stores references). */
    err = transport_mqtt_init(&g_identity, &g_wifi, &g_broker);
    if (err != ERR_OK) { LOG_E("transport_mqtt_init: %d", err); }

    /* Initialise watchdog table: all slots un-armed until first heartbeat. */
    for (int i = 0; i < (int)WDT_TASK_COUNT; i++) {
        s_wdt_miss[i]  = 0;
        s_wdt_armed[i] = false;
    }

    /* ── Create FreeRTOS tasks ─────────────────────────────────────────────
     *
     * Core affinity per CLAUDE.md §7.1:
     *   Core 0: app_main, ui_task, transport_task
     *   Core 1: radar_task
     *   Any:    env_task, air_task, light_task
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

    xTaskCreateAffinitySet(air_task, "air",
                           TASK_STACK_AIR, NULL,
                           TASK_PRI_AIR,
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
     * Transport task (Wi-Fi + mTLS + MQTT): owns the network path and is the
     * sole consumer of the producer queues.  Created last — it needs all
     * queues, identity, and config loaded.  Core 0, highest app priority.
     */
    xTaskCreateAffinitySet(transport_task, "transport",
                           TASK_STACK_TRANSPORT, NULL,
                           TASK_PRI_TRANSPORT,
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
