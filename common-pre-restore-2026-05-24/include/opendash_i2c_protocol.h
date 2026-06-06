/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_i2c_protocol.h
 * @brief OpenDash I2C Inter-Node Communication Protocol
 *
 * Defines the message format, command IDs, and node addresses for
 * communication between all OpenDash display nodes over the shared I2C bus.
 *
 * The Center display is always the I2C master. All other nodes are slaves.
 *
 * @par Protocol Format
 * Every message uses a fixed-header format:
 * | SYNC (0xAA) | CMD (1B) | LENGTH (1B) | PAYLOAD (0-248B) | CHECKSUM (1B) |
 *
 * @see docs/i2c-protocol.md for the full protocol specification.
 * @see ESP-IDF I2C Driver:
 *      https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32s3/api-reference/peripherals/i2c.html
 */

#ifndef OPENDASH_I2C_PROTOCOL_H
#define OPENDASH_I2C_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "opendash_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ────────────────────────────────────────────────────────────────────────────
 * I2C Bus Configuration
 * ──────────────────────────────────────────────────────────────────────────── */

/** @brief I2C port used for inter-node communication.
 *
 * The inter-node bus MUST use a different I2C port than the on-board I2C bus.
 * On the Waveshare 2.8C boards (left/right), PORT 0 is used for
 * the on-board TCA9554 + GT911 (SDA=GPIO15, SCL=GPIO7).
 * Therefore the inter-node bus uses PORT 1.
 *
 * On the Waveshare 4.3" center board, PORT 0 is used for GT911 touch
 * (SDA=GPIO19, SCL=GPIO20).  The inter-node bus also uses PORT 1.
 */
#define OPENDASH_I2C_PORT       1

/** @brief I2C clock speed in Hz (400 kHz = Fast Mode). */
#define OPENDASH_I2C_FREQ_HZ   400000

/** @brief GPIO pin for I2C SDA (inter-node bus, shared across all nodes).
 *
 * WARNING: On the Waveshare 2.8C boards (left/right), GPIO 15 is used by
 * the on-board TCA9554 I2C master bus.  DO NOT use GPIO 15 for the
 * inter-node bus on those boards — it will cause NACK errors on every
 * TCA9554 transaction due to EMI coupled from the long inter-board wire.
 *
 * GPIO 4 is available on the 2.8C boards as I2C_SLAVE_SDA, but is
 * hardwired to GT911 INT on the PCB.  Using GPIO 4 requires the GT911
 * to be held in permanent reset (EXIO2=LOW) to prevent bus interference.
 *
 * For production boards, a dedicated inter-node SDA pin should be used.
 * Current prototype wiring: GPIO 4 (left/right), GPIO 15 (center).
 */
#define OPENDASH_I2C_SDA_PIN   4

/** @brief GPIO pin for I2C SCL (inter-node bus, shared across all nodes). */
#define OPENDASH_I2C_SCL_PIN   16

/* ────────────────────────────────────────────────────────────────────────────
 * I2C Slave Addresses
 * ──────────────────────────────────────────────────────────────────────────── */

/** @brief I2C address of the Left gauge pod. */
#define OPENDASH_I2C_ADDR_LEFT  0x10

/** @brief I2C address of the Right gauge pod. */
#define OPENDASH_I2C_ADDR_RIGHT 0x11

/** @brief I2C address of the GPS/Telemetry unit. */
#define OPENDASH_I2C_ADDR_GPS   0x12

/** @brief I2C address of the external BMS node (rAtTrax).
 *  NOTE: 0x20 conflicts with the TCA9554 IO expander on the 2.8C boards.
 *  This is safe because the BMS is on the INTER-NODE bus (port 1) while
 *  the TCA9554 is on the ON-BOARD bus (port 0) — different physical buses.
 *  If the buses are ever bridged, change this address to avoid conflict. */
#define OPENDASH_I2C_ADDR_BMS   0x20

