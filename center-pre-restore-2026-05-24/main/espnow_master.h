/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file espnow_master.h
 * @brief OpenDash Center Display — ESP-NOW Wireless Master Controller
 *
 * The Center display acts as the ESP-NOW master, broadcasting discovery
 * PINGs and pushing data to all slave nodes wirelessly.
 *
 * Replaces the previous I2C master (i2c_master.h) — no wires needed.
 *
 * @see opendash_espnow.h for the transport layer.
 * @see opendash_i2c_protocol.h for the message format (reused over ESP-NOW).
 */

#ifndef OPENDASH_ESPNOW_MASTER_H
#define OPENDASH_ESPNOW_MASTER_H

#include "esp_err.h"
#include "opendash_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Node online status, accessible from other modules. */
typedef struct {
    bool left_online;
    bool right_online;
    bool gps_online;
    bool bms_online;
} espnow_master_node_status_t;

/**
 * @brief Initialize ESP-NOW transport for the center master.
 *
 * Sets up WiFi STA + ESP-NOW. Must be called after nvs_flash_init().
 *
 * @return ESP_OK on success, or an error code.
 */
esp_err_t espnow_master_init(void);

/**
 * @brief Start the ESP-NOW master polling task.
 *
 * Spawns a FreeRTOS task that:
 *   1. Broadcasts discovery PINGs
 *   2. Pushes data point updates to online nodes
 *   3. Requests telemetry from GPS node
 *   4. Tracks node online/offline status
 *
 * @return ESP_OK on success, or an error code.
 */
esp_err_t espnow_master_start(void);

/**
 * @brief Get the current online status of all nodes.
 *
 * @param[out] status  Pointer to status structure to populate.
 */
void espnow_master_get_status(espnow_master_node_status_t *status);

/**
 * @brief Send a data point value to a specific node.
 *
 * @param[in] node   Target node.
 * @param[in] dp_id  Data point identifier.
 * @param[in] value  Float value to send.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if node is invalid.
 */
esp_err_t espnow_master_send_data_point(opendash_node_t node,
                                         uint16_t dp_id, float value);

/* ────────────────────────────────────────────────────────────────────────
 * Public helpers used by the boost controller / system config UI
 * ──────────────────────────────────────────────────────────────────────── */

#include "opendash_espnow.h"
#include "opendash_i2c_protocol.h"

/**
 * @brief Send a raw protocol frame to a node by enum.
 *
 * Unicasts when the node's MAC has been learned, broadcasts otherwise so
 * the slave can discover the master mid-flight.
 */
esp_err_t espnow_master_send_raw(opendash_node_t node, uint8_t cmd,
                                  const void *payload, uint16_t len);

/**
 * @brief True when @p node is currently considered online.
 */
bool espnow_master_node_online(opendash_node_t node);

/**
 * @brief Callback signature for frames the master doesn't recognise.
 *
 * Used by the boost controller to receive BOOST_TELEMETRY / *_REPORT
 * frames without modifying espnow_master.c's main switch.
 */
typedef void (*espnow_master_rx_cb_t)(const opendash_espnow_event_t *evt,
                                       const opendash_i2c_msg_t *msg);

/** @brief Register an auxiliary RX callback (single-slot). NULL clears. */
void espnow_master_set_aux_rx_callback(espnow_master_rx_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* OPENDASH_ESPNOW_MASTER_H */
