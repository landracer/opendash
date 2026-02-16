/**
 * @file opendash_data_model.c
 * @brief OpenDash Data Model — Implementation
 *
 * Implements the central data store that holds all telemetry values.
 * The data store is a simple array of data point entries, searched
 * linearly by ID. This is efficient for the expected data set size
 * (< 128 entries) and avoids dynamic memory allocation.
 *
 * @see opendash_data_model.h for the full API documentation.
 * @see docs/data-points.md for the data point legend.
 */

#include "opendash_data_model.h"
#include <string.h>

/* ────────────────────────────────────────────────────────────────────────────
 * opendash_data_init
 * ──────────────────────────────────────────────────────────────────────────── */

opendash_err_t opendash_data_init(opendash_data_store_t *store)
{
    if (store == NULL) {
        return OPENDASH_ERR_INVALID_ARG;
    }

    /* Zero out the entire data store structure */
    memset(store, 0, sizeof(opendash_data_store_t));

    return OPENDASH_OK;
}

/* ────────────────────────────────────────────────────────────────────────────
 * Internal Helper: Find a data point by ID
 *
 * Performs a linear search through the data store. With a maximum of 128
 * entries, this is fast enough for real-time use (< 1 µs on ESP32-S3).
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Find the index of a data point by its ID.
 *
 * @param[in] store  Pointer to data store.
 * @param[in] id     Data point ID to search for.
 *
 * @return Index into the points array, or -1 if not found.
 */
static int find_index(const opendash_data_store_t *store, uint16_t id)
{
    for (uint16_t i = 0; i < store->count; i++) {
        if (store->points[i].id == id) {
            return (int)i;
        }
    }
    return -1;
}

/* ────────────────────────────────────────────────────────────────────────────
 * opendash_data_set
 * ──────────────────────────────────────────────────────────────────────────── */

opendash_err_t opendash_data_set(opendash_data_store_t *store,
                                  uint16_t id,
                                  float value,
                                  uint32_t timestamp_ms)
{
    if (store == NULL) {
        return OPENDASH_ERR_INVALID_ARG;
    }

    /* Check if this data point already exists in the store */
    int idx = find_index(store, id);

    if (idx >= 0) {
        /* Update existing entry */
        store->points[idx].value        = value;
        store->points[idx].timestamp_ms = timestamp_ms;
        store->points[idx].valid        = true;
    } else {
        /* Insert new entry if there is space */
        if (store->count >= OPENDASH_MAX_DATA_POINTS) {
            return OPENDASH_ERR_NO_MEM;
        }

        store->points[store->count].id           = id;
        store->points[store->count].value        = value;
        store->points[store->count].timestamp_ms = timestamp_ms;
        store->points[store->count].valid        = true;
        store->count++;
    }

    return OPENDASH_OK;
}

/* ────────────────────────────────────────────────────────────────────────────
 * opendash_data_get
 * ──────────────────────────────────────────────────────────────────────────── */

opendash_err_t opendash_data_get(const opendash_data_store_t *store,
                                  uint16_t id,
                                  float *out_value)
{
    if (store == NULL || out_value == NULL) {
        return OPENDASH_ERR_INVALID_ARG;
    }

    int idx = find_index(store, id);
    if (idx < 0) {
        return OPENDASH_ERR_NOT_FOUND;
    }

    *out_value = store->points[idx].value;
    return OPENDASH_OK;
}

/* ────────────────────────────────────────────────────────────────────────────
 * opendash_data_get_entry
 * ──────────────────────────────────────────────────────────────────────────── */

opendash_err_t opendash_data_get_entry(const opendash_data_store_t *store,
                                        uint16_t id,
                                        opendash_data_point_t *out)
{
    if (store == NULL || out == NULL) {
        return OPENDASH_ERR_INVALID_ARG;
    }

    int idx = find_index(store, id);
    if (idx < 0) {
        return OPENDASH_ERR_NOT_FOUND;
    }

    *out = store->points[idx];
    return OPENDASH_OK;
}
