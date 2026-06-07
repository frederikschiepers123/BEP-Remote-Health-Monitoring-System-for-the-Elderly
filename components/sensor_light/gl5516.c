#define LOG_TAG "GL5516"
#ifndef HOST_TEST
#include "log.h"
#endif

#include "gl5516.h"
#include "err.h"

#ifndef HOST_TEST
#include "hardware/adc.h"
#include "hardware/gpio.h"
#endif

#include <math.h>
#include <string.h>

/* Defaults from the Arduino reference sketch. Per-board calibration
 * overrides these via a future "light_cal" object in /cfg/sensors.json
 * (not implemented yet — tracked separately). */
#define GL5516_DEFAULT_A   50000.0f
#define GL5516_DEFAULT_B   0.7f

err_t gl5516_init(Gl5516 *dev, uint8_t adc_input, uint8_t adc_gpio,
                  float vcc_v, float r_fixed_ohm)
{
    memset(dev, 0, sizeof(*dev));
    dev->adc_input    = adc_input;
    dev->adc_gpio     = adc_gpio;
    dev->vcc_v        = vcc_v;
    dev->r_fixed_ohm  = r_fixed_ohm;
    dev->ldr_a        = GL5516_DEFAULT_A;
    dev->ldr_b        = GL5516_DEFAULT_B;

#ifndef HOST_TEST
    adc_init();
    adc_gpio_init(adc_gpio);
    /* Select the channel up front; adc_select_input() before each read is
     * a no-op if the channel hasn't changed since. */
    adc_select_input(adc_input);
    LOG_I("init OK on ADC%u (GPIO%u), VCC=%.2fV, R_fixed=%.0f ohm",
          adc_input, adc_gpio, (double)vcc_v, (double)r_fixed_ohm);
#endif
    dev->initialised = true;
    return ERR_OK;
}

err_t gl5516_read_sample(Gl5516 *dev, Gl5516Sample *out)
{
    if (!dev->initialised) return ERR_NOT_INIT;

    uint16_t counts = 0;
#ifndef HOST_TEST
    adc_select_input(dev->adc_input);
    counts = adc_read();   /* 12-bit on RP2350 */
#endif

    float v_adc = (float)counts * dev->vcc_v / 4095.0f;

    /* Guard tiny voltages — interpret as "dark, LDR very high resistance".
     * Use a large sentinel; the lux formula below floors at ~0. */
    float r_ldr;
    if (v_adc <= 0.001f) {
        r_ldr = 1.0e7f;          /* 10 MΩ — effectively saturated */
    } else {
        r_ldr = dev->r_fixed_ohm * ((dev->vcc_v / v_adc) - 1.0f);
        if (r_ldr <= 0.0f) r_ldr = 1.0f;
    }

    /* lux = (A / R_LDR)^(1/B). powf handles the typical 1–1e5 lux range
     * without losing precision. */
    float lux = powf(dev->ldr_a / r_ldr, 1.0f / dev->ldr_b);
    if (lux < 0.0f)  lux = 0.0f;

    out->voltage_v      = v_adc;
    out->resistance_ohm = r_ldr;
    out->lux            = lux;
    return ERR_OK;
}

/* ── light_driver_t v-table adapter ──────────────────────────────────────── */
#ifndef HOST_TEST
#include "light_driver.h"
#include "board_pico2wh.h"

static Gl5516 s_gl5516_ctx;

static err_t gl5516_drv_init(void *ctx) {
    return gl5516_init((Gl5516 *)ctx,
                       BOARD_LDR_ADC_INPUT, BOARD_LDR_ADC_GPIO,
                       BOARD_LDR_VCC_V, BOARD_LDR_FIXED_OHM);
}

static err_t gl5516_drv_read(void *ctx, LightSample *out) {
    Gl5516Sample s;
    err_t e = gl5516_read_sample((Gl5516 *)ctx, &s);
    if (e != ERR_OK) return e;
    out->lux = s.lux;
    return ERR_OK;
}

static light_driver_t s_gl5516_driver = {
    .init        = gl5516_drv_init,
    .read_sample = gl5516_drv_read,
    .name        = "GL5516",
    .ctx         = &s_gl5516_ctx,
};

light_driver_t *light_gl5516_driver(void) { return &s_gl5516_driver; }
#endif /* HOST_TEST */
