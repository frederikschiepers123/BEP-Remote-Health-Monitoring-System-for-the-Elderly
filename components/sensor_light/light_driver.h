#ifndef LIGHT_DRIVER_H
#define LIGHT_DRIVER_H

/* Abstract ambient-light sensor interface.
 *
 * Two implementations live behind this vtable, mapping to the two product
 * variants of the sensor module (see ADR-0001):
 *   - BH1750 (digital, I²C0)  — advanced module, sensor on the MR60BHA2
 *     breakout. Reports calibrated lux directly.
 *   - GL5516 (analog LDR, ADC0/GPIO26 + 1 kΩ voltage divider to GND) —
 *     generic module. Voltage → resistance → lux via a power-law fit.
 *
 * Selection is by /cfg/sensors.json's "light" field
 * ("bh1750" | "gl5516"), defaulting to bh1750 (the demoed variant).
 *
 * Both drivers fill the same unified LightSample; the MQTT payload
 * (§9.2.2) doesn't carry which sensor produced the reading.
 */

#include "err.h"
#include <stdint.h>

typedef struct {
    float lux;
} LightSample;

typedef struct {
    /** Initialise the underlying hardware (I²C session, ADC channel, …). */
    err_t (*init)(void *ctx);

    /** Read the most recent measurement and fill `out`. Self-contained. */
    err_t (*read_sample)(void *ctx, LightSample *out);

    /** Human-readable name for logging: "BH1750" or "GL5516". */
    const char *name;

    /** Opaque context pointer (driver-private state struct). */
    void *ctx;
} light_driver_t;

/* Driver constructors — each returns a pointer to a static light_driver_t. */
light_driver_t *light_bh1750_driver(void);
light_driver_t *light_gl5516_driver(void);

/* Read /cfg/sensors.json's "light" key and return the matching driver.
 * Defaults to BH1750 if the field is absent (advanced module is what's
 * actually demoed). Returns NULL only if the value is something the
 * firmware doesn't know about, so callers can fail loudly. */
light_driver_t *light_select_from_config(void);

#endif /* LIGHT_DRIVER_H */
