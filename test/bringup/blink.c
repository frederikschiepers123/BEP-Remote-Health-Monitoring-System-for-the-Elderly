/*
 * Bring-up step 1 (CLAUDE.md §15): bare-metal blink, NO FreeRTOS.
 *
 * Purpose: prove the toolchain, the .uf2 flash path, and that the board boots
 * and runs our code at all. The Pico 2 W's onboard LED is wired to the CYW43
 * radio chip, so we toggle it via pico_cyw43_arch (not a bare GPIO).
 *
 * Expected on hardware: onboard LED blinks at 1 Hz (500 ms on / 500 ms off).
 *
 * Build:  cmake -DPICO_BOARD=pico2_w -DBUILD_BRINGUP=ON .. && make bringup_blink
 * Artefact: build/test/bringup/bringup_blink.uf2
 */
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

int main(void)
{
    if (cyw43_arch_init() != 0) {
        /* CYW43 init failed — nothing we can signal without the LED; halt. */
        while (true) {
            tight_loop_contents();
        }
    }

    while (true) {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, true);
        sleep_ms(500);
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
        sleep_ms(500);
    }
}
