/**
 * @file ui_manager.h
 * @brief OpenDash Center Display — UI Manager
 *
 * Manages the LVGL user interface, including:
 * - Screen layout and sections
 * - Data point widgets (gauges, numeric displays, bars)
 * - Touch event handling
 * - Screen transitions
 */

#ifndef UI_MANAGER_H
#define UI_MANAGER_H

#include "esp_err.h"
#include "opendash_display_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the UI manager.
 *
 * Creates the baseline UI layout based on the provided configuration.
 *
 * @param[in] layout  Pointer to the display layout configuration.
 *
 * @return ESP_OK on success.
 */
esp_err_t ui_manager_init(const opendash_display_layout_t *layout);

/**
 * @brief Start the UI rendering task.
 *
 * Starts a FreeRTOS task that continuously calls lv_task_handler()
 * to render the UI.
 *
 * @return ESP_OK on success.
 */
esp_err_t ui_manager_start(void);

/**
 * @brief Update a data point value on the display.
 *
 * Updates the displayed value for a given data point ID.
 *
 * @param[in] data_point_id  Data point ID to update.
 * @param[in] value          New value to display.
 */
void ui_manager_update_value(uint16_t data_point_id, float value);

#ifdef __cplusplus
}
#endif

#endif /* UI_MANAGER_H */
