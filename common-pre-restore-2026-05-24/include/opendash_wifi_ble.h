/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_wifi_ble.h
 * @brief OpenDash WiFi and BLE Connectivity Manager
 *
 * Each OpenDash node can independently control its WiFi or BLE mode.
 * This is used for:
 * - OTA (Over-The-Air) firmware updates via WiFi
 * - Data transfer to the companion Android/iOS app
 * - Configuration changes from the companion app
 * - Wireless data download after a session
 *
 * Only one wireless mode (WiFi or BLE) is active at a time to conserve
 * resources. The mode can be changed via touch menu or I2C command.
 *
 * @see ESP-IDF WiFi API:
 *      https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32s3/api-reference/network/esp_wifi.html
 * @see ESP-IDF Bluetooth API:
 *      https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32s3/api-reference/bluetooth/index.html
 */

#ifndef OPENDASH_WIFI_BLE_H
#define OPENDASH_WIFI_BLE_H

#include <stdint.h>
#include <stdbool.h>
#include "opendash_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ────────────────────────────────────────────────────────────────────────────
 * Wireless Mode
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Wireless operating mode.
 *
 * Each node runs in exactly one mode at a time. DISABLED is the default
 * during racing to avoid any wireless interference or CPU overhead.
 */
typedef enum {
    OPENDASH_WIRELESS_DISABLED = 0,  /**< All wireless off (default during racing) */
    OPENDASH_WIRELESS_WIFI_AP  = 1,  /**< WiFi Access Point mode (for OTA/config) */
    OPENDASH_WIRELESS_WIFI_STA = 2,  /**< WiFi Station mode (connect to router) */
    OPENDASH_WIRELESS_BLE      = 3,  /**< BLE mode (for companion app sync) */
} opendash_wireless_mode_t;

/* ────────────────────────────────────────────────────────────────────────────
 * WiFi Configuration
 * ──────────────────────────────────────────────────────────────────────────── */

/** @brief Maximum SSID length. */
#define OPENDASH_WIFI_SSID_MAX      32

/** @brief Maximum password length. */
#define OPENDASH_WIFI_PASS_MAX      64

/**
 * @brief WiFi configuration structure.
 *
 * Stored in NVS for persistence across power cycles.
 */
typedef struct {
    char     ssid[OPENDASH_WIFI_SSID_MAX];      /**< WiFi SSID */
    char     password[OPENDASH_WIFI_PASS_MAX];   /**< WiFi password */
    uint8_t  channel;                            /**< WiFi channel (AP mode, 0=auto) */
} opendash_wifi_config_t;

/* ────────────────────────────────────────────────────────────────────────────
 * BLE Configuration
 * ──────────────────────────────────────────────────────────────────────────── */

/** @brief Maximum BLE device name length. */
#define OPENDASH_BLE_NAME_MAX   24

/**
 * @brief BLE configuration structure.
 */
typedef struct {
    char device_name[OPENDASH_BLE_NAME_MAX];  /**< BLE advertised device name */
} opendash_ble_config_t;

/* ────────────────────────────────────────────────────────────────────────────
 * Wireless Manager API
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Initialize the wireless manager.
 *
 * Sets up NVS-backed configuration but does NOT start any wireless mode.
 * Call opendash_wireless_set_mode() to activate WiFi or BLE.
 *
 * @param[in] node  This node's type (used for default device naming).
 *
 * @return OPENDASH_OK on success.
 *
 * @note Internally calls esp_netif_init() and esp_event_loop_create_default().
 * @see https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32s3/api-reference/network/esp_netif.html
 */
opendash_err_t opendash_wireless_init(opendash_node_t node);

/**
 * @brief Set the wireless operating mode.
 *
 * Stops any currently active wireless mode and starts the new one.
 * Setting DISABLED stops all wireless activity.
 *
 * @param[in] mode  Desired wireless mode.
 *
 * @return OPENDASH_OK on success, error code on failure.
 *
 * @note Mode transitions:
 *       - DISABLED → WIFI_AP: Starts WiFi AP with configured SSID/password
 *       - DISABLED → BLE: Starts BLE advertising
 *       - WIFI_AP → DISABLED: Stops WiFi AP
 *       - BLE → DISABLED: Stops BLE advertising
 */
opendash_err_t opendash_wireless_set_mode(opendash_wireless_mode_t mode);

/**
 * @brief Get the current wireless operating mode.
 *
 * @return Current opendash_wireless_mode_t.
 */
opendash_wireless_mode_t opendash_wireless_get_mode(void);

/**
 * @brief Update WiFi configuration.
 *
 * Saves the new configuration to NVS. If WiFi is currently active,
 * it will be restarted with the new settings.
 *
 * @param[in] config  Pointer to new WiFi configuration.
 *
 * @return OPENDASH_OK on success.
 */
opendash_err_t opendash_wireless_set_wifi_config(const opendash_wifi_config_t *config);

/**
 * @brief Update BLE configuration.
 *
 * @param[in] config  Pointer to new BLE configuration.
 *
 * @return OPENDASH_OK on success.
 */
opendash_err_t opendash_wireless_set_ble_config(const opendash_ble_config_t *config);

/**
 * @brief Check if an OTA update is available/in-progress.
 *
 * @return true if OTA is in progress, false otherwise.
 */
bool opendash_wireless_ota_in_progress(void);

/**
 * @brief Deinitialize the wireless manager and free all resources.
 */
void opendash_wireless_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENDASH_WIFI_BLE_H */
