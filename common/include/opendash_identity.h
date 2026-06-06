/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_identity.h
 * @brief OpenDash Device Identity & Serial Number System
 *
 * Prevents flashing wrong firmware to wrong hardware by storing a device
 * identity in NVS. Each unit self-identifies at boot using:
 *   - Hardware MAC address (unique per ESP32 chip — the "serial number")
 *   - Compiled node type (CENTER/LEFT/RIGHT/GPS/BMS/etc.)
 *   - NVS-stored expected node type (written on first flash)
 *
 * At boot, if the compiled node type doesn't match the NVS-stored identity,
 * a prominent warning is logged. This catches accidental cross-flashing.
 *
 * Usage (in each unit's main.c, after NVS init):
 *   opendash_identity_init(OPENDASH_NODE_CENTER);  // or LEFT, RIGHT, GPS...
 *
 * The first call on a fresh device writes the identity to NVS.
 * Subsequent boots compare the compiled type against the stored type.
 */

#ifndef OPENDASH_IDENTITY_H
#define OPENDASH_IDENTITY_H

#include "opendash_common.h"
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Device identity info (populated by opendash_identity_init).
 */
typedef struct {
    uint8_t mac[6];                 /**< Hardware WiFi MAC address (serial #) */
    char serial_str[18];            /**< MAC as string "AA:BB:CC:DD:EE:FF" */
    opendash_node_t compiled_node;  /**< Node type this firmware was built for */
    opendash_node_t stored_node;    /**< Node type stored in NVS (first flash) */
    bool identity_match;            /**< true if compiled == stored */
    bool first_boot;                /**< true if NVS had no stored identity */
} opendash_identity_t;

/**
 * @brief Initialize device identity and verify firmware/hardware match.
 *
 * Reads the ESP32 base MAC address, checks NVS for a previously stored
 * node identity, and validates that this firmware belongs on this unit.
 *
 * On first boot (no identity in NVS), the compiled node type is stored.
 * On subsequent boots, the stored type is compared against the compiled type.
 *
 * Logs a banner with serial number and match status at boot.
 *
 * @param compiled_node  The node type this firmware was compiled for.
 * @return ESP_OK on success (even if mismatch — it's a warning, not fatal).
 */
esp_err_t opendash_identity_init(opendash_node_t compiled_node);

/**
 * @brief Get the current device identity (after init).
 *
 * @param[out] id  Pointer to identity structure to populate.
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not yet initialized.
 */
esp_err_t opendash_identity_get(opendash_identity_t *id);

/**
 * @brief Get a human-readable name for a node type.
 *
 * @param node  The node type.
 * @return Static string like "CENTER", "LEFT", "RIGHT", "GPS", etc.
 */
const char *opendash_node_name(opendash_node_t node);

/**
 * @brief Force-reset the stored identity in NVS.
 *
 * Use this when intentionally re-purposing a unit (e.g., turning a
 * Left gauge into a Right gauge). The next boot will store the new
 * compiled node type.
 *
 * @return ESP_OK on success.
 */
esp_err_t opendash_identity_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENDASH_IDENTITY_H */
