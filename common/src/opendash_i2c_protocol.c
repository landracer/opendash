/**
 * @file opendash_i2c_protocol.c
 * @brief OpenDash I2C Protocol — Implementation
 *
 * Implements message building, validation, serialization, and deserialization
 * for the OpenDash inter-node I2C communication protocol.
 *
 * @see opendash_i2c_protocol.h for the full API documentation.
 * @see docs/i2c-protocol.md for the protocol specification.
 */

#include "opendash_i2c_protocol.h"

/* ────────────────────────────────────────────────────────────────────────────
 * Internal Helper: Compute XOR Checksum
 *
 * The checksum is the XOR of all bytes from sync through the last payload
 * byte. This provides basic error detection for I2C transmission errors.
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Compute XOR checksum over message header and payload.
 *
 * @param[in] msg  Pointer to message with sync, cmd, length, and payload set.
 *
 * @return Computed checksum byte.
 */
static uint8_t compute_checksum(const opendash_i2c_msg_t *msg)
{
    uint8_t cs = 0;

    /* XOR the header fields: sync, command, and length */
    cs ^= msg->sync;
    cs ^= msg->cmd;
    cs ^= msg->length;

    /* XOR each payload byte */
    for (uint8_t i = 0; i < msg->length; i++) {
        cs ^= msg->payload[i];
    }

    return cs;
}

/* ────────────────────────────────────────────────────────────────────────────
 * opendash_i2c_build_msg
 * ──────────────────────────────────────────────────────────────────────────── */

opendash_err_t opendash_i2c_build_msg(opendash_i2c_msg_t *msg,
                                       uint8_t cmd,
                                       const uint8_t *payload,
                                       uint8_t length)
{
    /* Validate arguments */
    if (msg == NULL) {
        return OPENDASH_ERR_INVALID_ARG;
    }
    if (length > OPENDASH_MSG_MAX_PAYLOAD) {
        return OPENDASH_ERR_INVALID_ARG;
    }
    if (length > 0 && payload == NULL) {
        return OPENDASH_ERR_INVALID_ARG;
    }

    /* Set the sync byte — always 0xAA to mark a valid message start */
    msg->sync = OPENDASH_MSG_SYNC;

    /* Set the command ID */
    msg->cmd = cmd;

    /* Set the payload length */
    msg->length = length;

    /* Copy payload data into the message structure */
    if (length > 0) {
        memcpy(msg->payload, payload, length);
    }

    /* Compute and set the XOR checksum */
    msg->checksum = compute_checksum(msg);

    return OPENDASH_OK;
}

/* ────────────────────────────────────────────────────────────────────────────
 * opendash_i2c_validate_msg
 * ──────────────────────────────────────────────────────────────────────────── */

bool opendash_i2c_validate_msg(const opendash_i2c_msg_t *msg)
{
    if (msg == NULL) {
        return false;
    }

    /* Check that the sync byte is correct */
    if (msg->sync != OPENDASH_MSG_SYNC) {
        return false;
    }

    /* Verify payload length is within bounds */
    if (msg->length > OPENDASH_MSG_MAX_PAYLOAD) {
        return false;
    }

    /* Recompute checksum and compare with the stored value */
    uint8_t expected_cs = compute_checksum(msg);
    if (msg->checksum != expected_cs) {
        return false;
    }

    return true;
}

/* ────────────────────────────────────────────────────────────────────────────
 * opendash_i2c_serialize
 * ──────────────────────────────────────────────────────────────────────────── */

opendash_err_t opendash_i2c_serialize(const opendash_i2c_msg_t *msg,
                                       uint8_t *buffer,
                                       uint16_t *out_len)
{
    if (msg == NULL || buffer == NULL || out_len == NULL) {
        return OPENDASH_ERR_INVALID_ARG;
    }

    uint16_t idx = 0;

    /* Write header: sync, cmd, length */
    buffer[idx++] = msg->sync;
    buffer[idx++] = msg->cmd;
    buffer[idx++] = msg->length;

    /* Write payload */
    for (uint8_t i = 0; i < msg->length; i++) {
        buffer[idx++] = msg->payload[i];
    }

    /* Write checksum as the final byte */
    buffer[idx++] = msg->checksum;

    *out_len = idx;
    return OPENDASH_OK;
}

/* ────────────────────────────────────────────────────────────────────────────
 * opendash_i2c_deserialize
 * ──────────────────────────────────────────────────────────────────────────── */

opendash_err_t opendash_i2c_deserialize(const uint8_t *buffer,
                                         uint16_t length,
                                         opendash_i2c_msg_t *msg)
{
    if (buffer == NULL || msg == NULL) {
        return OPENDASH_ERR_INVALID_ARG;
    }

    /* Minimum message size: SYNC + CMD + LENGTH + CHECKSUM = 4 bytes */
    if (length < 4) {
        return OPENDASH_ERR_INVALID_ARG;
    }

    /* Parse header */
    msg->sync   = buffer[0];
    msg->cmd    = buffer[1];
    msg->length = buffer[2];

    /* Validate sync byte */
    if (msg->sync != OPENDASH_MSG_SYNC) {
        return OPENDASH_ERR_CHECKSUM;  /* Not a valid message */
    }

    /* Check that buffer is large enough for the declared payload + checksum */
    if (length < (uint16_t)(OPENDASH_MSG_HEADER_SIZE + msg->length + 1)) {
        return OPENDASH_ERR_INVALID_ARG;
    }

    /* Copy payload */
    if (msg->length > 0) {
        memcpy(msg->payload, &buffer[OPENDASH_MSG_HEADER_SIZE], msg->length);
    }

    /* Extract checksum (last byte after payload) */
    msg->checksum = buffer[OPENDASH_MSG_HEADER_SIZE + msg->length];

    /* Validate checksum */
    if (!opendash_i2c_validate_msg(msg)) {
        return OPENDASH_ERR_CHECKSUM;
    }

    return OPENDASH_OK;
}

/* ────────────────────────────────────────────────────────────────────────────
 * opendash_i2c_get_addr
 * ──────────────────────────────────────────────────────────────────────────── */

uint8_t opendash_i2c_get_addr(opendash_node_t node)
{
    switch (node) {
        case OPENDASH_NODE_LEFT:    return OPENDASH_I2C_ADDR_LEFT;
        case OPENDASH_NODE_RIGHT:   return OPENDASH_I2C_ADDR_RIGHT;
        case OPENDASH_NODE_GPS:     return OPENDASH_I2C_ADDR_GPS;
        case OPENDASH_NODE_BMS:     return OPENDASH_I2C_ADDR_BMS;
        case OPENDASH_NODE_CENTER:  return 0;  /* Center is the master, no slave address */
        default:                    return 0;  /* Invalid node */
    }
}
