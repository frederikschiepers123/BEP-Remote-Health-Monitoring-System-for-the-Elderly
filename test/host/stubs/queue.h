/* Host-test stub for FreeRTOS queue.h — only the handle type is referenced
 * by radar_driver.h on the host build (the radar_task queue lives elsewhere). */
#ifndef HOST_STUB_QUEUE_H
#define HOST_STUB_QUEUE_H
typedef void *QueueHandle_t;
#endif
