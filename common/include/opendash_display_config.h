/**
 * @file opendash_display_config.h
 * @brief OpenDash Display Configuration System
 *
 * Provides a configuration layer that allows users to customize what data
 * appears on each screen section, what display mode is used (numeric, gauge,
 * bar, graph), and what alarm thresholds are active.
 *
 * Configuration is persisted in NVS (Non-Volatile Storage) so settings
 * survive power cycles. Can also be updated over I2C from the Center unit
 * or via the companion app over WiFi/BLE.
 *
 * @par Changing a display configuration
 * To change what data is displayed, modify the section's data_point_id.
 * This is the primary customization point — no code changes needed.
 *
 * @see ESP-IDF NVS API:
 *      https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32s3/api-reference/storage/nvs_flash.html
 */

#ifndef OPENDASH_DISPLAY_CONFIG_H
#define OPENDASH_DISPLAY_CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include "opendash_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ────────────────────────────────────────────────────────────────────────────
 * Display Mode Enumeration
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Display mode for a screen section.
 *
 * Determines how the data point value is rendered visually.
 */
typedef enum {
    OPENDASH_DISP_NUMERIC  = 0,  /**< Large numeric value with unit label */
    OPENDASH_DISP_ARC      = 1,  /**< Arc/gauge (circular sweep indicator) */
    OPENDASH_DISP_BAR      = 2,  /**< Horizontal or vertical bar graph */
    OPENDASH_DISP_GRAPH    = 3,  /**< Rolling line graph (history over time) */
} opendash_display_mode_t;

/* ────────────────────────────────────────────────────────────────────────────
 * Screen Layout Configuration
 * ──────────────────────────────────────────────────────────────────────────── */

/** @brief NVS namespace for display configuration storage. */
#define OPENDASH_NVS_NAMESPACE  "od_config"

/**
 * @brief Temperature unit preference
 */
typedef enum {
    OPENDASH_TEMP_CELSIUS    = 0,   /**< Celsius (°C) */
    OPENDASH_TEMP_FAHRENHEIT = 1,   /**< Fahrenheit (°F) */
} opendash_temp_unit_t;

/**
 * @brief Speed unit preference
 */
typedef enum {
    OPENDASH_SPEED_KMH = 0,   /**< Kilometers per hour (km/h) */
    OPENDASH_SPEED_MPH = 1,   /**< Miles per hour (mph) */
} opendash_speed_unit_t;

/**
 * @brief Full display layout configuration.
 *
 * Contains the configuration for all screen sections on a single display
 * node. Stored in NVS and loaded at boot.
 */
typedef struct {
    opendash_section_config_t sections[OPENDASH_MAX_SECTIONS];  /**< Per-section config */
    uint8_t                   num_sections;                     /**< Active section count */
    uint8_t                   brightness;                       /**< Backlight level 0-255 */
    uint8_t                   theme;                            /**< Color theme index */
    bool                      use_metric;                       /**< true=metric, false=imperial (legacy) */
    opendash_temp_unit_t      temp_unit;                        /**< Temperature display unit */
    opendash_speed_unit_t     speed_unit;                       /**< Speed display unit */
} opendash_display_layout_t;

/* ────────────────────────────────────────────────────────────────────────────
 * Configuration API
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Load display configuration from NVS.
 *
 * Reads the stored layout from Non-Volatile Storage. If no configuration
 * is found (first boot), loads default values appropriate for the node type.
 *
 * @param[in]  node    This node's type (determines default layout).
 * @param[out] layout  Pointer to layout structure to populate.
 *
 * @return OPENDASH_OK on success.
 *
 * @note Uses nvs_open() / nvs_get_blob() internally.
 * @see https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32s3/api-reference/storage/nvs_flash.html
 */
opendash_err_t opendash_config_load(opendash_node_t node,
                                     opendash_display_layout_t *layout);

/**
 * @brief Save display configuration to NVS.
 *
 * Persists the current layout so it survives power cycles.
 *
 * @param[in] layout  Pointer to layout structure to save.
 *
 * @return OPENDASH_OK on success, error code on NVS failure.
 */
opendash_err_t opendash_config_save(const opendash_display_layout_t *layout);

/**
 * @brief Reset configuration to factory defaults for a given node type.
 *
 * @param[in]  node    Node type (determines which defaults to apply).
 * @param[out] layout  Pointer to layout structure to populate with defaults.
 *
 * @return OPENDASH_OK on success.
 */
opendash_err_t opendash_config_reset_defaults(opendash_node_t node,
                                               opendash_display_layout_t *layout);

/**
 * @brief Update a single screen section's data point assignment.
 *
 * This is the primary way to change what data appears on screen.
 * The change is applied immediately and saved to NVS.
 *
 * @param[in,out] layout    Pointer to current layout.
 * @param[in]     section   Section index (0 to num_sections-1).
 * @param[in]     dp_id     New data point ID to display.
 * @param[in]     mode      Display mode (numeric, arc, bar, graph).
 *
 * @return OPENDASH_OK on success, OPENDASH_ERR_INVALID_ARG if section out of range.
 */
opendash_err_t opendash_config_set_section(opendash_display_layout_t *layout,
                                            uint8_t section,
                                            uint16_t dp_id,
                                            opendash_display_mode_t mode);

#ifdef __cplusplus
}
#endif

#endif /* OPENDASH_DISPLAY_CONFIG_H */
