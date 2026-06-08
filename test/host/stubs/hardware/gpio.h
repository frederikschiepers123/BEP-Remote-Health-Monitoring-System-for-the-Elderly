/* Host-test stub for pico-sdk hardware/gpio.h. */
#ifndef HOST_STUB_HARDWARE_GPIO_H
#define HOST_STUB_HARDWARE_GPIO_H
#define GPIO_FUNC_UART 2
#define GPIO_FUNC_I2C  3
void gpio_set_function(unsigned gpio, int fn);
#endif
