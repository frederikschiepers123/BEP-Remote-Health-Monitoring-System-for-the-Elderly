#define LOG_TAG "STORAGE"
#include "storage.h"

#include "log.h"
#include "lfs.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"
#include "pico/flash.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

/* Longest path we accept, including the ".tmp" suffix used for atomic writes. */
#define STORAGE_PATH_MAX        96

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

/* flash_safe_execute parks the other core (via the multicore lockout under
 * SMP, or trivially under single-core) and disables interrupts before running
 * the callback. This is the only correct way to touch flash while XIP is
 * serving instructions to either core. The previous save_and_disable_interrupts()
 * pattern was a single-core artefact and would crash core 1 the instant we
 * moved to pico_cyw43_arch_lwip_sys_freertos. See CLAUDE.md §7. */
#define STORAGE_FLASH_OP_TIMEOUT_MS 5000

typedef struct {
    uint32_t       addr;
    const uint8_t *buf;
    size_t         size;
} flash_prog_args_t;

static void do_flash_program(void *param) {
    const flash_prog_args_t *a = (const flash_prog_args_t *)param;
    flash_range_program(a->addr, a->buf, a->size);
}

static void do_flash_erase(void *param) {
    uint32_t addr = *(const uint32_t *)param;
    flash_range_erase(addr, FLASH_SECTOR_SIZE);
}

static int lfs_flash_prog(const struct lfs_config *cfg, lfs_block_t block,
                           lfs_off_t off, const void *buf, lfs_size_t size) {
    (void)cfg;
    flash_prog_args_t args = {
        .addr = (uint32_t)(STORAGE_FLASH_OFFSET + block * FLASH_SECTOR_SIZE + off),
        .buf  = (const uint8_t *)buf,
        .size = size,
    };
    int rc = flash_safe_execute(do_flash_program, &args, STORAGE_FLASH_OP_TIMEOUT_MS);
    if (rc != PICO_OK) {
        LOG_E("flash_safe_execute(prog) failed: %d", rc);
        return LFS_ERR_IO;
    }
    return LFS_ERR_OK;
}

static int lfs_flash_erase(const struct lfs_config *cfg, lfs_block_t block) {
    (void)cfg;
    uint32_t addr = (uint32_t)(STORAGE_FLASH_OFFSET + block * FLASH_SECTOR_SIZE);
    int rc = flash_safe_execute(do_flash_erase, &addr, STORAGE_FLASH_OP_TIMEOUT_MS);
    if (rc != PICO_OK) {
        LOG_E("flash_safe_execute(erase) failed: %d", rc);
        return LFS_ERR_IO;
    }
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

/* Create every parent directory of `path` (mkdir -p style). Existing
 * directories are not an error. The final path segment (the file) is left
 * alone. Root always exists. */
static err_t ensure_parent_dirs(const char *path) {
    char tmp[STORAGE_PATH_MAX];
    size_t len = strlen(path);
    if (len + 1u > sizeof(tmp)) return ERR_INVALID_ARG;
    memcpy(tmp, path, len + 1u);

    for (char *p = tmp + 1; *p != '\0'; p++) {   /* skip the leading '/' */
        if (*p == '/') {
            *p = '\0';
            int rc = lfs_mkdir(&lfs, tmp);
            if (rc != LFS_ERR_OK && rc != LFS_ERR_EXIST) return ERR_FS;
            *p = '/';
        }
    }
    return ERR_OK;
}

/* Atomic write (CLAUDE.md §11.2): write to "<path>.tmp", then rename over the
 * target. lfs_rename is atomic and lfs_file_close flushes, so a power loss
 * mid-write leaves the previous file intact rather than a truncated one. This
 * is the single write path used everywhere — §11.2 names it storage_write_atomic
 * but making storage_write itself atomic avoids churning every caller. */
err_t storage_write(const char *path, const void *buf, size_t len) {
    err_t e = ensure_parent_dirs(path);
    if (e != ERR_OK) return e;

    char tmp_path[STORAGE_PATH_MAX];
    int np = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    if (np < 0 || (size_t)np >= sizeof(tmp_path)) return ERR_INVALID_ARG;

    lfs_file_t f;
    int rc = lfs_file_open(&lfs, &f, tmp_path,
                            LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (rc != LFS_ERR_OK) return ERR_FS;

    lfs_ssize_t n = lfs_file_write(&lfs, &f, buf, (lfs_size_t)len);
    rc = lfs_file_close(&lfs, &f);
    if (n != (lfs_ssize_t)len || rc != LFS_ERR_OK) {
        (void)lfs_remove(&lfs, tmp_path);
        return ERR_FS;
    }

    rc = lfs_rename(&lfs, tmp_path, path);
    if (rc != LFS_ERR_OK) {
        (void)lfs_remove(&lfs, tmp_path);
        return ERR_FS;
    }
    return ERR_OK;
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

/* Recursively list `path`. Depth-bounded; the §11 layout is only one level
 * deep, so 4 is generous. Each frame holds an lfs_info (~256 B), so this is a
 * debug-only helper — call it from a context with ample stack. */
static void dump_dir(const char *path, int depth) {
    if (depth > 4) return;

    lfs_dir_t dir;
    int rc = lfs_dir_open(&lfs, &dir, path);
    if (rc != LFS_ERR_OK) {
        LOG_W("dump: cannot open %s (%d)", path, rc);
        return;
    }

    struct lfs_info info;
    while (lfs_dir_read(&lfs, &dir, &info) > 0) {
        if (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0) {
            continue;
        }
        char child[STORAGE_PATH_MAX];
        int np = (strcmp(path, "/") == 0)
                     ? snprintf(child, sizeof(child), "/%s", info.name)
                     : snprintf(child, sizeof(child), "%s/%s", path, info.name);
        if (np < 0 || (size_t)np >= sizeof(child)) {
            LOG_W("dump: path too long under %s", path);
            continue;
        }
        if (info.type == LFS_TYPE_DIR) {
            LOG_I("  %*s%s/", depth * 2, "", info.name);
            dump_dir(child, depth + 1);
        } else {
            LOG_I("  %*s%s  (%lu bytes)", depth * 2, "", info.name,
                  (unsigned long)info.size);
        }
    }
    lfs_dir_close(&lfs, &dir);
}

void storage_dump(void) {
    if (!mounted) {
        LOG_W("dump: filesystem not mounted");
        return;
    }
    LOG_I("filesystem tree (/ = top of the 256 KB littlefs region):");
    dump_dir("/", 0);
}
