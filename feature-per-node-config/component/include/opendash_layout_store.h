/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
#ifndef OPEN_DASH_LAYOUT_STORE_H
#define OPEN_DASH_LAYOUT_STORE_H

#include <stdint.h>
#include <esp_err.h>
#include "opendash_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Load a screen layout from NVS
 * 
 * @param mode Display mode (0-7)
 * @param out Pointer to screen_layout_v1_t to load into
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t opendash_layout_load(uint8_t mode, screen_layout_v1_t *out);

/**
 * @brief Save a screen layout to NVS
 * 
 * @param mode Display mode (0-7)
 * @param in Pointer to screen_layout_v1_t to save
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t opendash_layout_save(uint8_t mode, const screen_layout_v1_t *in);

/**
 * @brief Load a screen layout from NVS or use defaults
 * 
 * This function loads a layout from NVS. If no layout exists for the mode,
 * it uses the provided defaults and saves them to NVS for future use.
 * 
 * @param mode Display mode (0-7)
 * @param defaults Pointer to default screen_layout_v1_t
 * @param out Pointer to screen_layout_v1_t to load into
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t opendash_layout_load_or_default(uint8_t mode,
                                          const screen_layout_v1_t *defaults,
                                          screen_layout_v1_t *out);

/**
 * @brief Factory reset all layouts
 * 
 * This function erases all layouts from NVS, effectively resetting to defaults.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t opendash_layout_factory_reset(void);

#ifdef __cplusplus
}
#endif

#endif // OPEN_DASH_LAYOUT_STORE_H