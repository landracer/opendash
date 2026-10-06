/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file FreeRTOS.h (host test stub)
 * @brief Minimal FreeRTOS type/task-free surface for single-threaded host
 *        compilation of common/ modules. Task creation is NOT provided —
 *        tested modules must not start tasks (codec/config/health logic only).
 */
#ifndef OD_HOST_FREERTOS_H
#define OD_HOST_FREERTOS_H

#include <stdint.h>
#include <stdbool.h>

typedef int      BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;

#define pdTRUE   ((BaseType_t)1)
#define pdFALSE  ((BaseType_t)0)
#define portMAX_DELAY ((TickType_t)0xFFFFFFFFu)
#define configTICK_RATE_HZ 1000

#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

#endif /* OD_HOST_FREERTOS_H */