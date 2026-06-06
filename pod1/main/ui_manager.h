/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file ui_manager.h
 * @brief OpenDash Pod 1 — UI Manager
 *
 * Manages the LVGL user interface on the 466×466 round AMOLED display.
 *
 * Display Modes (cycle with swipe or boot button):
 *   OIL_TEMP — Oil temperature (large) + info panel
 *   WATER    — Coolant temperature (large) + info panel
 *   AFR      — Air-fuel ratio (large) + info panel
 *   BOOST    — Boost pressure (large) + info panel
 *   GFORCE   — G-force display (lateral/longitudinal/total)
 *   DEBUG    — System diagnostics, ESP-NOW stats
 *
 * All LVGL objects created once during init. Mode changes only update
 * label text — no object creation/destruction at runtime.
 */

#ifndef UI_MANAGER_H
#define UI_MANAGER_H

#include "esp_err.h"
#include "opendash_display_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    POD_DISPLAY_MODE_OIL     = 0,  /**< Oil temperature focus */
    POD_DISPLAY_MODE_WATER   = 1,  /**< Coolant temperature focus */
    POD_DISPLAY_MODE_AFR     = 2,  /**< Air-fuel ratio focus */
    POD_DISPLAY_MODE_BOOST   = 3,  /**< Boost pressure focus */
    POD_DISPLAY_MODE_GFORCE  = 4,  /**< G-force: lateral, longitudinal, total */
    POD_DISPLAY_MODE_DEBUG   = 5,  /**< Debug: ESP-NOW stats, system info */
    POD_DISPLAY_MODE_COUNT   = 6   /**< Total display modes */
} pod_display_mode_t;

esp_err_t ui_manager_init(const opendash_display_layout_t *layout);
esp_err_t ui_manager_start(void);
void ui_manager_update_value(uint16_t data_point_id, float value);
esp_err_t ui_manager_next_screen(void);
esp_err_t ui_manager_set_display_mode(pod_display_mode_t mode);
uint8_t ui_manager_get_current_screen(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_MANAGER_H */
