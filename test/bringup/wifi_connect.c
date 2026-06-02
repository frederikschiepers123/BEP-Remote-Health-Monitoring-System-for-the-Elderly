/*
 * Bring-up step 5 (CLAUDE.md §15 step 13): Wi-Fi associate + DHCP, plus /cfg
 * provisioning. This is the Wi-Fi-only milestone (USB tablet link deferred).
 *
 * It mirrors the real firmware's network stack: FreeRTOS + lwIP via
 * pico_cyw43_arch_lwip_sys_freertos, with logs on the USB-serial console.
 *
 * Credentials are NOT in source or in git. Supply them at configure time and
 * they are compiled into THIS dev build only (build/ is gitignored):
 *
 *   WIFI_SSID=MyNet WIFI_PSK=secret BROKER_IP=192.168.1.50 \
 *       cmake -S . -B build -DPICO_BOARD=pico2_w -DBUILD_BRINGUP=ON
 *   cmake --build build --target bringup_wifi
 *
 * On boot it writes /cfg/wifi.json + /cfg/broker.json (so the real firmware can
 * later read them), associates, and prints the DHCP-assigned IP on a loop.
 */
#include "FreeRTOS.h"
#include "task.h"

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"

#include "storage.h"
#include "err.h"

#include <stdio.h>
#include <string.h>

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PSK
#define WIFI_PSK ""
#endif
#ifndef BROKER_HOST
#define BROKER_HOST "tablet.local"
#endif
#ifndef BROKER_IP
#define BROKER_IP ""
#endif
#ifndef BROKER_PORT
#define BROKER_PORT 8883
#endif

/* FreeRTOSConfig.h required hooks. */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName);
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    printf("[bringup] STACK OVERFLOW in '%s'\n", pcTaskName ? pcTaskName : "?");
    for (;;) { tight_loop_contents(); }
}
void vApplicationMallocFailedHook(void);
void vApplicationMallocFailedHook(void)
{
    printf("[bringup] malloc failed\n");
    for (;;) { tight_loop_contents(); }
}

/* Write /cfg/wifi.json and /cfg/broker.json from the compiled-in values, so the
 * real firmware (which reads /cfg) is provisioned too. Exercises the new
 * mkdir-auto-create + atomic write. */
static void provision_cfg(void)
{
    char buf[160];
    int n;

    if (WIFI_SSID[0] != '\0') {
        n = snprintf(buf, sizeof(buf),
                     "{\"_v\":1,\"ssid\":\"%s\",\"psk\":\"%s\",\"country\":\"NL\"}",
                     WIFI_SSID, WIFI_PSK);
        if (n > 0 && (size_t)n < sizeof(buf)) {
            err_t e = storage_write("/cfg/wifi.json", buf, (size_t)n);
            printf("[bringup] wrote /cfg/wifi.json (%s)\n", e == ERR_OK ? "ok" : "FAIL");
        }
    }
    printf("[bringup] dbg: building broker JSON\n");
    n = snprintf(buf, sizeof(buf),
                 "{\"_v\":1,\"host\":\"%s\",\"ip\":\"%s\",\"port\":%d}",
                 BROKER_HOST, BROKER_IP, (int)BROKER_PORT);
    printf("[bringup] dbg: broker JSON built (n=%d), calling storage_write\n", n);
    if (n > 0 && (size_t)n < sizeof(buf)) {
        err_t e = storage_write("/cfg/broker.json", buf, (size_t)n);
        printf("[bringup] wrote /cfg/broker.json (%s)\n", e == ERR_OK ? "ok" : "FAIL");
    }
    printf("[bringup] dbg: provision_cfg done\n");
}

