/**
 * @file display_init.h
 * @brief OpenDash Center Display — Hardware Initialization
 *
 * Hardware: Waveshare ESP32-S3-Touch-LCD-4.3
 * LCD Controller: ST7262 (RGB interface)
 * Touch Controller: GT911 (I2C)
 * Resolution: 800×480 IPS LCD
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
 * This function performs the following:
 * 1. Configure the RGB LCD interface
 * 2. Initialize the LCD panel
 * 3. Initialize the touch controller (GT911)
 * 4. Initialize LVGL and register the display driver
 *
 * @return ESP_OK on success, error code on failure.
 */
esp_err_t display_init(void);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_INIT_H */
