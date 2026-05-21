#ifndef SENSOR_LIGHT_H
#define SENSOR_LIGHT_H

/* Light sensor task — GL5516 LDR via ADC.
 *
 * Publishes LightSample to q_light at 1 Hz.  The transport_task drains the
 * queue and forwards via MQTT.
 *
 * ADC pin / channel come from board_pico2wh.h.
 * Voltage divider: LDR in series with 10 kΩ pull-down to GND; ADC reads
 * the midpoint (LDR connects to Vcc = 3.3 V).
 *
 * Lux approximation:
 *   adc_raw  ∈ [0, 4095] for 12-bit ADC
 *   v_adc    = adc_raw * 3.3 / 4096   (volts)
 *   r_ldr    = R_REF * v_adc / (3.3 - v_adc)   where R_REF = 10 000 Ω
 *   lux      = 10.0 * (10000.0 / r_ldr)
 */

#include "err.h"

#include "FreeRTOS.h"
#include "queue.h"

#include <stdint.h>

/* ── Sample type ─────────────────────────────────────────────────────────── */

typedef struct {
    uint64_t ts_us;     /* monotonic microseconds since boot */
    uint32_t seq;       /* per-topic monotonic counter */
    float    lux;       /* approximate illuminance */
    uint8_t  q;         /* 0=ok, 3=invalid */
} LightSample;

/* ── Shared queue ────────────────────────────────────────────────────────── */

extern QueueHandle_t q_light;

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * Initialise the light component.  Creates q_light.
 * Must be called before the scheduler starts.
 */
err_t light_task_init(void);

/** FreeRTOS task entry point.  Never returns. */
void light_task(void *arg);

#endif /* SENSOR_LIGHT_H */
