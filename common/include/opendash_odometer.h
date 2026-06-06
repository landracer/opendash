/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_odometer.h
 * @brief OpenDash Odometer & Trip Meter System
 *
 * Tracks total distance (odometer) and resettable trip distance.
 * Data is stored in NVS so odometer survives power cycles.
 *
 * Distance sources (in priority order):
 *   1. Wheel RPM sensor (most accurate, requires calibration)
 *   2. GPS distance traveled (good accuracy at speed, poor at low speed)
 *
 * All internal values are stored in METERS for precision.
 * Display conversion to km/mi happens at the UI layer via
 * opendash_convert_distance() in opendash_ui_styles.h.
 *
 * NVS Storage Layout:
 *   Key "odo_total"  — uint32_t total distance in meters
 *   Key "odo_trip_a" — uint32_t trip A distance in meters
 *   Key "odo_trip_b" — uint32_t trip B distance in meters
 *
 * @note NVS writes are rate-limited to avoid flash wear.
 *       Data is committed every ODOMETER_NVS_SAVE_INTERVAL_M meters.
 *
 * @see opendash_ui_styles.h for unit conversion to km/miles
 * @see opendash_data_model.h for OPENDASH_DP_GPS_SPEED data point
 */

#ifndef OPENDASH_ODOMETER_H
#define OPENDASH_ODOMETER_H

#include <stdint.h>
#include <stdbool.h>
#include "opendash_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ────────────────────────────────────────────────────────────────────────────
 * Configuration
 * ──────────────────────────────────────────────────────────────────────────── */

/** @brief NVS namespace for odometer storage. */
#define ODOMETER_NVS_NAMESPACE      "od_odo"

/** @brief Save to NVS every N meters to reduce flash wear (default 100m). */
#define ODOMETER_NVS_SAVE_INTERVAL_M  100

/* ────────────────────────────────────────────────────────────────────────────
 * Data Types
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Distance source type
 */
typedef enum {
    ODOMETER_SOURCE_GPS        = 0,  /**< GPS speed × time integration */
    ODOMETER_SOURCE_WHEEL_RPM  = 1,  /**< Wheel RPM sensor (future) */
} odometer_source_t;

/**
 * @brief Odometer state (in-memory, synced to NVS periodically)
 */
typedef struct {
    uint32_t total_meters;           /**< Odometer: total lifetime distance (m) */
    uint32_t trip_a_meters;          /**< Trip A: resettable distance (m) */
    uint32_t trip_b_meters;          /**< Trip B: second resettable distance (m) */
    uint32_t last_nvs_save_meters;   /**< Total meters at last NVS write */
    bool     initialized;            /**< true after opendash_odometer_init() */
} opendash_odometer_t;

/* ────────────────────────────────────────────────────────────────────────────
 * Public API
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Initialize the odometer system.
 *
 * Loads saved values from NVS. If no data exists (first boot),
 * initializes everything to zero.
 *
 * @param[out] odo  Pointer to odometer state structure.
 * @return OPENDASH_OK on success.
 */
opendash_err_t opendash_odometer_init(opendash_odometer_t *odo);

/**
 * @brief Add distance traveled.
 *
 * Call this periodically from the main loop with the distance
 * traveled since the last call. All three counters (total, trip A,
 * trip B) are incremented. NVS is written when enough distance
 * has accumulated.
 *
 * @param[in,out] odo      Pointer to odometer state.
 * @param[in]     meters   Distance traveled since last call (meters).
 * @return OPENDASH_OK on success.
 */
opendash_err_t opendash_odometer_add_distance(opendash_odometer_t *odo,
                                               uint32_t meters);

/**
 * @brief Reset trip A counter to zero.
 *
 * @param[in,out] odo  Pointer to odometer state.
 * @return OPENDASH_OK on success.
 */
opendash_err_t opendash_odometer_reset_trip_a(opendash_odometer_t *odo);

/**
 * @brief Reset trip B counter to zero.
 *
 * @param[in,out] odo  Pointer to odometer state.
 * @return OPENDASH_OK on success.
 */
opendash_err_t opendash_odometer_reset_trip_b(opendash_odometer_t *odo);

/**
 * @brief Force an immediate NVS save.
 *
 * Normally NVS saves are rate-limited. Call this before shutdown
 * or on user request to ensure no data is lost.
 *
 * @param[in] odo  Pointer to odometer state.
 * @return OPENDASH_OK on success.
 */
opendash_err_t opendash_odometer_save_now(const opendash_odometer_t *odo);

/**
 * @brief Get total odometer in kilometers (float).
 */
static inline float opendash_odometer_get_km(const opendash_odometer_t *odo)
{
    return (odo != NULL) ? (float)odo->total_meters / 1000.0f : 0.0f;
}

/**
 * @brief Get trip A distance in kilometers (float).
 */
static inline float opendash_odometer_get_trip_a_km(const opendash_odometer_t *odo)
{
    return (odo != NULL) ? (float)odo->trip_a_meters / 1000.0f : 0.0f;
}

/**
 * @brief Get trip B distance in kilometers (float).
 */
static inline float opendash_odometer_get_trip_b_km(const opendash_odometer_t *odo)
{
    return (odo != NULL) ? (float)odo->trip_b_meters / 1000.0f : 0.0f;
}

#ifdef __cplusplus
}
#endif

#endif /* OPENDASH_ODOMETER_H */
