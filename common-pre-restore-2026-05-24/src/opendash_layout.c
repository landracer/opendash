/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_layout.c
 * @brief Pack/unpack helpers for screen_layout_v1_t wire format.
 *
 * @see PER_NODE_DISPLAY_CONFIG_SPEC.md §4.2
 */

#include "opendash_layout.h"
#include <string.h>

/* ESP32 is little-endian, so floats can be memcpy'd directly. DP IDs
 * are emitted big-endian on the wire to match the convention used by
 * SET_DATA_POINT (payload[0] = hi, payload[1] = lo). */

int opendash_layout_serialize(const screen_layout_v1_t *in,
                              uint8_t *out_buf, size_t out_cap,
                              size_t *out_len)
{
    if (!in || !out_buf || !out_len) {
        return -1;
    }
    if (in->slot_count > OPENDASH_LAYOUT_MAX_SLOTS) {
        return -1;
    }

    size_t need = 13u + 2u * (size_t)in->slot_count;
    if (out_cap < need) {
        return -1;
    }

    out_buf[0] = in->version;
    out_buf[1] = in->mode;
    out_buf[2] = in->slot_count;
    out_buf[3] = (uint8_t)((in->arc_dp_id >> 8) & 0xFF);
    out_buf[4] = (uint8_t)(in->arc_dp_id & 0xFF);
    memcpy(&out_buf[5],  &in->arc_min, sizeof(float));
    memcpy(&out_buf[9],  &in->arc_max, sizeof(float));

    for (uint8_t i = 0; i < in->slot_count; i++) {
        out_buf[13 + i * 2 + 0] = (uint8_t)((in->slot_dp_ids[i] >> 8) & 0xFF);
        out_buf[13 + i * 2 + 1] = (uint8_t)(in->slot_dp_ids[i] & 0xFF);
    }

    *out_len = need;
    return 0;
}

int opendash_layout_deserialize(const uint8_t *buf, size_t len,
                                screen_layout_v1_t *out)
{
    if (!buf || !out || len < 13u) {
        return -1;
    }

    uint8_t version    = buf[0];
    uint8_t mode       = buf[1];
    uint8_t slot_count = buf[2];

    if (version != OPENDASH_LAYOUT_VERSION) {
        return -1;
    }
    if (slot_count > OPENDASH_LAYOUT_MAX_SLOTS) {
        return -1;
    }
    if (len < (13u + 2u * (size_t)slot_count)) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->version    = version;
    out->mode       = mode;
    out->slot_count = slot_count;
    out->arc_dp_id  = (uint16_t)((uint16_t)buf[3] << 8) | (uint16_t)buf[4];
    memcpy(&out->arc_min, &buf[5], sizeof(float));
    memcpy(&out->arc_max, &buf[9], sizeof(float));

    for (uint8_t i = 0; i < slot_count; i++) {
        out->slot_dp_ids[i] =
            (uint16_t)((uint16_t)buf[13 + i * 2 + 0] << 8) |
            (uint16_t)buf[13 + i * 2 + 1];
    }

    return 0;
}
