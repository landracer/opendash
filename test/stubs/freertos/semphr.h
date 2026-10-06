/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file semphr.h (host test stub)
 * @brief Single-threaded no-op counting semaphore: Take/Give always succeed.
 *        Tests are single-threaded, so the mutex is logically transparent.
 */
#ifndef OD_HOST_SEMPHR_H
#define OD_HOST_SEMPHR_H

#include "freertos/FreeRTOS.h"

typedef struct od_fake_sem * SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t s);

#endif /* OD_HOST_SEMPHR_H */
