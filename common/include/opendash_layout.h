/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_layout.h
 * @brief OpenDash Per-Node Screen Layout
 *
 * Defines the screen_layout_v1_t structure that is:
 *   - sent over ESP-NOW as the payload of OPENDASH_CMD_SET_SCREEN_LAYOUT
 *   - persisted in NVS by every node (namespace "od_layout", keys "m<mode>")
 *
 * Wire layout (PER_NODE_DISPLAY_CONFIG_SPEC.md §4.2):
 *   Byte  Field
 *    0    version          = 0x01
 *    1    mode             (display_mode_t value, 0..7)
 *    2    slot_count       (1..16)
 *    3-4  arc_dp_id        (uint16, big-endian; 0x0000 = no arc)
 *    5-8  arc_min          (float32, little-endian)
 *    9-12 arc_max          (float32, little-endian)
 *    13.. slot_dp_ids[]    (uint16 big-endian × slot_count)
 *
 * Total wire bytes: 13 + 2 * slot_count (max 45 for slot_count=16).
 *
 * In-memory representation uses host byte order for ergonomics; the
 * helpers in this header convert to/from the packed wire format.
 *
 * @see PER_NODE_DISPLAY_CONFIG_SPEC.md
 */

#ifndef OPENDASH_LAYOUT_H
#define OPENDASH_LAYOUT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Current layout struct version on the wire and in NVS. */
#define OPENDASH_LAYOUT_VERSION     0x01

/** @brief Maximum slots in a single layout (UI grid is 6, arc nodes 4). */
#define OPENDASH_LAYOUT_MAX_SLOTS   16

/** @brief Maximum serialized wire size in bytes. */
#define OPENDASH_LAYOUT_WIRE_MAX    (13 + 2 * OPENDASH_LAYOUT_MAX_SLOTS)

/**
 * @brief In-memory layout description (host byte order).
 *
 * Stored verbatim in NVS as a blob (host-endian). Converted to the packed
 * mixed-endian wire format by opendash_layout_serialize().
 */
typedef struct {
    uint8_t  version;                                  /**< OPENDASH_LAYOUT_VERSION */
    uint8_t  mode;                                     /**< Display mode (0..7) */
    uint8_t  slot_count;                               /**< 1..OPENDASH_LAYOUT_MAX_SLOTS */
    uint16_t arc_dp_id;                                /**< Arc DP, 0 if no arc */
    float    arc_min;                                  /**< Arc minimum */
    float    arc_max;                                  /**< Arc maximum */
    uint16_t slot_dp_ids[OPENDASH_LAYOUT_MAX_SLOTS];   /**< Per-slot DPs */
} screen_layout_v1_t;

/**
 * @brief Serialize a layout into the packed wire format.
 *
 * @param in       Layout to serialize. Must have valid version/mode/slot_count.
 * @param out_buf  Destination buffer (≥ OPENDASH_LAYOUT_WIRE_MAX bytes).
 * @param out_cap  Capacity of out_buf in bytes.
 * @param out_len  On success, receives 13 + 2 * slot_count.
 * @return 0 on success, -1 on invalid args / buffer too small.
 */
int opendash_layout_serialize(const screen_layout_v1_t *in,
                              uint8_t *out_buf, size_t out_cap,
                              size_t *out_len);

/**
 * @brief Parse a wire-format payload into an in-memory layout.
 *
 * @param buf      Wire bytes (starting at version).
 * @param len      Length of buf in bytes (must be ≥ 13).
 * @param out      Destination layout (host byte order).
 * @return 0 on success, -1 on malformed input or unsupported version.
 */
int opendash_layout_deserialize(const uint8_t *buf, size_t len,
                                screen_layout_v1_t *out);

#ifdef __cplusplus
}
#endif

#endif /* OPENDASH_LAYOUT_H */
