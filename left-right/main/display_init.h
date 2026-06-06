/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file display_init.h
 * @brief OpenDash Left/Right Gauges — Hardware Initialization
 *
 * Hardware: Waveshare ESP32-S3-LCD-2.8C
 * LCD Controller: ST7701 (SPI interface)
 * Resolution: 480×480 Round IPS LCD
 */

#ifndef DISPLAY_INIT_H
#define DISPLAY_INIT_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the display hardware.
 *
 * Initializes the 480×480 round LCD panel and LVGL.
 *
 * @return ESP_OK on success, error code on failure.
 */
esp_err_t display_init(void);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_INIT_H */
