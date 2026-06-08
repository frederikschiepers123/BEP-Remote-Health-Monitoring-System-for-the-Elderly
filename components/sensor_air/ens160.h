#ifndef ENS160_H
#define ENS160_H

/* ScioSense ENS160 air-quality sensor (eCO2 + TVOC + UBA AQI).
 *
 * Driver semantics:
 *   - ens160_init() resets the chip, verifies part ID (0x0160), then puts it
 *     into standard sensing mode. Returns ERR_OK without waiting for warmup —
 *     the caller decides what to do during the validity-flag-warmup period
 *     (~3 min after entering standard mode).
 *   - ens160_set_compensation() writes the current ambient temperature
 *     (Celsius) and humidity (%rH) to TEMP_IN / RH_IN. Recommended every
 *     sample for best gas-sensor accuracy; the BME280 supplies both.
 *   - ens160_read_sample() reads the status byte plus AQI / TVOC / eCO2 in a
 *     single 6-byte burst. Caller must inspect status (see ens160_validity)
 *     to know whether the reading is meaningful.
 *
 * Bus: I²C0 shared with the BME280 (CLAUDE.md §3.2). Default 7-bit address
 * 0x53; 0x52 if the ADDR pin is grounded.
 */

#include "err.h"
#include "hardware/i2c.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t co2_ppm;     /* eCO2 estimate (typ. 400–2000 ppm)                 */
    uint16_t tvoc_ppb;    /* total volatile organic compounds (0–65000 ppb)    */
    uint8_t  aqi;         /* UBA-style index 1=excellent … 5=unhealthy         */
    uint8_t  status;      /* raw STATUS register; pass to ens160_validity()    */
} Ens160Sample;

typedef struct {
    i2c_inst_t *i2c;
    uint8_t     addr;     /* 0x52 or 0x53 */
} Ens160;

/* Validity field (status bits 3:2). */
typedef enum {
    ENS160_VALIDITY_NORMAL          = 0,  /* steady-state, readings usable    */
    ENS160_VALIDITY_WARMUP          = 1,  /* first ~3 min after standard mode */
    ENS160_VALIDITY_INITIAL_STARTUP = 2,  /* first ~1 h for a brand-new chip  */
    ENS160_VALIDITY_INVALID         = 3   /* output unusable                  */
} Ens160Validity;

/* STATUS register flag bits (DATA_STATUS, 0x20). */
#define ENS160_STATUS_STATAS    0x80U   /* an OPMODE is running                */
#define ENS160_STATUS_STATER    0x40U   /* high-level device error            */
#define ENS160_STATUS_NEWDAT    0x02U   /* new sample in DATA_* registers      */

static inline Ens160Validity ens160_validity(uint8_t status)
{
    return (Ens160Validity)((status >> 2) & 0x03U);
}

static inline bool ens160_new_data_ready(uint8_t status)
{
    return (status & ENS160_STATUS_NEWDAT) != 0U;
}

/* True only when the chip reports an OPMODE actively running. A STATUS of
 * 0x00 has the validity bits = 00 (which ens160_validity() would call NORMAL),
 * but STATAS clear means the device is NOT measuring — its DATA_* registers
 * read as zeros. Always gate "is this reading usable" on this, not on the
 * validity field alone. */
static inline bool ens160_is_operating(uint8_t status)
{
    return (status & ENS160_STATUS_STATAS) != 0U;
}

err_t ens160_init(Ens160 *dev, i2c_inst_t *i2c, uint8_t addr);
err_t ens160_set_compensation(Ens160 *dev, float temp_c, float hum_pct);
err_t ens160_read_sample(Ens160 *dev, Ens160Sample *out);

#endif /* ENS160_H */
