/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
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

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the display hardware.
 *
 * This function performs the following:
 * 1. Configure the RGB LCD interface
 * 2. Initialize the LCD panel
 * 3. Turn on the backlight
 * 4. Initialize LVGL and register the display driver
 *
 * @return ESP_OK on success, error code on failure.
 */
esp_err_t display_init(void);

/**
 * @brief Lock LVGL mutex for thread-safe access
 *
 * Call this before accessing LVGL functions from multiple tasks.
 *
 * @param timeout_ms Timeout in milliseconds
 * @return true if mutex was acquired, false on timeout
 */
bool display_lvgl_lock(uint32_t timeout_ms);

/**
 * @brief Unlock LVGL mutex
 */
void display_lvgl_unlock(void);

/**
 * @brief Get the LVGL display handle
 *
 * @return Pointer to the LVGL display, or NULL if not initialized
 */
lv_display_t *display_get_lvgl_disp(void);

/**
 * @brief Set display backlight brightness
 *
 * Uses PWM to control backlight level on GPIO2.
 *
 * @param brightness Brightness level 0-100 (0 = off, 100 = max)
 * @return ESP_OK on success, error code on failure.
 */
esp_err_t display_set_brightness(uint8_t brightness);

/**
 * @brief Get current backlight brightness
 *
 * @return Current brightness level 0-100
 */
uint8_t display_get_brightness(void);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_INIT_H */
