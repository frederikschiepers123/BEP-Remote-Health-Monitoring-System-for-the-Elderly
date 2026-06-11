#ifndef FLASH_MAP_H
#define FLASH_MAP_H

/* Single source of truth for the QSPI flash partition layout (CLAUDE.md §11).
 *
 * Both components/storage (littlefs KV) and components/spool (the non-volatile
 * outbound FIFO, ADR-0003) derive their flash regions from this header, so the
 * two can never silently overlap — a static_assert below tiles them exactly.
 *
 * Layout — the firmware image lives at the BOTTOM (XIP_BASE, grows up); the two
 * data regions are carved from the TOP of flash, growing down:
 *
 *     0x000000  ┌───────────────────────────────┐  XIP_BASE
 *               │  firmware .text/.rodata/.data  │  (grows up; must stay below
 *               │            ...                 │   SPOOL_FLASH_OFFSET)
 *               │           (free)               │
 *   SPOOL_OFF   ├───────────────────────────────┤  ← spool  (SPOOL_FLASH_SIZE)
 *               │  NV outbound FIFO (ADR-0003)   │
 *  STORAGE_OFF  ├───────────────────────────────┤  ← littlefs (STORAGE_FLASH_SIZE)
 *               │  /cfg /certs /state            │
 *               └───────────────────────────────┘  PICO_FLASH_SIZE_BYTES (top)
 *
 * Requires PICO_FLASH_SIZE_BYTES (pico/stdlib.h) and FLASH_SECTOR_SIZE
 * (hardware/flash.h) to be visible — include those headers before this one.
 */

#include <assert.h>

/* littlefs KV store: top 256 KB (unchanged from the original layout). */
#define STORAGE_FLASH_SIZE    (256u * 1024u)

/* Spool FIFO: 1 MB immediately below littlefs. One 256-byte flash page per
 * record → 4096 record slots → ≥15 min of buffered samples at the ~4 rec/s
 * aggregate publish cadence (env+air+radar+light), with margin (≈17 min).
 * Sized in whole flash sectors. See ADR-0003 for the capacity derivation. */
#define SPOOL_FLASH_SIZE      (1024u * 1024u)

#define STORAGE_FLASH_OFFSET  (PICO_FLASH_SIZE_BYTES - STORAGE_FLASH_SIZE)
#define SPOOL_FLASH_OFFSET    (PICO_FLASH_SIZE_BYTES - STORAGE_FLASH_SIZE - SPOOL_FLASH_SIZE)

/* The spool sits immediately below littlefs with no gap and no overlap. If a
 * future change resizes either region, this assert fails the build rather than
 * letting the two silently collide. */
static_assert(SPOOL_FLASH_OFFSET + SPOOL_FLASH_SIZE == STORAGE_FLASH_OFFSET,
              "spool and littlefs regions must be adjacent and non-overlapping");

/* Both regions must be a whole number of erase sectors. */
static_assert(STORAGE_FLASH_SIZE % FLASH_SECTOR_SIZE == 0u,
              "STORAGE_FLASH_SIZE must be a multiple of the flash sector size");
static_assert(SPOOL_FLASH_SIZE % FLASH_SECTOR_SIZE == 0u,
              "SPOOL_FLASH_SIZE must be a multiple of the flash sector size");

#endif /* FLASH_MAP_H */
