/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file channel_registry.h
 * @brief Center-side persistent channel registry — the software switch panel's
 *        button→relay bindings, plus the start-panel boot-routing flag.
 *
 * The 28 soft buttons on the switch panel (DISPLAY_MODE_RELAY) each bind to a
 * (node, channel) pair with a human label. The whole table lives in NVS so a
 * reflash never loses the mapping; edits land immediately (write-through).
 *
 * The "panel auto" flag drives boot routing: when enabled and the engine is
 * not running (no/low sensor data at boot) the center boots straight into
 * the switch panel instead of the gauge screens.
 */

#ifndef OPENDASH_CENTER_CHANNEL_REGISTRY_H
#define OPENDASH_CENTER_CHANNEL_REGISTRY_H

#include "opendash_common.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Number of soft buttons on the switch panel (7 columns × 4 rows). */
#define CHANNEL_REGISTRY_COUNT  28

/** @brief Max label length including NUL. */
#define CHANNEL_LABEL_MAX       12

/**
 * @brief One panel button binding: what it is called and what it drives.
 *
 * is_on is the last-known channel state (cached across screens and center
 * reboots — a power-fault on a slave is detected against this cache).
 */
typedef struct {
    char            label[CHANNEL_LABEL_MAX]; /**< Display label, e.g. "RAD FAN 1" */
    opendash_node_t node;                     /**< Target relay/MOS node */
    uint8_t         channel;                  /**< Channel index on that node (0-based) */
    bool            is_on;                    /**< Cached channel state (true=ON) */
} channel_binding_t;

/**
 * @brief Load the registry + panel-auto flag from NVS (defaults when absent).
 *        Call once at boot BEFORE ui_manager_init().
 */
esp_err_t channel_registry_init(void);

/** @brief Read-only view of one binding (NULL if idx out of range). */
const channel_binding_t *channel_registry_get(int idx);

/**
 * @brief Re-bind button idx to a new (node, channel) pair.
 *
 * Regenerates the label from the new binding ("<NODE> CH<n>") and
 * write-throughs the whole table to NVS.
 */
esp_err_t channel_registry_set_binding(int idx, opendash_node_t node, uint8_t channel);

/** @brief Set a custom label for button idx (truncated to CHANNEL_LABEL_MAX-1, uppercased). */
esp_err_t channel_registry_set_label(int idx, const char *label);

/** @brief Update cached channel state for button idx (no NVS write). */
void channel_registry_set_state(int idx, bool on);

/** @brief Find the button bound to (node, channel); -1 if none. */
int channel_registry_find(opendash_node_t node, uint8_t channel);

/** @brief Restore factory default bindings/labels and persist. */
esp_err_t channel_registry_reset_defaults(void);

/** @brief Short human name for a node ("RLY4", "RLY-A", "MOS-B", ...). */
const char *channel_registry_node_short(opendash_node_t node);

/** @brief Format the canonical auto-label for a binding into out[]. */
void channel_registry_format_label(char *out, size_t sz, opendash_node_t node, uint8_t channel);

/** @brief Boot-routing flag: true = boot to switch panel while engine is off. */
bool channel_registry_panel_auto(void);

/** @brief Persist the boot-routing flag. */
esp_err_t channel_registry_set_panel_auto(bool on);

#ifdef __cplusplus
}
#endif

#endif /* OPENDASH_CENTER_CHANNEL_REGISTRY_H */
