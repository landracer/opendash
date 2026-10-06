/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file esp_timer.h (host test stub)
 * @brief Controllable virtual clock. Tests advance time deterministically —
 *        no wall-clock dependence in health/timeout logic.
 */
#ifndef OD_HOST_ESP_TIMER_H
#define OD_HOST_ESP_TIMER_H

#include <stdint.h>

/* Microseconds since (virtual) boot. Backed by a test-controlled counter. */
int64_t esp_timer_get_time(void);

/* Test controls */
void     od_stub_clock_reset(void);
void     od_stub_clock_advance_ms(uint32_t ms);

#endif /* OD_HOST_ESP_TIMER_H */