/* Expansion pod addresses (0x30–0x37) */
#define OPENDASH_I2C_ADDR_POD1  0x30    /**< Expansion pod 1 */
#define OPENDASH_I2C_ADDR_POD2  0x31    /**< Expansion pod 2 */
#define OPENDASH_I2C_ADDR_POD3  0x32    /**< Expansion pod 3 */
#define OPENDASH_I2C_ADDR_POD4  0x33    /**< Expansion pod 4 */
#define OPENDASH_I2C_ADDR_POD5  0x34    /**< Expansion pod 5 */
#define OPENDASH_I2C_ADDR_POD6  0x35    /**< Expansion pod 6 */
#define OPENDASH_I2C_ADDR_POD7  0x36    /**< Expansion pod 7 */
#define OPENDASH_I2C_ADDR_POD8  0x37    /**< Expansion pod 8 */

/* ────────────────────────────────────────────────────────────────────────────
 * Message Format Constants
 * ──────────────────────────────────────────────────────────────────────────── */

/** @brief Sync byte marking the start of every valid message. */
#define OPENDASH_MSG_SYNC       0xAA

/** @brief Maximum payload size in bytes. */
#define OPENDASH_MSG_MAX_PAYLOAD 248

/** @brief Message header size (SYNC + CMD + LENGTH). */
#define OPENDASH_MSG_HEADER_SIZE 3

/** @brief Total maximum message size (header + max payload + checksum). */
#define OPENDASH_MSG_MAX_SIZE   (OPENDASH_MSG_HEADER_SIZE + OPENDASH_MSG_MAX_PAYLOAD + 1)

/* ────────────────────────────────────────────────────────────────────────────
 * Command IDs — Master → Slave
 * ──────────────────────────────────────────────────────────────────────────── */

/** @brief Send a data point value for display. Payload: [dp_id:2][value:4]. */
#define OPENDASH_CMD_SET_DATA_POINT     0x01

/** @brief Configure screen layout. Payload: [section:1][dp_id:2]... */
#define OPENDASH_CMD_SET_SCREEN_LAYOUT  0x02

/** @brief Set alarm thresholds. Payload: [dp_id:2][lo:4][hi:4][flags:1]. */
#define OPENDASH_CMD_SET_ALARM          0x03

/** @brief Set display brightness. Payload: [level:1] (0-255). */
#define OPENDASH_CMD_SET_BRIGHTNESS     0x04

/** @brief Update checklist item. Payload: [item_id:1][status:1]. */
#define OPENDASH_CMD_CHECKLIST_UPDATE   0x05

/** @brief Request a data point from slave. Payload: [dp_id:2]. */
#define OPENDASH_CMD_REQUEST_DATA       0x06

/** @brief System command. Payload: [subcmd:1][params...]. */
#define OPENDASH_CMD_SYSTEM             0x07

/* ── Relay / MOS control (master→slave) ── */
/** @brief Set relay/MOS channel(s).
 *  MASK FORMAT (preferred): [0xCA, node_id, mask, seq]
 *  LEGACY FORMAT: [channel, state, pwm_duty?] (3rd byte optional, 0..255). */
#define OPENDASH_CMD_SET_RELAY            0x10
/** @brief Request current relay/MOS state. Payload: []. */
#define OPENDASH_CMD_REQUEST_RELAY_STATUS 0x11

/* ──────────────────────────────────────────────────────────────────────────
 * Boost controller — Master → Slave (0x20…0x2F reserved for boost authoring)
 * ──────────────────────────────────────────────────────────────────────── */

