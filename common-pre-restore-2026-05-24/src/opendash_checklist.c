/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_checklist.c
 * @brief OpenDash Pre-Flight Checklist — Implementation
 *
 * Implements the pre-flight checklist system. Provides functions to
 * initialize, add items, update status, reset, and check completion.
 *
 * @see opendash_checklist.h for the full API documentation.
 */

#include "opendash_checklist.h"
#include <string.h>

/* ────────────────────────────────────────────────────────────────────────────
 * Default Checklist Items
 *
 * These are pre-loaded on first boot if no saved checklist exists in NVS.
 * Teams can customize this list via the companion app or by editing this
 * array and reflashing.
 * ──────────────────────────────────────────────────────────────────────────── */

/** @brief Default pre-flight checklist entries. */
static const struct {
    const char      *name;
    opendash_node_t  node;
} default_items[] = {
    { "Tire pressures checked",          OPENDASH_NODE_CENTER },
    { "Lug nuts torqued",                OPENDASH_NODE_CENTER },
    { "Brake fluid level OK",            OPENDASH_NODE_CENTER },
    { "Coolant level OK",                OPENDASH_NODE_CENTER },
    { "Oil level OK",                    OPENDASH_NODE_CENTER },
    { "Battery connections secure",      OPENDASH_NODE_BMS    },
    { "BMS reporting normal",            OPENDASH_NODE_BMS    },
    { "Data logger armed",               OPENDASH_NODE_GPS    },
    { "GPS satellite lock acquired",     OPENDASH_NODE_GPS    },
    { "Parachute armed and pinned",      OPENDASH_NODE_GPS    },
    { "Fire suppression system checked", OPENDASH_NODE_CENTER },
    { "Harness and belts secure",        OPENDASH_NODE_CENTER },
    { "Kill switch tested",              OPENDASH_NODE_CENTER },
    { "Radio check complete",            OPENDASH_NODE_CENTER },
};

/** @brief Number of default items. */
#define DEFAULT_ITEM_COUNT (sizeof(default_items) / sizeof(default_items[0]))

/* ────────────────────────────────────────────────────────────────────────────
 * opendash_checklist_init
 * ──────────────────────────────────────────────────────────────────────────── */

opendash_err_t opendash_checklist_init(opendash_checklist_t *checklist)
{
    if (checklist == NULL) {
        return OPENDASH_ERR_INVALID_ARG;
    }

    /* Clear the entire structure */
    memset(checklist, 0, sizeof(opendash_checklist_t));

    /*
     * TODO: Attempt to load from NVS first.
     * For now, populate with default items.
     *
     * Future implementation:
     *   nvs_handle_t handle;
     *   esp_err_t err = nvs_open(OPENDASH_NVS_NAMESPACE, NVS_READONLY, &handle);
     *   if (err == ESP_OK) { ... load from NVS ... }
     */

    /* Load default checklist items */
    for (uint8_t i = 0; i < DEFAULT_ITEM_COUNT && i < OPENDASH_CHECKLIST_MAX_ITEMS; i++) {
        checklist->items[i].id            = i;
        checklist->items[i].status        = OPENDASH_CHECK_PENDING;
        checklist->items[i].assigned_node = default_items[i].node;
        checklist->items[i].completed_at_ms = 0;

        /* Copy the name string, ensuring null termination */
        strncpy(checklist->items[i].name,
                default_items[i].name,
                OPENDASH_CHECKLIST_NAME_LEN - 1);
        checklist->items[i].name[OPENDASH_CHECKLIST_NAME_LEN - 1] = '\0';
    }

    checklist->count        = (DEFAULT_ITEM_COUNT < OPENDASH_CHECKLIST_MAX_ITEMS)
                              ? (uint8_t)DEFAULT_ITEM_COUNT
                              : OPENDASH_CHECKLIST_MAX_ITEMS;
    checklist->all_complete = false;

    return OPENDASH_OK;
}

/* ────────────────────────────────────────────────────────────────────────────
 * opendash_checklist_add
 * ──────────────────────────────────────────────────────────────────────────── */

