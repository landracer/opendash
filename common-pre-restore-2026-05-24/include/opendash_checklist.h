/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_checklist.h
 * @brief OpenDash Pre-Flight Checklist System
 *
 * Provides a customizable checklist that teams/crews can use to verify
 * tasks are completed before the car runs. Checklist items are configured
 * per-node and status is shared across all nodes via I2C.
 *
 * Each checklist item has a name, a responsible node, and a completion status.
 * Items can be confirmed via touch interaction on any display.
 *
 * @par Example Use Cases
 * - Verify tire pressures checked
 * - Confirm brake bleed complete
 * - Ensure data logger armed
 * - Battery connections verified
 * - Parachute armed and pinned
 * - Fire suppression system checked
 *
 * @see ESP-IDF NVS API:
 *      https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32s3/api-reference/storage/nvs_flash.html
 */

#ifndef OPENDASH_CHECKLIST_H
#define OPENDASH_CHECKLIST_H

#include <stdint.h>
#include <stdbool.h>
#include "opendash_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ────────────────────────────────────────────────────────────────────────────
 * Checklist Constants
 * ──────────────────────────────────────────────────────────────────────────── */

/** @brief Maximum number of checklist items. */
#define OPENDASH_CHECKLIST_MAX_ITEMS    32

/** @brief Maximum length of a checklist item name (including null terminator). */
#define OPENDASH_CHECKLIST_NAME_LEN     48

/* ────────────────────────────────────────────────────────────────────────────
 * Checklist Item Status
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Checklist item completion status.
 */
typedef enum {
    OPENDASH_CHECK_PENDING    = 0,  /**< Not yet completed */
    OPENDASH_CHECK_COMPLETE   = 1,  /**< Marked as complete by crew */
    OPENDASH_CHECK_SKIPPED    = 2,  /**< Intentionally skipped */
    OPENDASH_CHECK_FAILED     = 3   /**< Item failed / needs attention */
} opendash_check_status_t;

/* ────────────────────────────────────────────────────────────────────────────
 * Checklist Data Structures
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Single checklist item.
 */
typedef struct {
    uint8_t                  id;                                /**< Item ID (0-31) */
    char                     name[OPENDASH_CHECKLIST_NAME_LEN]; /**< Human-readable name */
    opendash_check_status_t  status;                            /**< Current status */
    opendash_node_t          assigned_node;                     /**< Node responsible for this item */
    uint32_t                 completed_at_ms;                   /**< Timestamp when completed (0 if pending) */
} opendash_checklist_item_t;

/**
 * @brief Full checklist structure.
 */
typedef struct {
    opendash_checklist_item_t items[OPENDASH_CHECKLIST_MAX_ITEMS];  /**< All items */
    uint8_t                   count;                                /**< Number of active items */
    bool                      all_complete;                         /**< true if all items are complete */
} opendash_checklist_t;

/* ────────────────────────────────────────────────────────────────────────────
 * Checklist API
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Initialize the checklist system.
 *
 * Populates the checklist with a default set of common pre-race checks.
 * NVS-based persistence is planned but not yet implemented.
 *
 * @param[out] checklist  Pointer to checklist structure to initialize.
 *
 * @return OPENDASH_OK on success.
 */
opendash_err_t opendash_checklist_init(opendash_checklist_t *checklist);

/**
 * @brief Add a checklist item.
 *
 * @param[in,out] checklist  Pointer to checklist.
 * @param[in]     name       Item name string (null-terminated).
 * @param[in]     node       Node responsible for this item.
 *
 * @return OPENDASH_OK on success, OPENDASH_ERR_NO_MEM if checklist is full.
 */
opendash_err_t opendash_checklist_add(opendash_checklist_t *checklist,
                                       const char *name,
                                       opendash_node_t node);

/**
 * @brief Update the status of a checklist item.
 *
 * Called when a crew member taps to confirm an item on any display.
 * Status updates are broadcast to all nodes via I2C.
 *
 * @param[in,out] checklist     Pointer to checklist.
 * @param[in]     item_id       Item ID to update.
 * @param[in]     status        New status.
 * @param[in]     timestamp_ms  Timestamp of status change.
 *
 * @return OPENDASH_OK on success, OPENDASH_ERR_NOT_FOUND if item_id invalid.
 */
opendash_err_t opendash_checklist_update(opendash_checklist_t *checklist,
                                          uint8_t item_id,
                                          opendash_check_status_t status,
                                          uint32_t timestamp_ms);

/**
 * @brief Reset all checklist items to PENDING status.
 *
 * Called at the start of a new session or when crew needs to re-verify.
 *
 * @param[in,out] checklist  Pointer to checklist.
 *
 * @return OPENDASH_OK on success.
 */
opendash_err_t opendash_checklist_reset(opendash_checklist_t *checklist);

/**
 * @brief Check if all items are complete.
 *
 * Updates the all_complete flag and returns the result.
 *
 * @param[in,out] checklist  Pointer to checklist.
 *
 * @return true if all items are COMPLETE or SKIPPED, false otherwise.
 */
bool opendash_checklist_is_complete(opendash_checklist_t *checklist);

/**
 * @brief Save checklist configuration to NVS.
 *
 * @param[in] checklist  Pointer to checklist.
 *
 * @return OPENDASH_OK on success.
 */
opendash_err_t opendash_checklist_save(const opendash_checklist_t *checklist);

#ifdef __cplusplus
}
#endif

#endif /* OPENDASH_CHECKLIST_H */
