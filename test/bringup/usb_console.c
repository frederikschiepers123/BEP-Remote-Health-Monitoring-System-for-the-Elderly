/*
 * Bring-up step 3 (revised): USB-serial DEV console.
 *
 * Scope note: the USB-CDC *data link to the tablet* is deferred — the firmware
 * runs over Wi-Fi for now (see CLAUDE.md §2.1 scope note). This program is NOT
 * that link. It is a developer convenience: the Pico enumerates as a plain COM
 * port on YOUR PC so you can read log output while bringing up Wi-Fi.
 *
 * It uses the SDK's pico_stdio_usb, which provides its own USB descriptors and
 * self-services tud_task() from a timer/IRQ — so plain printf() + sleep_ms()
 * works with no FreeRTOS and no custom USB stack.
 *
 * Expected on hardware: a USB serial port appears on the PC; opening it shows a
 * 1 Hz "tick" line, and the onboard LED toggles in step.
 *
 * Build:  cmake -DPICO_BOARD=pico2_w -DBUILD_BRINGUP=ON .. && make bringup_usb_console
 * Read it: Windows → Device Manager shows a new COMx; open it at any baud in
 *          PuTTY / a serial monitor (USB-CDC ignores the baud rate).
 */
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include <stdio.h>

int main(void)
{
    stdio_init_all();              /* brings up the USB-serial console */
    (void)cyw43_arch_init();       /* onboard LED lives on the CYW43 chip */

    /* Give a freshly-attached host a moment to open the port. */
    for (int i = 0; i < 20 && !stdio_usb_connected(); i++) {
        sleep_ms(100);
    }

    uint32_t tick = 0;
    while (true) {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, (tick & 1u) != 0u);
        printf("[bringup] usb-serial console alive: tick %lu\n",
               (unsigned long)tick++);
        sleep_ms(1000);
    }
}