/** @brief Push live engine snapshot to slave. Payload: opendash_boost_live_t. */
#define OPENDASH_CMD_BOOST_LIVE_DATA      0x20
/** @brief Replace scalar params. Payload: opendash_boost_params_t. */
#define OPENDASH_CMD_BOOST_SET_PARAMS     0x21
/** @brief Replace a single duty row. Payload: opendash_boost_duty_row_t. */
#define OPENDASH_CMD_BOOST_SET_DUTY_ROW   0x22
/** @brief Replace a single setpoint row. Payload: opendash_boost_setpoint_row_t. */
#define OPENDASH_CMD_BOOST_SET_SETP_ROW   0x23
/** @brief Replace the throttle reduction curve. Payload: opendash_boost_throttle_curve_t. */
#define OPENDASH_CMD_BOOST_SET_THROTTLE   0x24
/** @brief Set active mode quickly. Payload: [mode:1] (opendash_boost_mode_t). */
#define OPENDASH_CMD_BOOST_SET_MODE       0x25
/** @brief Ask slave to emit one telemetry frame immediately. Payload: []. */
#define OPENDASH_CMD_BOOST_TELEMETRY_REQ  0x26
/** @brief Ask slave to dump params + all rows. Payload: []. */
#define OPENDASH_CMD_BOOST_PULL_ALL       0x27
/** @brief Request one duty row. Payload: [mode:1][gear:1]. */
#define OPENDASH_CMD_BOOST_GET_DUTY_ROW   0x28
/** @brief Request one setpoint row. Payload: [mode:1][gear:1]. */
#define OPENDASH_CMD_BOOST_GET_SETP_ROW   0x29
/** @brief Request the throttle reduction curve. Payload: []. */
#define OPENDASH_CMD_BOOST_GET_THROTTLE   0x2A

/* ────────────────────────────────────────────────────────────────────────────
 * Command IDs — Slave → Master (Responses)
 * ──────────────────────────────────────────────────────────────────────────── */

/** @brief Data response. Payload: [dp_id:2][value:4][timestamp:4]. */
#define OPENDASH_CMD_DATA_RESPONSE      0x81

/** @brief Node status report. Payload: [node_id:1][flags:2]. */
#define OPENDASH_CMD_STATUS_REPORT      0x82

/** @brief Checklist status. Payload: [item_id:1][status:1]. */
#define OPENDASH_CMD_CHECKLIST_STATUS   0x83

/** @brief Alarm triggered notification. Payload: [dp_id:2][value:4]. */
#define OPENDASH_CMD_ALARM_TRIGGERED    0x84

/** @brief Relay/MOS channel state report.
 *  MASK FORMAT: [0xCA, node_id, mask]. */
#define OPENDASH_CMD_RELAY_STATUS       0x85

/* ── Boost subsystem — Slave → Master (0x90…0x9F reserved) ── */

/** @brief Periodic boost telemetry. Payload: opendash_boost_telemetry_t. */
#define OPENDASH_CMD_BOOST_TELEMETRY      0x90
/** @brief Echo of current params. Payload: opendash_boost_params_t. */
#define OPENDASH_CMD_BOOST_PARAMS_REPORT  0x91
/** @brief Echo of a duty row. Payload: opendash_boost_duty_row_t. */
#define OPENDASH_CMD_BOOST_DUTY_REPORT    0x92
/** @brief Echo of a setpoint row. Payload: opendash_boost_setpoint_row_t. */
#define OPENDASH_CMD_BOOST_SETP_REPORT    0x93
/** @brief Echo of the throttle curve. Payload: opendash_boost_throttle_curve_t. */
#define OPENDASH_CMD_BOOST_THROTTLE_REPORT 0x94

/** @brief Negative acknowledgment (error). Payload: [error_code:1]. */
#define OPENDASH_CMD_NAK                0xFF

/* ────────────────────────────────────────────────────────────────────────────
 * System Sub-Commands (used with OPENDASH_CMD_SYSTEM)
 * ──────────────────────────────────────────────────────────────────────────── */

#define OPENDASH_SUBCMD_REBOOT          0x01  /**< Reboot the node */
#define OPENDASH_SUBCMD_OTA_START       0x02  /**< Prepare for OTA update */
#define OPENDASH_SUBCMD_FACTORY_RESET   0x03  /**< Reset all settings to defaults */
#define OPENDASH_SUBCMD_PING            0x04  /**< Ping / heartbeat */
#define OPENDASH_SUBCMD_TIME_SYNC       0x05  /**< Time sync from GPS. Payload: [subcmd][hour][min][sec][day][month][year_lo][year_hi][fix_valid] */
#define OPENDASH_SUBCMD_ENTER_BT_OTA    0x06  /**< Tear down ESP-NOW and start BLE OTA service */

