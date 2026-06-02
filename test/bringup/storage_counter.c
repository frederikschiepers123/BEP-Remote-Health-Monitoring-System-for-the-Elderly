/*
 * Bring-up step 4 (CLAUDE.md §15): littlefs mount + boot counter.
 *
 * Drives the REAL components/storage layer: mount the filesystem (it formats
 * itself on first boot, when the region reads back as corrupt/blank), then
 * read-modify-write a small JSON boot counter. Reset the board and the count
 * climbs — proving littlefs persists across reboots.
 *
 * The storage work runs ONCE at boot; the result is then reported on a 1 Hz
 * loop (with an LED heartbeat) so you see it no matter when you open the port
 * — the board re-enumerates USB on every reset, so one-shot output gets missed.
 *
 * Uses the §11 canonical path /state/boot_count.json — this also exercises
 * storage_write()'s parent-directory auto-creation and atomic rename-on-close.
 *
 * Build:  cmake -DPICO_BOARD=pico2_w -DBUILD_BRINGUP=ON .. && make bringup_storage
 * Read it: open the Pico's COM port (see usb_console.c) and reset repeatedly.
 */
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include "storage.h"
#include "err.h"

#include <stdio.h>

#define BOOT_COUNT_PATH "/state/boot_count.json"

int main(void)
{
    stdio_init_all();
    bool have_led = (cyw43_arch_init() == 0);

    /* Do the filesystem work once, capturing the outcome to report on a loop. */
    err_t mount_err = storage_mount();
    err_t write_err = ERR_OK;
    unsigned long count = 0;
    const char *note = "";

    if (mount_err == ERR_OK) {
        char buf[64];
        size_t n = 0;
        err_t rerr = storage_read(BOOT_COUNT_PATH, buf, sizeof(buf) - 1u, &n);
        if (rerr == ERR_OK) {
            buf[n] = '\0';
            if (sscanf(buf, "{\"_v\":1,\"count\":%lu}", &count) != 1) {
                count = 0;
                note = "(previous file unparsable)";
            }
        } else if (rerr == ERR_NOT_FOUND) {
            note = "(first boot)";
        } else {
            note = "(read error)";
        }
        count++;
        int len = snprintf(buf, sizeof(buf), "{\"_v\":1,\"count\":%lu}", count);
        write_err = storage_write(BOOT_COUNT_PATH, buf, (size_t)len);
    }

    uint32_t hb = 0;
    for (;;) {
        if (have_led) {
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, (hb & 1u) != 0u);
        }
        if (mount_err != ERR_OK) {
            printf("[bringup] storage MOUNT FAILED err=%d  (heartbeat %lu)\n",
                   (int)mount_err, (unsigned long)hb);
        } else if (write_err != ERR_OK) {
            printf("[bringup] storage WRITE FAILED err=%d count=%lu  (heartbeat %lu)\n",
                   (int)write_err, count, (unsigned long)hb);
        } else {
            printf("[bringup] storage OK | boot count=%lu %s | reset to increment  (heartbeat %lu)\n",
                   count, note, (unsigned long)hb);
            /* Every 5 s, show the actual filesystem so you can SEE that /state
             * was created and the file is there, plus its raw JSON content. */
            if (hb % 5u == 0u && mount_err == ERR_OK) {
                storage_dump();
                char raw[64];
                size_t rn = 0;
                if (storage_read(BOOT_COUNT_PATH, raw, sizeof(raw) - 1u, &rn) == ERR_OK) {
                    raw[rn] = '\0';
                    printf("[bringup] %s contents: %s\n", BOOT_COUNT_PATH, raw);
                }
            }
        }
        hb++;
        sleep_ms(1000);
    }
}
