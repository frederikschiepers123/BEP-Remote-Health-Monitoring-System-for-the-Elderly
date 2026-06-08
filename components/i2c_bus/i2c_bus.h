#ifndef I2C_BUS_H
#define I2C_BUS_H

/* Shared I²C0 bus arbitration (CLAUDE.md §7.2, §13.5).
 *
 * Five firmware tasks touch I²C0: env_task (AHT21/BME280), air_task (ENS160),
 * light_task (BH1750), ui_task (SH1122 OLED).  The pico-sdk i2c_*_blocking
 * calls are NOT task-aware: if two tasks issue transactions concurrently under
 * SMP the byte streams interleave on the wire.  Observed symptoms (bench-
 * confirmed during bring-up): AHT21 humidity stuck on a previous value, BH1750
 * reading 0, ENS160 reading all zeros, OLED "I2C data row failed: -1".
 *
 * Every call site that touches I²C0 must hold this mutex for the whole
 * transaction.  It is a LEAF lock: no other mutex may be acquired while it is
 * held, and it must not be held across a blocking queue/network call.
 *
 * i2c_bus_init() also performs the one-time bus hardware init (i2c_init +
 * SDA/SCL function select + pull-ups) so there is exactly one owner of the
 * bus configuration.  Call it once from app_main before any task starts. */

#include "err.h"

/* Initialise the I²C0 hardware and create the bus mutex.  Idempotent-safe to
 * call once at boot.  Returns ERR_NO_MEM if the mutex cannot be created. */
err_t i2c_bus_init(void);

/* Acquire the bus.  Blocks until available (portMAX_DELAY). */
void i2c_bus_lock(void);

/* Release the bus. */
void i2c_bus_unlock(void);

#endif /* I2C_BUS_H */