/* ────────────────────────────────────────────────────────────────────────────
 * Message Structure
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief I2C message structure.
 *
 * All inter-node messages use this structure. The checksum field is
 * automatically computed when building a message and verified on receipt.
 */
typedef struct {
    uint8_t  sync;                                  /**< Always OPENDASH_MSG_SYNC (0xAA) */
    uint8_t  cmd;                                   /**< Command ID */
    uint8_t  length;                                /**< Payload length in bytes */
    uint8_t  payload[OPENDASH_MSG_MAX_PAYLOAD];     /**< Message payload */
    uint8_t  checksum;                              /**< XOR of sync, cmd, length, and all payload bytes */
} opendash_i2c_msg_t;

/* ────────────────────────────────────────────────────────────────────────────
 * Protocol Functions
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Build an I2C message with the correct sync byte and checksum.
 *
 * Populates the message structure with the given command and payload,
 * then computes and sets the checksum field.
 *
 * @param[out] msg      Pointer to message structure to populate.
 * @param[in]  cmd      Command ID byte.
 * @param[in]  payload  Pointer to payload data (can be NULL if length is 0).
 * @param[in]  length   Payload length in bytes (0 to OPENDASH_MSG_MAX_PAYLOAD).
 *
 * @return OPENDASH_OK on success, OPENDASH_ERR_INVALID_ARG if parameters are invalid.
 */
opendash_err_t opendash_i2c_build_msg(opendash_i2c_msg_t *msg,
                                       uint8_t cmd,
                                       const uint8_t *payload,
                                       uint8_t length);

/**
 * @brief Validate a received I2C message.
 *
 * Checks the sync byte and verifies the checksum.
 *
 * @param[in] msg  Pointer to the received message structure.
 *
 * @return true if the message is valid, false otherwise.
 */
bool opendash_i2c_validate_msg(const opendash_i2c_msg_t *msg);

/**
 * @brief Serialize a message into a byte buffer for I2C transmission.
 *
 * Converts the message structure into a contiguous byte array suitable
 * for sending over the I2C bus.
 *
 * @param[in]  msg      Pointer to the message to serialize.
 * @param[out] buffer   Output byte buffer (must be at least OPENDASH_MSG_MAX_SIZE bytes).
 * @param[out] out_len  Number of bytes written to buffer.
 *
 * @return OPENDASH_OK on success, OPENDASH_ERR_INVALID_ARG if parameters are invalid.
 */
opendash_err_t opendash_i2c_serialize(const opendash_i2c_msg_t *msg,
                                       uint8_t *buffer,
                                       uint16_t *out_len);

/**
 * @brief Deserialize a byte buffer into a message structure.
 *
 * Parses a received byte buffer and populates the message structure.
 * Also validates sync byte and checksum.
 *
 * @param[in]  buffer   Input byte buffer.
 * @param[in]  length   Number of bytes in buffer.
 * @param[out] msg      Pointer to message structure to populate.
 *
 * @return OPENDASH_OK on success, OPENDASH_ERR_CHECKSUM if checksum fails,
 *         OPENDASH_ERR_INVALID_ARG if parameters are invalid.
 */
opendash_err_t opendash_i2c_deserialize(const uint8_t *buffer,
                                         uint16_t length,
                                         opendash_i2c_msg_t *msg);

/**
 * @brief Get the I2C slave address for a given node type.
 *
 * @param[in] node  Node type enumeration value.
 *
 * @return I2C address (7-bit), or 0 if node is the master (Center) or invalid.
 */
uint8_t opendash_i2c_get_addr(opendash_node_t node);

#ifdef __cplusplus
}
#endif

#endif /* OPENDASH_I2C_PROTOCOL_H */
