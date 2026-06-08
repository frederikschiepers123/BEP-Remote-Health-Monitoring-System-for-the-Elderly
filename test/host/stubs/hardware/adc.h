/* Host-test stub for pico-sdk hardware/adc.h. */
#ifndef HOST_STUB_HARDWARE_ADC_H
#define HOST_STUB_HARDWARE_ADC_H
#include <stdint.h>
void     adc_init(void);
void     adc_gpio_init(unsigned gpio);
void     adc_select_input(unsigned input);
uint16_t adc_read(void);
#endif
