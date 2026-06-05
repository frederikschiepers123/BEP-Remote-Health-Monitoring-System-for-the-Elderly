/*
 * Bring-up — factory provisioning over USB-serial.
 *
 * Replaces the compile-time-baked cert path. The factory workstation flashes
 * this UF2, opens the Pico's COM port, and runs a small Python driver that
 * pushes the device's identity bundle (UUID + ECDSA P-256 keypair + cert
 * chain + per-device config) into the canonical §11 littlefs paths.
 *
 * Protocol (line-based ASCII, with binary payloads after PUT):
 *
 *   device → host   PROVISION READY v=1\n
 *   host → device   HELLO\n
 *   device → host   HELLO OK\n
 *   host → device   PUT <path> <len>\n<len bytes>
 *   device → host   OK sha256=<hex>\n         (or ERR <msg>\n)
 *   host → device   LIST\n
 *   device → host   FILE <path> <size> <sha256_hex>\n  (per file that exists)
 *                   LIST END\n
 *   host → device   REBOOT\n
 *   device → host   BYE\n  → watchdog reset
 *
 * Path whitelist: /cfg/... /certs/... /state/...  (§11 closed set).
 * Max payload per PUT: 4 KiB. Any parse failure → ERR; command loop continues
 * so the host can retry without re-flashing.
 *
 * mbedTLS supplies SHA-256 (already in the build for mTLS); no extra deps.
 */
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/watchdog.h"

#include "storage.h"
#include "err.h"

#include "mbedtls/sha256.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define PROV_MAX_PAYLOAD     4096
#define PROV_MAX_LINE         256
#define PROV_MAX_PATH          96
#define PROV_READ_TIMEOUT_US 5000000   /* 5 s per byte while reading a body */

static uint8_t g_payload[PROV_MAX_PAYLOAD];

/* Canonical §11 paths LIST walks. The on-device file universe is closed. */
static const char *const KNOWN_PATHS[] = {
    "/cfg/wifi.json",
    "/cfg/broker.json",
    "/cfg/device.json",
    "/cfg/sensors.json",
    "/certs/ca.der",
    "/certs/dev.crt",
    "/certs/dev.key",
    "/state/last_seq.json",
    "/state/boot_count.json",
};
#define KNOWN_PATHS_COUNT (sizeof(KNOWN_PATHS) / sizeof(KNOWN_PATHS[0]))

static void hex_encode(const uint8_t *in, size_t n, char *out) {
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i]     = H[(in[i] >> 4) & 0x0F];
        out[2 * i + 1] = H[in[i] & 0x0F];
    }
    out[2 * n] = '\0';
}

static void sha256_hex(const uint8_t *buf, size_t len, char hex[65]) {
    uint8_t digest[32];
    (void)mbedtls_sha256(buf, len, digest, 0);
    hex_encode(digest, sizeof(digest), hex);
}

/* Read one line into buf (NUL-terminated, '\n' stripped, '\r' stripped).
 * Returns line length on success, -1 on timeout, -2 on overflow. */
static int read_line(char *buf, size_t cap) {
    size_t i = 0;
    for (;;) {
        int c = getchar_timeout_us(PROV_READ_TIMEOUT_US);
        if (c == PICO_ERROR_TIMEOUT) return -1;
        if (c == '\n') {
            if (i > 0 && buf[i - 1] == '\r') i--;
            buf[i] = '\0';
            return (int)i;
        }
        if (i + 1u >= cap) return -2;
        buf[i++] = (char)c;
    }
}

/* Read exactly `len` bytes into buf. Per-byte 5 s timeout. */
static int read_exact(uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        int c = getchar_timeout_us(PROV_READ_TIMEOUT_US);
        if (c == PICO_ERROR_TIMEOUT) return -1;
        buf[i] = (uint8_t)c;
    }
    return 0;
}

static bool path_is_allowed(const char *path) {
    if (path[0] != '/') return false;
    if (strstr(path, "..") != NULL) return false;
    if (strncmp(path, "/cfg/",   5) == 0) return path[5]   != '\0';
    if (strncmp(path, "/certs/", 7) == 0) return path[7]   != '\0';
    if (strncmp(path, "/state/", 7) == 0) return path[7]   != '\0';
    return false;
}

