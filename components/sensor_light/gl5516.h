#ifndef GL5516_H
#define GL5516_H

/* GL5516 cadmium-sulfide photoresistor read via ADC + voltage divider.
 * Used on the generic sensor-module variant (see ADR-0001); the advanced
 * module uses the BH1750 instead and leaves this pin unpopulated.
 *
 * Circuit (per the schematic):
 *   3.3V ── LDR ── ADC_NODE ── R_fixed ── GND
 *                  └─ GPIO26 / ADC0
 *
 * Math:
 *   V_adc   = ADC_counts × VCC / 4095        (12-bit conversion)
 *   R_LDR   = R_fixed × (VCC / V_adc - 1)
 *   lux     = (LDR_A / R_LDR)^(1 / LDR_B)
 *
 * Starting constants per the Arduino reference: A = 50000, B = 0.7.
 * These need per-board calibration against the BH1750 on an advanced
 * module before they're trustworthy — tracked separately, not here. */

#include "err.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    float lux;
    float resistance_ohm;   /* derived; useful for calibration logs */
    float voltage_v;        /* derived; useful for sanity checks    */
} Gl5516Sample;

typedef struct {
    uint8_t adc_input;      /* hardware_adc input index, e.g. 0 = ADC0 */
    uint8_t adc_gpio;       /* GPIO number to put into ADC mode        */
    float   vcc_v;
    float   r_fixed_ohm;
    float   ldr_a;
    float   ldr_b;
    bool    initialised;
} Gl5516;

err_t gl5516_init(Gl5516 *dev, uint8_t adc_input, uint8_t adc_gpio,
                  float vcc_v, float r_fixed_ohm);
err_t gl5516_read_sample(Gl5516 *dev, Gl5516Sample *out);

#endif /* GL5516_H */
