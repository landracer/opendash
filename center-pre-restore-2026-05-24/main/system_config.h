/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file system_config.h
 * @brief Center-side persistent system configuration (NVS-backed).
 *
 * Stores user-tweakable runtime settings that do not belong in any specific
 * subsystem. Currently:
 *
 *   - g_boost_target_node : which slave runs the boost controller.
 *
 * NVS namespace: "boost_ui" (re-used for any future per-node UI prefs).
 */

#ifndef OPENDASH_CENTER_SYSTEM_CONFIG_H
#define OPENDASH_CENTER_SYSTEM_CONFIG_H

#include "opendash_common.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The slave node that hosts the active boost controller.
 *
 * Default: OPENDASH_NODE_MOS_4CH_A. Persisted via NVS key "boost_target"
 * in the "boost_ui" namespace. The boost UI writes this when the user
 * picks a different node, and boost_client reads it every push tick.
 */
extern opendash_node_t g_boost_target_node;

/** @brief Load persisted settings, install defaults if absent. */
esp_err_t system_config_init(void);

/** @brief Persist the current g_boost_target_node value to NVS. */
esp_err_t system_config_save_boost_target(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENDASH_CENTER_SYSTEM_CONFIG_H */
