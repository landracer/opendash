/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_layout_store.h
 * @brief NVS-backed persistence for per-mode screen layouts.
 *
 * Each node owns its own copy of every mode's layout. Storage is local
 * to the node — there is no sync protocol beyond CENTER pushing a fresh
 * layout via OPENDASH_CMD_SET_SCREEN_LAYOUT.
 *
 * NVS namespace: "od_layout"
 * NVS key:       "m<mode>"   (e.g. "m0", "m1", … one blob per mode)
 * Blob value:    sizeof(screen_layout_v1_t) bytes (host byte order)
 *
 * @see PER_NODE_DISPLAY_CONFIG_SPEC.md §4.3
 */

#ifndef OPENDASH_LAYOUT_STORE_H
#define OPENDASH_LAYOUT_STORE_H

#include <stdint.h>
#include "esp_err.h"
#include "opendash_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief NVS namespace used for all layout blobs. */
#define OPENDASH_LAYOUT_NVS_NAMESPACE   "od_layout"

/**
 * @brief Load the saved layout for a given mode from NVS.
 *
 * @param mode  Display mode (0..7).
 * @param out   Destination layout.
 * @return ESP_OK on success, ESP_ERR_NVS_NOT_FOUND if no entry,
 *         ESP_ERR_INVALID_VERSION on version mismatch, or another esp_err_t.
 */
esp_err_t opendash_layout_load(uint8_t mode, screen_layout_v1_t *out);

/**
 * @brief Persist a layout for a given mode to NVS (commits immediately).
 */
esp_err_t opendash_layout_save(uint8_t mode, const screen_layout_v1_t *in);

/**
 * @brief Load saved layout, or write `defaults` to NVS and return them.
 *
 * Call this from each node's UI init so first boot seeds NVS with the
 * compiled-in defaults and subsequent boots return user edits unchanged.
 */
esp_err_t opendash_layout_load_or_default(uint8_t mode,
                                          const screen_layout_v1_t *defaults,
                                          screen_layout_v1_t *out);

/**
 * @brief Erase every saved layout (factory reset).
 */
esp_err_t opendash_layout_factory_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENDASH_LAYOUT_STORE_H */
