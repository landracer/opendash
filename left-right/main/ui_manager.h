/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file ui_manager.h
 * @brief OpenDash Left/Right Gauges — UI Manager
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
 */
esp_err_t ui_manager_init(const opendash_display_layout_t *layout);

/**
 * @brief Start the UI rendering task.
 */
esp_err_t ui_manager_start(void);

/**
 * @brief Update a data point value on the display.
 */
void ui_manager_update_value(uint16_t data_point_id, float value);

#ifdef __cplusplus
}
#endif

#endif /* UI_MANAGER_H */
