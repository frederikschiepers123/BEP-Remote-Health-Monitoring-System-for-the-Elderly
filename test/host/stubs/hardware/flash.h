/* Host-test stub for <hardware/flash.h>.
 *
 * The spool component (components/spool/spool.c) includes this for the flash
 * geometry constants. On the host build (HOST_TEST=1) the spool's flash HAL is
 * a RAM-backed fake (inside spool.c) and never calls flash_range_* — those
 * declarations exist only so the on-target code path stays compilable; they are
 * #if'd out on host. The geometry must match the real RP2350 values. */
#ifndef HOST_STUB_HARDWARE_FLASH_H
#define HOST_STUB_HARDWARE_FLASH_H

#include <stdint.h>
#include <stddef.h>

#define FLASH_SECTOR_SIZE  (4096u)
#define FLASH_PAGE_SIZE    (256u)

void flash_range_erase(uint32_t flash_offs, size_t count);
void flash_range_program(uint32_t flash_offs, const uint8_t *data, size_t count);

#endif /* HOST_STUB_HARDWARE_FLASH_H */
