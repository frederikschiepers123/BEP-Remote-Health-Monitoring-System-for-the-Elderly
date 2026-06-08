/*
 * Bring-up step 2 (CLAUDE.md §15): FreeRTOS hello — one task blinks at 1 Hz.
 *
 * Purpose: prove the FreeRTOS SMP port boots on the RP2350 with this repo's
 * FreeRTOSConfig.h (both Cortex-M33 cores, kernel-provided static memory,
 * heap_4). If step 1 blinks but this does not, the problem is the RTOS config,
 * not the board or flash path.
 *
 * Expected on hardware: onboard LED blinks at 1 Hz, driven from a task under
 * the scheduler (visually identical to step 1, but now via FreeRTOS).
 *
 * Build:  cmake -DPICO_BOARD=pico2_w -DBUILD_BRINGUP=ON .. && make bringup_freertos
 * Artefact: build/test/bringup/bringup_freertos.uf2
 */
#include "FreeRTOS.h"
#include "task.h"

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

static void blink_task(void *arg)
{
    (void)arg;
    for (;;) {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, true);
        vTaskDelay(pdMS_TO_TICKS(500));
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* FreeRTOSConfig.h requires these application hooks (configCHECK_FOR_STACK_-
 * OVERFLOW and configUSE_MALLOC_FAILED_HOOK). Minimal halt implementations. */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName);
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    (void)pcTaskName;
    for (;;) {
        tight_loop_contents();
    }
}

void vApplicationMallocFailedHook(void);
void vApplicationMallocFailedHook(void)
{
    for (;;) {
        tight_loop_contents();
    }
}

int main(void)
{
    if (cyw43_arch_init() != 0) {
        while (true) {
            tight_loop_contents();
        }
    }

    xTaskCreate(blink_task, "blink", 256, NULL, 1, NULL);
    vTaskStartScheduler();

    /* NOT REACHED */
    while (true) {
        tight_loop_contents();
    }
}
