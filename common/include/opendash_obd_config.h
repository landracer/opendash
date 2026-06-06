/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_obd_config.h
 * @brief OBD2 feature configuration with NVS persistence
 *
 * Manages OBD2 opt-in settings:
 * - Enable/disable OBD page in normal swipe cycle
 * - Warning thresholds per sensor (caution + critical levels)
 * - MIL indicator enable/disable
 *
 * All settings are persisted to NVS under the "obd_cfg" namespace.
 */

#ifndef OPENDASH_OBD_CONFIG_H
#define OPENDASH_OBD_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of configurable warning thresholds */
#define OBD_WARN_THRESHOLD_COUNT  6

/** Warning threshold for a single sensor */
typedef struct {
    float caution;   /**< Caution level (orange flash) — 0 = disabled */
    float critical;  /**< Critical level (red flash) — 0 = disabled */
    bool  above;     /**< true = warn when ABOVE threshold, false = when BELOW */
} obd_warning_threshold_t;

/** Sensor indices for warning thresholds */
typedef enum {
    OBD_WARN_COOLANT_TEMP = 0,
    OBD_WARN_OIL_TEMP     = 1,
    OBD_WARN_OIL_PRESSURE = 2,
    OBD_WARN_BATTERY_VOLT = 3,
    OBD_WARN_BOOST_PSI    = 4,
    OBD_WARN_AFR          = 5,
} obd_warn_sensor_t;

/** Full OBD configuration */
typedef struct {
    bool obd_enabled;           /**< OBD page shows in normal swipe cycle */
    bool mil_indicator_enabled; /**< Show MIL icon on all screens when DTCs present */
    obd_warning_threshold_t warnings[OBD_WARN_THRESHOLD_COUNT];
} obd_config_t;

/**
 * @brief Load OBD config from NVS (or set defaults if not found).
 * Call once during init after nvs_flash_init().
 */
void obd_config_load(void);

/**
 * @brief Save current OBD config to NVS.
 */
void obd_config_save(void);

/**
 * @brief Get pointer to current OBD config (read-only).
 */
const obd_config_t *obd_config_get(void);

/**
 * @brief Toggle OBD enabled state and save.
 * @return New enabled state.
 */
bool obd_config_toggle_enabled(void);

/**
 * @brief Toggle MIL indicator and save.
 * @return New MIL indicator state.
 */
bool obd_config_toggle_mil_indicator(void);

/**
 * @brief Set a warning threshold and save.
 */
void obd_config_set_warning(obd_warn_sensor_t sensor,
                             float caution, float critical, bool above);

#ifdef __cplusplus
}
#endif

#endif /* OPENDASH_OBD_CONFIG_H */
