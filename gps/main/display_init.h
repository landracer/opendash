/**
 * @file display_init.h
 * @brief OpenDash GPS / Telemetry Unit — Hardware Initialization
 *
 * Hardware: Waveshare ESP32-S3-Touch-AMOLED-1.75
 * Display Controller: CO5300 (QSPI interface)
 * Touch Controller: CST9217 (I2C)
 * Resolution: 466×466 Round AMOLED
 *
 * Direct LVGL + ESP LCD integration (no BSP / esp_lvgl_adapter).
 * The BSP is incompatible with ESP-IDF 6.1-dev.
 */

#ifndef DISPLAY_INIT_H
#define DISPLAY_INIT_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Display dimensions */
#define GPS_LCD_H_RES   466
#define GPS_LCD_V_RES   466

/**
 * @brief Initialize all display hardware.
 *
 * Performs (in order):
 * 1. I2C master bus init (shared: touch, IMU, GPS, PMU)
 * 2. CO5300 QSPI AMOLED panel init
 * 3. CST9217 touch controller init
 * 4. LVGL display driver + draw buffers + tick timer
 * 5. Boot button + touch reading tasks
 *
 * MUST be called before gps_handler_init() and imu_handler_init()
 * since they obtain the I2C bus handle from display_get_i2c_handle().
 *
 * @return ESP_OK on success.
 */
esp_err_t display_init(void);

/**
 * @brief Get the shared I2C master bus handle.
 *
 * Used by GPS and IMU handlers to add devices on the shared bus.
 *
 * @return I2C master bus handle, or NULL if not initialized.
 */
i2c_master_bus_handle_t display_get_i2c_handle(void);

/**
 * @brief Lock LVGL mutex for thread-safe access.
 * @param timeout_ms Timeout in ms.
 * @return true if acquired.
 */
bool display_lvgl_lock(uint32_t timeout_ms);

/**
 * @brief Unlock LVGL mutex.
 */
void display_lvgl_unlock(void);

/**
 * @brief Get the LVGL display handle.
 */
lv_display_t *display_get_lvgl_disp(void);

/**
 * @brief Set display brightness (0–100%).
 * Brightness via CO5300 register 0x51 (AMOLED, no PWM).
 */
esp_err_t display_set_brightness(uint8_t brightness);

/**
 * @brief Get current brightness level.
 */
uint8_t display_get_brightness(void);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_INIT_H */
