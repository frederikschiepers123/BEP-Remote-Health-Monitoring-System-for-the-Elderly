/* Host-test stub for FreeRTOS task.h — just the bits the drivers use. */
#ifndef HOST_STUB_TASK_H
#define HOST_STUB_TASK_H
#include <stdint.h>
typedef uint32_t TickType_t;
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define taskYIELD()       ((void)0)
void vTaskDelay(TickType_t ticks);   /* body in stubs.c (no-op) */
#endif
