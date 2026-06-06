/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
#ifndef OPEN_DASH_LAYOUT_H
#define OPEN_DASH_LAYOUT_H

#include <stdint.h>
#include "opendash_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Screen layout structure for version 1
 * 
 * This structure defines how data points are arranged on displays.
 * It's used for both sending layouts over ESP-NOW and persisting them to NVS.
 */
typedef struct {
    uint8_t version;            /**< Layout version (0x01) */
    uint8_t mode;               /**< Display mode (0-7) */
    uint8_t slot_count;         /**< Number of slots (1-16) */
    uint16_t arc_dp_id;         /**< DP ID for arc gauge (0 if no arc) */
    float arc_min;              /**< Arc minimum value */
    float arc_max;              /**< Arc maximum value */
    uint16_t slot_dp_ids[16];   /**< DP IDs for each slot (16 max) */
} screen_layout_v1_t;

#ifdef __cplusplus
}
#endif

#endif // OPEN_DASH_LAYOUT_H