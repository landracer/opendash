/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_dp_catalog.h
 * @brief OpenDash Data Point Catalog
 *
 * Static metadata for every assignable data point (DP). Used by the
 * per-node display configuration feature to populate UI pickers and to
 * supply sensible default arc min/max ranges when the user changes a slot.
 *
 * The catalog is *advisory*: it does not enforce which DPs a node can render,
 * it only provides the labels, units, and defaults a UI editor needs.
 *
 * @see PER_NODE_DISPLAY_CONFIG_SPEC.md §4.1
 * @see opendash_data_model.h — Source of OPENDASH_DP_* IDs
 */

#ifndef OPENDASH_DP_CATALOG_H
#define OPENDASH_DP_CATALOG_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief High-level grouping for the PID picker UI. */
typedef enum {
    OPENDASH_DP_CAT_ENGINE = 0,    /**< RPM, MAP, MAF, throttle */
    OPENDASH_DP_CAT_TEMP,          /**< coolant, oil, intake, EGT */
    OPENDASH_DP_CAT_PRESSURE,      /**< boost, oil, fuel pressure */
    OPENDASH_DP_CAT_FUEL,          /**< AFR, lambda, level, consumption */
    OPENDASH_DP_CAT_DRIVETRAIN,    /**< gear, vehicle speed, trans temp, IMU */
    OPENDASH_DP_CAT_GPS,           /**< speed, lat/lon, alt, fix, hdop, lap */
    OPENDASH_DP_CAT_BMS,           /**< pack volts, current, SOC, cell mV */
    OPENDASH_DP_CAT_VESC,          /**< motor temp, FET temp, duty, current */
    OPENDASH_DP_CAT_OBD,           /**< OBD-II PIDs not covered above */
    OPENDASH_DP_CAT_SYSTEM,        /**< battery V, ambient T, free heap, relays */
    OPENDASH_DP_CAT_COUNT
} opendash_dp_category_t;

/** @brief Per-DP metadata row. */
typedef struct {
    uint16_t    dp_id;          /**< OPENDASH_DP_* constant */
    const char *short_name;     /**< Human label (≤10 chars), e.g. "RPM" */
    const char *units;          /**< Units string, e.g. "rpm", "°C", "kPa" */
    float       default_min;    /**< Sensible arc minimum */
    float       default_max;    /**< Sensible arc maximum */
    uint8_t     category;       /**< opendash_dp_category_t */
    uint8_t     decimals;       /**< Display decimals (0,1,2,3) */
} opendash_dp_info_t;

/** @brief Full catalog table (sorted by dp_id ascending). */
extern const opendash_dp_info_t opendash_dp_catalog[];

/** @brief Number of entries in opendash_dp_catalog. */
extern const size_t             opendash_dp_catalog_count;

/**
 * @brief Look up catalog metadata for a DP ID.
 *
 * @param dp_id  16-bit OPENDASH_DP_* identifier.
 * @return Pointer to catalog row, or NULL if dp_id is unknown.
 */
const opendash_dp_info_t *opendash_dp_lookup(uint16_t dp_id);

#ifdef __cplusplus
}
#endif

#endif /* OPENDASH_DP_CATALOG_H */
