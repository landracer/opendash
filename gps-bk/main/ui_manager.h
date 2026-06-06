/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file ui_manager.h
 * @brief OpenDash GPS / Telemetry Unit — UI Manager
 *
 * Manages the LVGL user interface on the 466×466 round AMOLED display.
 *
 * Display Modes (cycle with boot button):
 *   GPS   — Speed (large), heading, satellites, coordinates, fix status
 *   LAP   — Lap time, delta, speed, sector info
 *   GFORCE — G-force display with lateral/longitudinal/total values
 *
 * Architecture mirrors the center display's single-screen, multi-mode
 * approach: all LVGL objects are created once during init, and mode
 * changes only update labels — no object creation/destruction at runtime.
 */

#ifndef UI_MANAGER_H
#define UI_MANAGER_H

#include "esp_err.h"
#include "opendash_display_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Display modes for the GPS unit
 */
typedef enum {
    GPS_DISPLAY_MODE_GPS     = 0,  /**< GPS data: speed, heading, sats, coords */
    GPS_DISPLAY_MODE_LAP     = 1,  /**< Lap timing: lap time, delta, speed */
    GPS_DISPLAY_MODE_GFORCE  = 2,  /**< G-force: lateral, longitudinal, total */
    GPS_DISPLAY_MODE_DEBUG   = 3,  /**< Debug: NMEA stats, sentence types, raw data */
    GPS_DISPLAY_MODE_COUNT   = 4   /**< Total display modes */
} gps_display_mode_t;

/**
 * @brief Initialize the UI manager.
 *
 * Creates the baseline UI layout for the round AMOLED display.
 * All LVGL objects are created once, display mode changes only update text.
 *
 * @param[in] layout  Pointer to the display layout configuration.
 * @return ESP_OK on success.
 */
esp_err_t ui_manager_init(const opendash_display_layout_t *layout);

/**
 * @brief Start the UI rendering and update tasks.
 *
 * @return ESP_OK on success.
 */
esp_err_t ui_manager_start(void);

/**
 * @brief Update a data point value on the display.
 *
 * Called by the I2C node when values are pushed from the Center display.
 *
 * @param[in] data_point_id  Data point ID to update.
 * @param[in] value          New value to display.
 */
void ui_manager_update_value(uint16_t data_point_id, float value);

/**
 * @brief Cycle to the next display mode.
 *
 * Called by the boot button handler in display_init.c.
 * Cycles: GPS → LAP → GFORCE → GPS → ...
 *
 * @return ESP_OK on success.
 */
esp_err_t ui_manager_next_screen(void);

/**
 * @brief Set display mode directly.
 *
 * @param[in] mode  Display mode to switch to.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if mode out of range.
 */
esp_err_t ui_manager_set_display_mode(gps_display_mode_t mode);

/**
 * @brief Get current display mode index.
 *
 * @return Current display mode.
 */
uint8_t ui_manager_get_current_screen(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_MANAGER_H */
