#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* ── Scheduler ────────────────────────────────────────────────────────────── */
#define configUSE_PREEMPTION                    1
#define configUSE_TIME_SLICING                  1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 0

/* ── SMP (both RP2350 Cortex-M33 cores) ──────────────────────────────────── */
#define configNUMBER_OF_CORES                   2
#define configUSE_CORE_AFFINITY                 1
#define configRUN_MULTIPLE_PRIORITIES           1
#define configUSE_TASK_PREEMPTION_DISABLE       0
#define configUSE_PASSIVE_IDLE_HOOK             0   /* required when SMP is enabled */

/* ── ARMv8-M port (RP2350 Cortex-M33, No-TrustZone) ──────────────────────── */
/* Values mandated by the RP2350_ARM_NTZ port: single privilege level, secure
 * state only, no MPU, no TrustZone. FPU enabled (we use floating point). */
#define configENABLE_FPU                        1
#define configENABLE_MPU                        0
#define configENABLE_TRUSTZONE                  0
#define configRUN_FREERTOS_SECURE_ONLY          1

/* ── Tick ─────────────────────────────────────────────────────────────────── */
#define configCPU_CLOCK_HZ                      150000000UL
#define configTICK_RATE_HZ                      1000
#define configUSE_TICKLESS_IDLE                 0   /* always-mains-powered */

/* ── Tasks ────────────────────────────────────────────────────────────────── */
#define configMAX_PRIORITIES                    8
#define configMINIMAL_STACK_SIZE                256   /* words */
#define configMAX_TASK_NAME_LEN                 16
#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_TASK_NOTIFICATIONS            1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES   3

/* ── Mutexes / semaphores ─────────────────────────────────────────────────── */
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             1
#define configUSE_COUNTING_SEMAPHORES           1
#define configUSE_QUEUE_SETS                    0

/* ── Memory ───────────────────────────────────────────────────────────────── */
#define configSUPPORT_STATIC_ALLOCATION         1
#define configSUPPORT_DYNAMIC_ALLOCATION        1
/* Let the kernel supply the static buffers for the idle/passive-idle/timer
 * tasks, so the application need not provide vApplicationGet*TaskMemory(). */
#define configKERNEL_PROVIDED_STATIC_MEMORY     1
#define configTOTAL_HEAP_SIZE                   (192 * 1024)   /* 192 KB */
#define configAPPLICATION_ALLOCATED_HEAP        0

/* ── Hooks ────────────────────────────────────────────────────────────────── */
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0
#define configUSE_MALLOC_FAILED_HOOK            1
#define configCHECK_FOR_STACK_OVERFLOW          2   /* full pattern check */

/* ── Run-time stats ───────────────────────────────────────────────────────── */
#define configGENERATE_RUN_TIME_STATS           0
#define configUSE_TRACE_FACILITY                1
#define configUSE_STATS_FORMATTING_FUNCTIONS    0

/* ── Timers ───────────────────────────────────────────────────────────────── */
#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               (configMAX_PRIORITIES - 1)
#define configTIMER_QUEUE_LENGTH                10
#define configTIMER_TASK_STACK_DEPTH            512   /* words */

/* ── Assert ───────────────────────────────────────────────────────────────── */
#include <assert.h>
#define configASSERT(x) assert(x)

/* ── Co-routines (unused) ─────────────────────────────────────────────────── */
#define configUSE_CO_ROUTINES                   0

/* ── API inclusion ────────────────────────────────────────────────────────── */
#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_xResumeFromISR                  1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_uxTaskGetStackHighWaterMark     1
#define INCLUDE_xTaskGetHandle                  1
#define INCLUDE_eTaskGetState                   1
#define INCLUDE_xEventGroupSetBitFromISR        1
#define INCLUDE_xTimerPendFunctionCall          1
#define INCLUDE_xTaskAbortDelay                 1
#define INCLUDE_xTaskGetTaskHandle              1
#define INCLUDE_xSemaphoreGetMutexHolder        1   /* used by pico_async_context */

/* ── Interrupt priority (Cortex-M33) ─────────────────────────────────────── */
#ifdef __NVIC_PRIO_BITS
    #define configPRIO_BITS __NVIC_PRIO_BITS
#else
    #define configPRIO_BITS 4
#endif
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY         15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY    5
#define configKERNEL_INTERRUPT_PRIORITY         (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

#endif /* FREERTOS_CONFIG_H */
