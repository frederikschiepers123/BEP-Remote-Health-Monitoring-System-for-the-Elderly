/* Host-test stub for pico-sdk hardware/uart.h. */
#ifndef HOST_STUB_HARDWARE_UART_H
#define HOST_STUB_HARDWARE_UART_H

#include <stdint.h>
#include <stdbool.h>

typedef struct uart_inst { int dummy; } uart_inst_t;

#define UART_PARITY_NONE 0

/* Prototypes — bodies supplied by the test file. */
unsigned uart_init(uart_inst_t *uart, unsigned baud);
void     uart_set_format(uart_inst_t *uart, unsigned data, unsigned stop, int parity);
void     uart_set_fifo_enabled(uart_inst_t *uart, bool enabled);
void     uart_deinit(uart_inst_t *uart);
bool     uart_is_readable(uart_inst_t *uart);
char     uart_getc(uart_inst_t *uart);

#endif /* HOST_STUB_HARDWARE_UART_H */
