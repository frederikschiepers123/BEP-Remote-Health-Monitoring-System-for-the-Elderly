/*
 * Bring-up — probe `pico_cyw43_arch_lwip_sys_freertos` to reproduce / fix the
 * cyw43_arch_init hang documented in memory project_cyw43_sys_freertos_hang.md.
 *
 * Strategy: identical structure to bringup_wifi (proven to work under
 * threadsafe_background) but linked against the sys_freertos arch so we
 * exercise the real production code path. Adds debug prints around every
 * potentially-blocking call so we can see exactly where it stops returning.
 *
 * Run iteratively: tweak knobs (stack sizes, priorities, async_context
 * config), reflash, observe at which dbg line the output freezes.
 *
 * NOTE: this UF2 uses the *root* lwipopts.h (NO_SYS=0). The bringup-only
 * lwipopts override in test/bringup/lwip/ is for threadsafe_background only.
 */
#include "FreeRTOS.h"
#include "task.h"

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"

#include <stdio.h>

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PSK
#define WIFI_PSK ""
#endif

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName);
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    printf("[probe] STACK OVERFLOW in task '%s'\n",
           pcTaskName ? pcTaskName : "?");
    for (;;) { tight_loop_contents(); }
}
void vApplicationMallocFailedHook(void);
void vApplicationMallocFailedHook(void)
{
    printf("[probe] malloc failed\n");
    for (;;) { tight_loop_contents(); }
}

static void wifi_task(void *arg)
{
    (void)arg;

    while (!stdio_usb_connected()) vTaskDelay(pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(500));

    printf("\n[probe] sys_freertos wifi task on core=%u\n",
           (unsigned)get_core_num());

    printf("[probe] dbg(1): about to call cyw43_arch_init_with_country\n");
    int rc = cyw43_arch_init_with_country(CYW43_COUNTRY_NETHERLANDS);
    printf("[probe] dbg(2): cyw43_arch_init_with_country returned rc=%d\n", rc);
    if (rc != 0) {
        for (;;) {
            printf("[probe] cyw43 init FAILED rc=%d\n", rc);
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }

    printf("[probe] dbg(3): enable_sta_mode\n");
    cyw43_arch_enable_sta_mode();

    if (WIFI_SSID[0] == '\0') {
        for (;;) {
            printf("[probe] cyw43 init OK but no WIFI_SSID/PSK compiled in\n");
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }

    printf("[probe] dbg(4): connecting to SSID '%s' ...\n", WIFI_SSID);
    rc = cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PSK,
                                            CYW43_AUTH_WPA2_AES_PSK, 30000);
    printf("[probe] dbg(5): wifi_connect_timeout returned rc=%d\n", rc);
    if (rc != 0) {
        for (;;) {
            printf("[probe] wifi connect FAILED rc=%d\n", rc);
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }
    const ip4_addr_t *ip = netif_ip4_addr(netif_default);
    printf("[probe] CONNECTED — IP = %s\n",
           ip ? ip4addr_ntoa(ip) : "(none)");

    for (uint32_t hb = 0; ; hb++) {
        printf("[probe] heartbeat %lu  link=%d\n",
               (unsigned long)hb,
               cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA));
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

int main(void)
{
    stdio_init_all();   /* stdio first, like the working bring-ups */
    /* Pin to core 0 to match the working threadsafe_background path. */
    xTaskCreateAffinitySet(wifi_task, "wifi", 8192, NULL, 2, 0x01, NULL);
    vTaskStartScheduler();
    for (;;) { tight_loop_contents(); }
}