opendash_err_t opendash_checklist_add(opendash_checklist_t *checklist,
                                       const char *name,
                                       opendash_node_t node)
{
    if (checklist == NULL || name == NULL) {
        return OPENDASH_ERR_INVALID_ARG;
    }

    if (checklist->count >= OPENDASH_CHECKLIST_MAX_ITEMS) {
        return OPENDASH_ERR_NO_MEM;
    }

    uint8_t idx = checklist->count;

    checklist->items[idx].id            = idx;
    checklist->items[idx].status        = OPENDASH_CHECK_PENDING;
    checklist->items[idx].assigned_node = node;
    checklist->items[idx].completed_at_ms = 0;

    strncpy(checklist->items[idx].name, name, OPENDASH_CHECKLIST_NAME_LEN - 1);
    checklist->items[idx].name[OPENDASH_CHECKLIST_NAME_LEN - 1] = '\0';

    checklist->count++;
    checklist->all_complete = false;

    return OPENDASH_OK;
}

/* ────────────────────────────────────────────────────────────────────────────
 * opendash_checklist_update
 * ──────────────────────────────────────────────────────────────────────────── */

opendash_err_t opendash_checklist_update(opendash_checklist_t *checklist,
                                          uint8_t item_id,
                                          opendash_check_status_t status,
                                          uint32_t timestamp_ms)
{
    if (checklist == NULL) {
        return OPENDASH_ERR_INVALID_ARG;
    }

    /* Find the item by ID */
    for (uint8_t i = 0; i < checklist->count; i++) {
        if (checklist->items[i].id == item_id) {
            checklist->items[i].status = status;

            /* Record completion timestamp if marking complete */
            if (status == OPENDASH_CHECK_COMPLETE) {
                checklist->items[i].completed_at_ms = timestamp_ms;
            }

            /* Re-evaluate overall completion */
            opendash_checklist_is_complete(checklist);

            return OPENDASH_OK;
        }
    }

    return OPENDASH_ERR_NOT_FOUND;
}

/* ────────────────────────────────────────────────────────────────────────────
 * opendash_checklist_reset
 * ──────────────────────────────────────────────────────────────────────────── */

opendash_err_t opendash_checklist_reset(opendash_checklist_t *checklist)
{
    if (checklist == NULL) {
        return OPENDASH_ERR_INVALID_ARG;
    }

    /* Reset all items to PENDING status */
    for (uint8_t i = 0; i < checklist->count; i++) {
        checklist->items[i].status          = OPENDASH_CHECK_PENDING;
        checklist->items[i].completed_at_ms = 0;
    }

    checklist->all_complete = false;

    return OPENDASH_OK;
}

/* ────────────────────────────────────────────────────────────────────────────
 * opendash_checklist_is_complete
 * ──────────────────────────────────────────────────────────────────────────── */

bool opendash_checklist_is_complete(opendash_checklist_t *checklist)
{
    if (checklist == NULL || checklist->count == 0) {
        return false;
    }

    /* Check every item — all must be COMPLETE or SKIPPED */
    for (uint8_t i = 0; i < checklist->count; i++) {
        if (checklist->items[i].status != OPENDASH_CHECK_COMPLETE &&
            checklist->items[i].status != OPENDASH_CHECK_SKIPPED) {
            checklist->all_complete = false;
            return false;
        }
    }

    checklist->all_complete = true;
    return true;
}

/* ────────────────────────────────────────────────────────────────────────────
 * opendash_checklist_save
 * ──────────────────────────────────────────────────────────────────────────── */

opendash_err_t opendash_checklist_save(const opendash_checklist_t *checklist)
{
    if (checklist == NULL) {
        return OPENDASH_ERR_INVALID_ARG;
    }

    /*
     * TODO: Implement NVS persistence.
     *
     * Future implementation:
     *   nvs_handle_t handle;
     *   esp_err_t err = nvs_open(OPENDASH_NVS_NAMESPACE, NVS_READWRITE, &handle);
     *   if (err == ESP_OK) {
     *       nvs_set_blob(handle, "checklist", checklist, sizeof(*checklist));
     *       nvs_commit(handle);
     *       nvs_close(handle);
     *   }
     */

    /* NVS persistence not yet implemented — return error so callers know */
    return OPENDASH_ERR_GENERAL;
}
