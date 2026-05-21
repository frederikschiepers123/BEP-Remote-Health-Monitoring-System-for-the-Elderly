#define LOG_TAG "STORAGE"
#include "storage.h"

#include "log.h"
#include "lfs.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

#include <string.h>
#include <stdbool.h>

/* ── littlefs flash backend ──────────────────────────────────────────────── */

#define STORAGE_FLASH_SIZE      (256 * 1024)
#define STORAGE_FLASH_OFFSET    (PICO_FLASH_SIZE_BYTES - STORAGE_FLASH_SIZE)

static int lfs_flash_read(const struct lfs_config *cfg, lfs_block_t block,
                           lfs_off_t off, void *buf, lfs_size_t size) {
    (void)cfg;
    uint32_t addr = (uint32_t)(STORAGE_FLASH_OFFSET + block * FLASH_SECTOR_SIZE + off);
    memcpy(buf, (const void *)(XIP_BASE + addr), size);
    return LFS_ERR_OK;
}

static int lfs_flash_prog(const struct lfs_config *cfg, lfs_block_t block,
                           lfs_off_t off, const void *buf, lfs_size_t size) {
    (void)cfg;
    uint32_t addr = (uint32_t)(STORAGE_FLASH_OFFSET + block * FLASH_SECTOR_SIZE + off);
    uint32_t irq = save_and_disable_interrupts();
    flash_range_program(addr, (const uint8_t *)buf, size);
    restore_interrupts(irq);
    return LFS_ERR_OK;
}

static int lfs_flash_erase(const struct lfs_config *cfg, lfs_block_t block) {
    (void)cfg;
    uint32_t addr = (uint32_t)(STORAGE_FLASH_OFFSET + block * FLASH_SECTOR_SIZE);
    uint32_t irq = save_and_disable_interrupts();
    flash_range_erase(addr, FLASH_SECTOR_SIZE);
    restore_interrupts(irq);
    return LFS_ERR_OK;
}

static int lfs_flash_sync(const struct lfs_config *cfg) {
    (void)cfg;
    return LFS_ERR_OK;
}

static uint8_t lfs_read_buf[256];
static uint8_t lfs_prog_buf[256];
static uint8_t lfs_lookahead_buf[16];

static const struct lfs_config lfs_cfg = {
    .read  = lfs_flash_read,
    .prog  = lfs_flash_prog,
    .erase = lfs_flash_erase,
    .sync  = lfs_flash_sync,
    .read_size      = 256,
    .prog_size      = 256,
    .block_size     = FLASH_SECTOR_SIZE,
    .block_count    = STORAGE_FLASH_SIZE / FLASH_SECTOR_SIZE,
    .cache_size     = 256,
    .lookahead_size = 16,
    .block_cycles   = 500,
    .read_buffer    = lfs_read_buf,
    .prog_buffer    = lfs_prog_buf,
    .lookahead_buffer = lfs_lookahead_buf,
};

static lfs_t lfs;
static bool  mounted = false;

/* ── Public API ──────────────────────────────────────────────────────────── */

err_t storage_mount(void) {
    int rc = lfs_mount(&lfs, &lfs_cfg);
    if (rc == LFS_ERR_CORRUPT) {
        LOG_W("filesystem corrupt — formatting");
        rc = lfs_format(&lfs, &lfs_cfg);
        if (rc != LFS_ERR_OK) {
            LOG_E("format failed: %d", rc);
            return ERR_FS;
        }
        rc = lfs_mount(&lfs, &lfs_cfg);
    }
    if (rc != LFS_ERR_OK) {
        LOG_E("mount failed: %d", rc);
        return ERR_FS;
    }
    mounted = true;
    LOG_I("mounted (%u KB)", (unsigned)(STORAGE_FLASH_SIZE / 1024));
    return ERR_OK;
}

void storage_unmount(void) {
    if (mounted) {
        lfs_unmount(&lfs);
        mounted = false;
    }
}

err_t storage_read(const char *path, void *buf, size_t buf_size, size_t *len_out) {
    lfs_file_t f;
    int rc = lfs_file_open(&lfs, &f, path, LFS_O_RDONLY);
    if (rc == LFS_ERR_NOENT) return ERR_NOT_FOUND;
    if (rc != LFS_ERR_OK) return ERR_FS;

    lfs_ssize_t n = lfs_file_read(&lfs, &f, buf, (lfs_size_t)buf_size);
    lfs_file_close(&lfs, &f);

    if (n < 0) return ERR_FS;
    if (len_out) *len_out = (size_t)n;
    return ERR_OK;
}

err_t storage_write(const char *path, const void *buf, size_t len) {
    lfs_file_t f;
    int rc = lfs_file_open(&lfs, &f, path,
                            LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (rc != LFS_ERR_OK) return ERR_FS;

    lfs_ssize_t n = lfs_file_write(&lfs, &f, buf, (lfs_size_t)len);
    lfs_file_close(&lfs, &f);

    return (n == (lfs_ssize_t)len) ? ERR_OK : ERR_FS;
}

err_t storage_remove(const char *path) {
    int rc = lfs_remove(&lfs, path);
    if (rc == LFS_ERR_NOENT) return ERR_OK;
    return (rc == LFS_ERR_OK) ? ERR_OK : ERR_FS;
}

bool storage_exists(const char *path) {
    struct lfs_info info;
    return lfs_stat(&lfs, path, &info) == LFS_ERR_OK;
}
