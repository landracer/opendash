/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file sd_logger.c
 * @brief OpenDash SD Card Logger — stub for center display
 *
 * No-op implementation. SD card logging for center can be added
 * when TF card slot pins are configured.
 */

#include "sd_logger.h"
#include "esp_log.h"

static const char *TAG = "sd_logger";

esp_err_t sd_logger_init(void)
{
    ESP_LOGI(TAG, "SD logger not configured for center display");
    return ESP_FAIL;
}

esp_err_t sd_logger_start(void)
{
    return ESP_ERR_INVALID_STATE;
}

esp_err_t sd_logger_stop(void)
{
    return ESP_OK;
}

bool sd_logger_is_available(void)
{
    return false;
}

void sd_logger_log_datapoint(uint16_t dp_id, float value)
{
    (void)dp_id;
    (void)value;
}
