/* Host-test stub for <pico/stdlib.h>.
 *
 * Provides only what the host-compiled first-party sources need: the flash
 * size + XIP base used by flash_map.h / the spool. The 4 MB value matches the
 * Pico 2 W (RP2350) QSPI flash so the spool's derived geometry is identical to
 * the on-target build. XIP_BASE is unused on host (the spool's flash HAL is a
 * RAM fake), but defined so the on-target read path stays compilable. */
#ifndef HOST_STUB_PICO_STDLIB_H
#define HOST_STUB_PICO_STDLIB_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES  (4u * 1024u * 1024u)
#endif

#ifndef XIP_BASE
#define XIP_BASE  (0u)
#endif

#endif /* HOST_STUB_PICO_STDLIB_H */
