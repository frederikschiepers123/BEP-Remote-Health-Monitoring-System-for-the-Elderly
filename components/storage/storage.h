#ifndef STORAGE_H
#define STORAGE_H

/* Thin KV store over littlefs.
 * Mount point: top 256 KB of QSPI flash.
 * All file I/O in the firmware must go through this component.
 * No direct littlefs calls outside components/storage/.
 *
 * SMP note: storage_write triggers flash erase/program through
 * pico_flash's flash_safe_execute, which parks the "other" core for the
 * duration of the XIP-offline window. Under FreeRTOS-SMP (the
 * pico_cyw43_arch_lwip_sys_freertos arch), one task on each non-writing
 * core must have called multicore_lockout_victim_init() / its FreeRTOS
 * wrapper before the first storage_write. The single-core
 * threadsafe_background arch needs no extra setup. */

#include "err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Mount / unmount the filesystem. Call once at boot before any KV operation. */
err_t storage_mount(void);
void  storage_unmount(void);

/* Read an entire file into buf.  *len_out receives the number of bytes read.
 * Returns ERR_NOT_FOUND if the path does not exist. */
err_t storage_read(const char *path, void *buf, size_t buf_size, size_t *len_out);

/* Write buf to path, creating or overwriting the file atomically. */
err_t storage_write(const char *path, const void *buf, size_t len);

/* Remove a file.  Silently succeeds if the file does not exist. */
err_t storage_remove(const char *path);

/* Check whether path exists. */
bool  storage_exists(const char *path);

/* Debug: recursively print the filesystem tree (path, type, size) via the log,
 * for bring-up and on-device incident inspection over the serial console. */
void  storage_dump(void);

#endif /* STORAGE_H */