static void wifi_task(void *arg)
{
    (void)arg;

    /* stdio_init_all() must run AFTER vTaskStartScheduler under the SDK's
     * pico_async_context_freertos / pico_time_adapter integration, otherwise
     * pico_stdio_usb often fails to enumerate a COM port. */
    stdio_init_all();

    /* Wait (potentially long) for the host to open the COM port BEFORE doing
     * any heavy work. cyw43_arch_init runs blocking SPI/PIO transfers that can
     * starve the IRQ-driven tud_task() — if that happens while Windows is
     * mid-CDC-handshake on the port open, the host times out with
     * ERROR_SEM_TIMEOUT (121). With wifi init deferred until after the port is
     * open, the CDC handshake completes cleanly and stays robust afterwards. */
    while (!stdio_usb_connected()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelay(pdMS_TO_TICKS(500));   /* let Windows finish line-coding setup */
    printf("\n[bringup] wifi_task: host connected, starting init (core=%u)\n",
           (unsigned)get_core_num());

    if (storage_mount() != ERR_OK) {
        printf("[bringup] storage_mount FAILED — continuing without /cfg\n");
    } else {
        provision_cfg();
        printf("[bringup] dbg: calling storage_dump\n");
        storage_dump();
        printf("[bringup] dbg: storage_dump returned\n");
    }

    if (WIFI_SSID[0] == '\0') {
        for (;;) {
            printf("[bringup] NO WIFI CREDENTIALS COMPILED IN — re-configure with:\n");
            printf("[bringup]   WIFI_SSID=<name> WIFI_PSK=<pw> [BROKER_IP=<ip>] cmake ...\n");
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }

    /* cyw43_arch must be initialised from a task in the sys_freertos config. */
    printf("[bringup] dbg: calling cyw43_arch_init_with_country (this can take several seconds)\n");
    int init_rc = cyw43_arch_init_with_country(CYW43_COUNTRY_NETHERLANDS);
    printf("[bringup] dbg: cyw43_arch_init_with_country returned rc=%d\n", init_rc);
    if (init_rc != 0) {
        for (;;) {
            printf("[bringup] cyw43_arch_init FAILED rc=%d (heartbeat)\n", init_rc);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
    printf("[bringup] dbg: cyw43 init OK, lighting LED to confirm chip is reachable\n");
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, true);
    cyw43_arch_enable_sta_mode();
    printf("[bringup] dbg: enable_sta_mode done; connecting to SSID '%s' ...\n", WIFI_SSID);

    bool connected = false;
    for (uint32_t attempt = 1; ; attempt++) {
        if (!connected) {
            int rc = cyw43_arch_wifi_connect_timeout_ms(
                WIFI_SSID, WIFI_PSK, CYW43_AUTH_WPA2_AES_PSK, 30000);
            if (rc == 0) {
                connected = true;
                cyw43_arch_lwip_begin();
                const ip4_addr_t *ip = netif_ip4_addr(netif_default);
                printf("[bringup] CONNECTED — IP = %s\n",
                       ip ? ip4addr_ntoa(ip) : "(none yet)");
                cyw43_arch_lwip_end();
            } else {
                printf("[bringup] connect attempt %lu failed rc=%d — retrying\n",
                       (unsigned long)attempt, rc);
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
        }

        int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        cyw43_arch_lwip_begin();
        const ip4_addr_t *ip = netif_ip4_addr(netif_default);
        printf("[bringup] link=%d ip=%s broker=%s:%d (heartbeat %lu)\n",
               link, ip ? ip4addr_ntoa(ip) : "0.0.0.0",
               BROKER_IP[0] ? BROKER_IP : BROKER_HOST, (int)BROKER_PORT,
               (unsigned long)attempt);
        cyw43_arch_lwip_end();

        if (link < 0) {           /* link dropped — re-associate */
            connected = false;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

int main(void)
{
    /* stdio_init_all() is intentionally NOT called here — it's done inside
     * wifi_task after the scheduler is up (see comment in wifi_task). */
    /* Pin to core 0: the pico-sdk's sys_freertos cyw43 + lwIP integration is
     * not fully SMP-clean, and cyw43_arch_init can hang if the calling task
     * migrates between cores during init. Affinity mask 0x01 = core 0 only. */
    xTaskCreateAffinitySet(wifi_task, "wifi", 4096, NULL, 2, 0x01, NULL);
    vTaskStartScheduler();
    for (;;) { tight_loop_contents(); }
}
