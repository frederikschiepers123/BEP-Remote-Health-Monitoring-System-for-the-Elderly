#define LOG_TAG "APP_MAIN"
#include "log.h"

#include "app_config.h"

/* Component headers */
#include "board_pico2wh.h"
#include "storage.h"
#include "spool.h"
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

    /* NOTE: ui_input_init() is NOT called here.  Its button GPIO IRQ shares
     * IO_IRQ_BANK0 with cyw43's WL_HOST_WAKE (GP24) LEVEL_HIGH interrupt — the
     * one that drives the threadsafe_background poll engine.  Claiming that bank
     * before cyw43_arch_init disturbs the wake servicing and the Wi-Fi
     * association never completes.  So buttons are initialised AFTER bring-up,
     * in app_start_sensor_tasks(). */

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

            /* Spool depth + overflow drops since boot (session-scoped; ADR-0003).
             * A persistently high backlog means the broker is unreachable; a
             * rising drop count means the outage exceeded the ~15 min capacity. */
            LOG_I("spool: %u buffered, %u dropped (cap %u)",
                  (unsigned)spool_count(), (unsigned)spool_dropped_total(),
                  (unsigned)spool_capacity());
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ── Board hardware init (called before scheduler) ───────────────────────── */

static void board_hw_init(void)
{
    /* NOTE: stdio_init_all() is deliberately NOT called here.  Under the SDK's
     * pico_async_context_freertos / pico_time_adapter integration it must run
     * AFTER vTaskStartScheduler — calling it pre-scheduler breaks that time
     * integration, which (a) makes pico_stdio_usb fail to enumerate (the COM-
     * port "semaphore timeout") and (b) starves the cyw43 threadsafe_background
     * poll so the Wi-Fi association never completes.  It is therefore called at
     * the top of transport_task instead, before cyw43 init — matching the
     * proven bring-up (test/bringup/wifi_connect.c). */

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

/* ── Producer task creation (deferred until after Wi-Fi bring-up) ────────────
 *
 * Invoked once by transport_task (via the transport_mqtt_init callback) AFTER
 * the cyw43 association has finished — success or failure.  The producer tasks
 * are NOT created before the scheduler: the threadsafe_background cyw43
 * association only completes when no task that touches its async-context
 * (lwIP/cyw43) contends it.  transport_task is the only such task, so it brings
 * Wi-Fi up alone and then starts the producers here.  Runs from the transport
 * task context, scheduler already running — xTaskCreate is valid there.
 *
 * ui_task is NOT deferred — it is network-inert (only I²C/SH1122) and is
 * created pre-scheduler in main() so the OLED can render the bring-up
 * diagnostic live during association.  See app_config.h "Wi-Fi association
 * bring-up". */
static void app_start_sensor_tasks(void)
{
    TaskHandle_t h;
    LOG_I("Wi-Fi bring-up done — starting producer tasks");

    /* Buttons now — AFTER cyw43 has claimed IO_IRQ_BANK0 for its WL_HOST_WAKE
     * level interrupt.  ui_input_init() adds a GPIO IRQ on that same bank; doing
     * it before cyw43_arch_init disturbs the cyw43 poll engine and wedges Wi-Fi
     * association (the root-cause of the "assoc N/4" hang).  Registering it here
     * lets cyw43 own the bank first; the button handler coexists as a co-IRQ. */
    if (ui_input_init(button_handler, NULL) != ERR_OK) {
        LOG_W("Button input init failed");   /* non-fatal — convenience only */
    }

    /* Creation only fails on heap exhaustion (already halted by the malloc
     * hook), but log per-task so a silent absence can't hide — the watchdog
     * never arms an un-created task's slot, so this is the only signal. */
    if (xTaskCreateAffinitySet(env_task,   "env",   TASK_STACK_ENV,   NULL,
                               TASK_PRI_ENV,   0x1U, &h) != pdPASS) {
        LOG_E("env task create failed");
    }
    if (xTaskCreateAffinitySet(air_task,   "air",   TASK_STACK_AIR,   NULL,
                               TASK_PRI_AIR,   0x1U, &h) != pdPASS) {
        LOG_E("air task create failed");
    }
    if (xTaskCreateAffinitySet(radar_task, "radar", TASK_STACK_RADAR, NULL,
                               TASK_PRI_RADAR, 0x1U, &h) != pdPASS) {
        LOG_E("radar task create failed");
    }
    if (xTaskCreateAffinitySet(light_task, "light", TASK_STACK_LIGHT, NULL,
                               TASK_PRI_LIGHT, 0x1U, &h) != pdPASS) {
        LOG_E("light task create failed");
    }
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

    /* Mount the non-volatile outbound spool (ADR-0003). After storage_mount so
     * both share the flash_safe_execute path; before tasks so the transport
     * finds any backlog left by a previous power cycle. Non-fatal on failure —
     * publishing degrades to best-effort rather than halting the device. */
    err = spool_mount();
    if (err != ERR_OK) { LOG_E("spool_mount failed: %d — outage buffering disabled", err); }

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

    /* Hand identity + config to the transport task (stores references), plus
     * the callback it invokes to start the producer/UI tasks after Wi-Fi
     * bring-up (they must not contend the cyw43 association — see
     * app_start_sensor_tasks). */
    err = transport_mqtt_init(&g_identity, &g_wifi, &g_broker,
                              app_start_sensor_tasks);
    if (err != ERR_OK) { LOG_E("transport_mqtt_init: %d", err); }

    /* Initialise watchdog table: all slots un-armed until first heartbeat. */
    for (int i = 0; i < (int)WDT_TASK_COUNT; i++) {
        s_wdt_miss[i]  = 0;
        s_wdt_armed[i] = false;
    }

    /* ── Create FreeRTOS tasks (two-phase startup) ─────────────────────────
     *
     * ALL tasks are pinned to Core 0 (affinity 0x1).  pico_cyw43_arch_lwip_
     * threadsafe_background services cyw43 + lwIP from a Core-0 background poll
     * and is NOT SMP-aware: a continuously-running Core-1 task starves that
     * poll and wedges Wi-Fi (this is why the sys_freertos path was abandoned —
     * see lwipopts.h / ADR-0002). Core 1 stays idle.
     *
     * Only app_main (supervisor), transport, and the network-inert ui task are
     * created here.  The PRODUCER tasks (env/air/radar/light) are created LATER
     * by transport_task (app_start_sensor_tasks, via the transport_mqtt_init
     * callback) once the cyw43 association is done: if a task that touches the
     * cyw43 async-context (lwIP/cyw43) runs during the join handshake it
     * contends that context and the connect never completes.  transport is the
     * only such task during bring-up — reproducing the proven bring-up's
     * invariant (associate with nothing else contending the async-context).
     * ui is exempt because it only drives the OLED over I²C (no cyw43/lwIP), so
     * it runs during bring-up to show the live diagnostic.  See app_config.h
     * "Wi-Fi association bring-up".
     *
     * xTaskCreateAffinitySet: affinity mask bit 0 = Core 0, bit 1 = Core 1.
     */
    TaskHandle_t h;

    xTaskCreateAffinitySet(app_main_task, "app_main",
                           TASK_STACK_APP_MAIN, NULL,
                           TASK_PRI_APP_MAIN,
                           0x1U,   /* Core 0 */
                           &h);

    /*
     * Transport task (Wi-Fi + mTLS + MQTT): owns the network path and is the
     * sole consumer of the producer queues.  Brings Wi-Fi up as the only
     * cyw43-touching task, then starts the producer tasks itself.  Core 0,
     * highest app priority.
     */
    xTaskCreateAffinitySet(transport_task, "transport",
                           TASK_STACK_TRANSPORT, NULL,
                           TASK_PRI_TRANSPORT,
                           0x1U,   /* Core 0 */
                           &h);

    /*
     * UI task: created pre-scheduler (unlike the producers) so the OLED renders
     * the live bring-up diagnostic — "cyw43 init..", "assoc <ssid> N/4",
     * "WIFI FAIL rc=..", "MQTT up" — during the association window (up to
     * ~126 s with retries).  Safe to run during bring-up: it is network-inert
     * (only I²C/SH1122, never cyw43/lwIP), so it never contends the cyw43
     * async-context, and at prio 2 it cannot preempt transport (prio 5) anyway.
     * Its event queue was created above by ui_oled_init().
     */
    if (xTaskCreateAffinitySet(ui_task, "ui", TASK_STACK_UI, NULL,
                               TASK_PRI_UI, 0x1U, &h) != pdPASS) {
        LOG_E("ui task create failed");
    }

    /* Start the scheduler — does not return */
    LOG_I("Starting FreeRTOS scheduler");
    vTaskStartScheduler();

    /* NOT REACHED */
    while (true) {
        tight_loop_contents();
    }
    return 0;
}
