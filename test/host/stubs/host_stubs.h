/* Shared host-test helpers (implemented in host_stubs.c).
 *
 * Centralises the non-test-specific stubs so each test only has to mock the
 * transaction surface it actually drives (i2c read/write sequence, or the
 * radar UART byte stream). Time is host-controlled and deterministic. */
#ifndef HOST_STUBS_H
#define HOST_STUBS_H

#include <stdint.h>
#include <stddef.h>

/* ── Host-controlled clock (backs to_ms_since_boot / time_us_64) ──────────────
 * Default: each query auto-advances 1 ms so driver timeout loops always make
 * progress. A test may pin or step the clock explicitly. */
void     host_time_reset(void);              /* clock = 0, auto-advance on      */
void     host_time_set_ms(uint32_t ms);      /* pin clock; disables auto-advance */
void     host_time_auto(int enabled);        /* toggle the +1ms-per-query mode   */

/* ── Radar UART byte feed (backs uart_is_readable / uart_getc) ───────────────
 * Load a buffer of bytes the BHA2 parser will consume; once drained,
 * uart_is_readable returns false so read_byte times out. */
void     host_uart_load(const uint8_t *bytes, size_t len);

#endif /* HOST_STUBS_H */
