/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_bt_ota.h
 * @brief OpenDash BLE OTA (Over-The-Air) Firmware Update Service
 *
 * Provides a BLE GATT server that accepts firmware binary data from a
 * Web Bluetooth client (Chrome browser) and writes it to the ESP32's
 * OTA partition. Once the transfer is complete and verified, the device
 * reboots into the new firmware.
 *
 * Flow:
 *   1. Center sends OPENDASH_SUBCMD_ENTER_BT_OTA via ESP-NOW
 *   2. Node tears down ESP-NOW/WiFi, calls opendash_bt_ota_start()
 *   3. Node advertises as "OpenDash-<NODE>-OTA" via BLE
 *   4. Chrome Web Bluetooth app connects and sends firmware chunks
 *   5. On completion, node verifies checksum and reboots
 *
 * GATT Service Layout:
 *   Service UUID: 0x00FF (OpenDash OTA)
 *   ├─ Char 0xFF01: OTA Control (write) — commands: BEGIN, END, ABORT
 *   ├─ Char 0xFF02: OTA Data (write-no-response) — firmware chunks (512B max)
 *   └─ Char 0xFF03: OTA Status (notify) — progress, errors, version
 *
 * Security:
 *   - BLE OTA is ONLY activated by an explicit ESP-NOW command from center
 *   - 30-second advertising timeout (if no connection, revert to ESP-NOW)
 *   - Firmware image validated before activation (esp_ota_end())
 *   - Rollback to factory partition on boot failure (ESP-IDF built-in)
 *
 * @note Requires NimBLE stack (CONFIG_BT_NIMBLE_ENABLED=y)
 * @note WiFi/ESP-NOW MUST be stopped before calling opendash_bt_ota_start()
 *       because ESP32 shares the radio between WiFi and BLE.
 */

#ifndef OPENDASH_BT_OTA_H
#define OPENDASH_BT_OTA_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "opendash_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ────────────────────────────────────────────────────────────────────────────
 * OTA GATT UUIDs
 * ──────────────────────────────────────────────────────────────────────────── */

/** @brief OTA Service UUID (16-bit short). */
#define OPENDASH_BT_OTA_SVC_UUID        0x00FF

/** @brief OTA Control characteristic UUID. Write: BEGIN(0x01), END(0x02), ABORT(0x03). */
#define OPENDASH_BT_OTA_CTRL_UUID       0xFF01

/** @brief OTA Data characteristic UUID. Write-no-response: raw firmware bytes. */
#define OPENDASH_BT_OTA_DATA_UUID       0xFF02

/** @brief OTA Status characteristic UUID. Notify: [state:1][progress:1][error:1]. */
#define OPENDASH_BT_OTA_STATUS_UUID     0xFF03

/** @brief OTA Offset characteristic UUID. Read: [bytes_received:4 LE].
 *  Lets a reconnecting client query how many bytes the server already has
 *  so the session can resume instead of re-erasing and restarting at 0. */
#define OPENDASH_BT_OTA_OFFSET_UUID     0xFF04

/* ────────────────────────────────────────────────────────────────────────────
 * OTA Control Commands (written to CTRL characteristic)
 * ──────────────────────────────────────────────────────────────────────────── */

#define OPENDASH_OTA_CMD_BEGIN      0x01  /**< Erase OTA partition, prepare for data */
#define OPENDASH_OTA_CMD_END        0x02  /**< Finalize OTA, validate, set boot partition */
#define OPENDASH_OTA_CMD_ABORT      0x03  /**< Abort OTA, discard partial data */
#define OPENDASH_OTA_CMD_VERSION    0x04  /**< Request current firmware version */
#define OPENDASH_OTA_CMD_RESUME     0x05  /**< Resume in-flight session at given offset.
                                              Payload: [cmd:1][offset_LE:4] (5 bytes total). */

/* ────────────────────────────────────────────────────────────────────────────
 * OTA Status Codes (notified to STATUS characteristic)
 * ──────────────────────────────────────────────────────────────────────────── */

typedef enum {
    OPENDASH_OTA_STATE_IDLE       = 0x00,  /**< Not in OTA mode */
    OPENDASH_OTA_STATE_READY      = 0x01,  /**< Advertising, waiting for connection */
    OPENDASH_OTA_STATE_CONNECTED  = 0x02,  /**< Client connected */
    OPENDASH_OTA_STATE_RECEIVING  = 0x03,  /**< Receiving firmware data */
    OPENDASH_OTA_STATE_VERIFYING  = 0x04,  /**< Validating firmware image */
    OPENDASH_OTA_STATE_COMPLETE   = 0x05,  /**< OTA successful, will reboot */
    OPENDASH_OTA_STATE_ERROR      = 0xFF,  /**< Error occurred */
} opendash_ota_state_t;

/** @brief Maximum OTA data chunk size per BLE write (MTU-limited). */
#define OPENDASH_OTA_CHUNK_SIZE     512

/* ────────────────────────────────────────────────────────────────────────────
 * Public API
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Start the BLE OTA service.
 *
 * Initializes NimBLE, registers GATT services, and begins advertising.
 * WiFi/ESP-NOW MUST already be stopped (shared radio).
 *
 * @param[in] node  This node's type (used for BLE device name).
 * @return ESP_OK on success.
 *
 * @note This function blocks until OTA completes or times out.
 *       On success, the device reboots automatically.
 *       On timeout (30s no connection), returns to allow caller to
 *       re-init ESP-NOW.
 */
esp_err_t opendash_bt_ota_start(opendash_node_t node);

/**
 * @brief Check if BLE OTA is currently active.
 *
 * @return true if OTA service is running.
 */
bool opendash_bt_ota_is_active(void);

/**
 * @brief Abort any in-progress OTA and shut down BLE.
 *
 * @return ESP_OK on success.
 */
esp_err_t opendash_bt_ota_stop(void);

/**
 * @brief Convenience: notify center, tear down ESP-NOW, start BLE OTA, reboot.
 *
 * Single entry point used by every slave's ENTER_BT_OTA handler so that:
 *   - All nodes report `OPENDASH_STATUS_FLAG_BLE_OTA` back to center the same way
 *   - All nodes follow the same teardown order (status report → espnow_deinit → bt_ota_start)
 *   - Centralized restart-on-return guarantees clean recovery if BLE OTA times out
 *
 * Callers MUST perform any node-specific hardware-safe shutdown (relays off,
 * PWM off, persistent state flushed) BEFORE calling this function. This helper
 * only handles the transport/protocol/restart sequence.
 *
 * @param[in] self        This node's ID (used for BLE name and status report).
 * @param[in] center_mac  Center's MAC for status report. Pass NULL if unknown
 *                        (no status report sent — center will only see node go offline).
 * @return Does not return on success (esp_restart). Returns an error code only
 *         if NimBLE init fails before any restart attempt.
 */
esp_err_t opendash_bt_ota_enter(opendash_node_t self, const uint8_t center_mac[6]);

#ifdef __cplusplus
}
#endif

#endif /* OPENDASH_BT_OTA_H */