static void cmd_put(char *args) {
    /* args = "<path> <len>" */
    char *path = args;
    char *sp   = strchr(args, ' ');
    if (sp == NULL) {
        printf("ERR PUT needs <path> <len>\n");
        return;
    }
    *sp = '\0';
    char *len_str = sp + 1;

    if (strlen(path) >= PROV_MAX_PATH) {
        printf("ERR path too long\n");
        return;
    }
    if (!path_is_allowed(path)) {
        printf("ERR path not allowed (use /cfg/ /certs/ /state/)\n");
        return;
    }

    char *end = NULL;
    unsigned long len = strtoul(len_str, &end, 10);
    if (end == len_str || *end != '\0') {
        printf("ERR len not an integer\n");
        return;
    }
    if (len == 0 || len > PROV_MAX_PAYLOAD) {
        printf("ERR len out of range (1..%d)\n", PROV_MAX_PAYLOAD);
        return;
    }

    if (read_exact(g_payload, (size_t)len) != 0) {
        printf("ERR timeout reading %lu bytes\n", len);
        return;
    }

    err_t e = storage_write(path, g_payload, (size_t)len);
    if (e != ERR_OK) {
        printf("ERR fs write failed err=%d\n", (int)e);
        return;
    }

    char hex[65];
    sha256_hex(g_payload, (size_t)len, hex);
    printf("OK sha256=%s\n", hex);
}

static void cmd_list(void) {
    for (size_t i = 0; i < KNOWN_PATHS_COUNT; i++) {
        const char *p = KNOWN_PATHS[i];
        if (!storage_exists(p)) continue;
        size_t n = 0;
        err_t e = storage_read(p, g_payload, sizeof(g_payload), &n);
        if (e != ERR_OK) {
            printf("ERR fs read failed for %s err=%d\n", p, (int)e);
            continue;
        }
        char hex[65];
        sha256_hex(g_payload, n, hex);
        printf("FILE %s %zu %s\n", p, n, hex);
    }
    printf("LIST END\n");
}

static void cmd_remove(char *args) {
    if (!path_is_allowed(args)) {
        printf("ERR path not allowed\n");
        return;
    }
    err_t e = storage_remove(args);
    if (e != ERR_OK) {
        printf("ERR fs remove failed err=%d\n", (int)e);
        return;
    }
    printf("OK removed\n");
}

static void reboot_now(void) {
    printf("BYE\n");
    /* Give USB a chance to flush before the reset. */
    sleep_ms(100);
    watchdog_reboot(0, 0, 0);
    for (;;) tight_loop_contents();
}

int main(void) {
    stdio_init_all();
    bool have_led = (cyw43_arch_init() == 0);

    err_t mount_err = storage_mount();

    while (!stdio_usb_connected()) {
        if (have_led) cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
        sleep_ms(50);
        if (have_led) cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
        sleep_ms(50);
    }
    sleep_ms(300);

    if (mount_err != ERR_OK) {
        printf("PROVISION FAIL mount err=%d\n", (int)mount_err);
        for (;;) sleep_ms(1000);
    }

    printf("PROVISION READY v=1\n");

    char line[PROV_MAX_LINE];
    for (;;) {
        if (have_led) cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
        int n = read_line(line, sizeof(line));
        if (have_led) cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);

        if (n == -1) continue;                     /* idle; keep waiting */
        if (n == -2) { printf("ERR line too long\n"); continue; }
        if (n == 0)  continue;

        if (strcmp(line, "HELLO") == 0) {
            printf("HELLO OK\n");
        } else if (strncmp(line, "PUT ", 4) == 0) {
            cmd_put(line + 4);
        } else if (strcmp(line, "LIST") == 0) {
            cmd_list();
        } else if (strncmp(line, "REMOVE ", 7) == 0) {
            cmd_remove(line + 7);
        } else if (strcmp(line, "REBOOT") == 0) {
            reboot_now();
        } else {
            printf("ERR unknown cmd '%s'\n", line);
        }
    }
}
