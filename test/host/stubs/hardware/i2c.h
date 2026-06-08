/* Host-test stub for pico-sdk hardware/i2c.h.
 * Provides the complete i2c_inst_t type + blocking-API prototypes so driver
 * headers/sources compile natively. The test file supplies the function
 * bodies (mocked transactions). */
#ifndef HOST_STUB_HARDWARE_I2C_H
#define HOST_STUB_HARDWARE_I2C_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct i2c_inst { int dummy; } i2c_inst_t;

int i2c_write_blocking(i2c_inst_t *i2c, uint8_t addr,
                       const uint8_t *src, size_t len, bool nostop);
int i2c_read_blocking(i2c_inst_t *i2c, uint8_t addr,
                      uint8_t *dst, size_t len, bool nostop);

#endif /* HOST_STUB_HARDWARE_I2C_H */
