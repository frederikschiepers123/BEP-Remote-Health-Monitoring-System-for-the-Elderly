#ifndef STORAGE_H
#define STORAGE_H

/* Thin KV store over littlefs.
 * Mount point: top 256 KB of QSPI flash.
 * All file I/O in the firmware must go through this component.
 * No direct littlefs calls outside components/storage/. */

#include "err.h"
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

#endif /* STORAGE_H */
