/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file stubs/freertos/task.h (host test stub)
 * @brief Task API surface used by opendash_uart.c. Tasks are NEVER started
 *        on the host — xTaskCreate is a no-op that reports success.
 */
#ifndef OD_HOST_TASK_H
#define OD_HOST_TASK_H
#include "freertos/FreeRTOS.h"

typedef void *TaskHandle_t;
#define pdPASS ((BaseType_t)1)

BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t depth,
                       void *arg, UBaseType_t prio, TaskHandle_t *handle);
void vTaskDelay(TickType_t ticks);
void vTaskSuspend(TaskHandle_t t);

#endif /* OD_HOST_TASK_H */