#define LOG_TAG "LIGHT"
#include "log.h"

#include "sensor_light.h"
#include "light_driver.h"
#include "i2c_bus.h"
#include "err.h"
#include "app_config.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include <stdint.h>

/* ── Watchdog forward declaration (defined in app_main.c) ─────────────────── */

extern void wdt_task_alive(WdtTaskId id);

/* ── Module-level state ──────────────────────────────────────────────────── */

QueueHandle_t q_light = NULL;

static light_driver_t *s_drv = NULL;

/* ── light_task_init ─────────────────────────────────────────────────────── */

err_t light_task_init(void)
{
    q_light = xQueueCreate(Q_LIGHT_DEPTH, sizeof(LightMsg));
    if (q_light == NULL) {
        LOG_E("Failed to create q_light");
        return ERR_NO_MEM;
    }
    return ERR_OK;
}

/* ── light_task ──────────────────────────────────────────────────────────── */

void light_task(void *arg)
{
    (void)arg;

    /* Select BH1750/GL5516 from /cfg/sensors.json (defaults to BH1750). */
    s_drv = light_select_from_config();
    if (s_drv == NULL) {
        LOG_E("light_select_from_config() returned NULL — light_task halting");
        vTaskSuspend(NULL);
    }

    i2c_bus_lock();
    err_t err = s_drv->init(s_drv->ctx);
    i2c_bus_unlock();
    if (err != ERR_OK) {
        LOG_E("light driver %s init failed: %d — light_task halting",
              s_drv->name, err);
        vTaskSuspend(NULL);
    }
    LOG_I("light task started, driver=%s", s_drv->name);

    uint32_t dropped = 0;

    for (;;) {
        wdt_task_alive(WDT_TASK_LIGHT);

        LightMsg msg;
        i2c_bus_lock();
        err = s_drv->read_sample(s_drv->ctx, &msg.v);
        i2c_bus_unlock();

        if (err == ERR_OK) {
            msg.q = 0;
            LOG_D("light %s %.1f lux", s_drv->name, (double)msg.v.lux);
        } else {
            LOG_W("light %s read failed: %d", s_drv->name, err);
            msg.q = 3;
            msg.v.lux = 0.0f;
        }

        if (xQueueSendToBack(q_light, &msg, 0) != pdTRUE) {
            if (++dropped % 10 == 0) {
                LOG_W("q_light full — dropped %lu samples so far",
                      (unsigned long)dropped);
            }
        }

        /* 0.2 Hz sample cadence, but heartbeat the watchdog every second —
         * the supervisor's miss window is 2 s, well under the 5 s sample gap. */
        for (int i = 0; i < 5; i++) {
            wdt_task_alive(WDT_TASK_LIGHT);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}
