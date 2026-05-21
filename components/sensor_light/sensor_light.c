#define LOG_TAG "LIGHT"
#include "log.h"

#include "sensor_light.h"
#include "board_pico2wh.h"
#include "app_config.h"
#include "err.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "hardware/adc.h"
#include "hardware/gpio.h"
#include "pico/time.h"

#include <stdint.h>

/* ── Watchdog forward declaration ────────────────────────────────────────── */

extern void wdt_task_alive(WdtTaskId id);

/* ── Module-level state ──────────────────────────────────────────────────── */

QueueHandle_t q_light = NULL;

/* ── Constants for lux conversion ───────────────────────────────────────── */

/* GL5516 voltage divider: R_REF = 10 kΩ to GND, LDR to Vcc */
#define R_REF_OHM   10000.0f
#define VCC         3.3f
#define ADC_FULL    4096.0f   /* 12-bit ADC */

/* ── light_task_init ─────────────────────────────────────────────────────── */

err_t light_task_init(void)
{
    q_light = xQueueCreate(Q_LIGHT_DEPTH, sizeof(LightSample));
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

    /* ADC init using board constants */
    adc_init();
    adc_gpio_init(BOARD_LDR_ADC_PIN);
    adc_select_input(BOARD_LDR_ADC_CHANNEL);

    uint32_t seq     = 0;
    uint32_t dropped = 0;

    for (;;) {
        wdt_task_alive(WDT_TASK_LIGHT);

        LightSample sample;
        sample.seq   = seq++;
        sample.ts_us = (uint64_t)to_us_since_boot(get_absolute_time());
        sample.q     = 0;

        uint16_t adc_raw = adc_read();

        /*
         * Convert ADC reading to lux.
         *
         * v_adc = adc_raw * VCC / ADC_FULL
         * If v_adc is 0 (LDR shorted / dark ADC stuck at 0) we would divide
         * by zero in the r_ldr formula; clamp to avoid that.
         *
         * r_ldr = R_REF * v_adc / (VCC - v_adc)
         * lux   = 10.0 * (10000.0 / r_ldr)
         *       = 10.0 * (10000.0 * (VCC - v_adc)) / (R_REF * v_adc)
         */
        float v_adc = ((float)adc_raw * VCC) / ADC_FULL;

        if (v_adc < 0.001f) {
            /* ADC essentially zero — complete darkness or ADC stuck */
            sample.lux = 0.0f;
            sample.q   = 0;
        } else if (v_adc >= VCC - 0.001f) {
            /* Saturated — LDR resistance near zero */
            sample.lux = 10000.0f;  /* arbitrary max */
            sample.q   = 2;         /* degraded — saturated */
        } else {
            float r_ldr = R_REF_OHM * v_adc / (VCC - v_adc);
            sample.lux  = 10.0f * (R_REF_OHM / r_ldr);
            sample.q    = 0;
        }

        LOG_D("Light: adc=%u v=%.3f lux=%.1f q=%u",
              adc_raw, (double)v_adc, (double)sample.lux, sample.q);

        if (xQueueSendToBack(q_light, &sample, 0) != pdTRUE) {
            dropped++;
            if (dropped % 10 == 0) {
                LOG_W("q_light full — dropped %lu samples",
                      (unsigned long)dropped);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
