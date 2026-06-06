/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file sd_logger.h
 * @brief OpenDash SD Card Logger — stub for center display
 *
 * Provides the sd_logger API used by espnow_master.c.
 * Currently a no-op stub — SD card logging can be implemented
 * when hardware pins are configured for the center display's TF slot.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t sd_logger_init(void);
esp_err_t sd_logger_start(void);
esp_err_t sd_logger_stop(void);
bool      sd_logger_is_available(void);
void      sd_logger_log_datapoint(uint16_t dp_id, float value);

#ifdef __cplusplus
}
#endif
