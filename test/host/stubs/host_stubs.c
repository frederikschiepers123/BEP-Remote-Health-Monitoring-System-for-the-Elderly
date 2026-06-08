/* Shared host-test stub implementations — see host_stubs.h.
 * Provides the pico-sdk / FreeRTOS symbols the drivers reference that are NOT
 * part of a test's specific mocked transaction surface. */
#include "host_stubs.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── Clock ───────────────────────────────────────────────────────────────── */
static uint32_t s_now_ms = 0;
static int      s_auto   = 1;

void host_time_reset(void)        { s_now_ms = 0; s_auto = 1; }
void host_time_set_ms(uint32_t m) { s_now_ms = m; s_auto = 0; }
void host_time_auto(int en)       { s_auto = en ? 1 : 0; }

/* pico/time.h surface */
typedef uint64_t absolute_time_t;
absolute_time_t get_absolute_time(void) { return 0; }
uint32_t to_ms_since_boot(absolute_time_t t) {
    (void)t;
    uint32_t v = s_now_ms;
    if (s_auto) s_now_ms++;
    return v;
}
uint64_t time_us_64(void) { return (uint64_t)s_now_ms * 1000u; }

/* ── FreeRTOS ────────────────────────────────────────────────────────────── */
void vTaskDelay(uint32_t ticks) { (void)ticks; }

/* ── log ─────────────────────────────────────────────────────────────────── */
void log_write(int level, const char *tag, const char *fmt, ...) {
    (void)level;
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[%s] ", tag); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}

/* ── hardware/gpio.h ─────────────────────────────────────────────────────── */
void gpio_set_function(unsigned gpio, int fn) { (void)gpio; (void)fn; }

/* ── hardware/uart.h — no-op config + a test-loaded byte feed ────────────── */
typedef struct uart_inst uart_inst_t;
unsigned uart_init(uart_inst_t *u, unsigned baud) { (void)u; (void)baud; return baud; }
void uart_set_format(uart_inst_t *u, unsigned d, unsigned s, int p) { (void)u; (void)d; (void)s; (void)p; }
void uart_set_fifo_enabled(uart_inst_t *u, bool e) { (void)u; (void)e; }
void uart_deinit(uart_inst_t *u) { (void)u; }

static const uint8_t *s_uart_buf = NULL;
static size_t s_uart_len = 0, s_uart_pos = 0;
void host_uart_load(const uint8_t *bytes, size_t len) {
    s_uart_buf = bytes; s_uart_len = len; s_uart_pos = 0;
}
bool uart_is_readable(uart_inst_t *u) { (void)u; return s_uart_pos < s_uart_len; }
char uart_getc(uart_inst_t *u) {
    (void)u;
    return (s_uart_pos < s_uart_len) ? (char)s_uart_buf[s_uart_pos++] : (char)0;
}

/* ── hardware/adc.h ──────────────────────────────────────────────────────── */
void     adc_init(void) {}
void     adc_gpio_init(unsigned g) { (void)g; }
void     adc_select_input(unsigned i) { (void)i; }
uint16_t adc_read(void) { return 0; }
