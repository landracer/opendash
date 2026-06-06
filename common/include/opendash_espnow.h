/**
 * @file opendash_espnow.h
 * @brief OpenDash ESP-NOW Wireless Transport Layer
 *
 * Provides peer-to-peer wireless communication between all OpenDash nodes
 * using ESP-NOW (WiFi direct, no router required).
 *
 * Key advantages over wired I2C:
 *   - Zero additional wires between boards
 *   - No GPIO pin conflicts with on-board peripherals
 *   - Low latency (~1-5 ms typical)
 *   - Up to 250 bytes per frame (fits all protocol messages)
 *   - Built into every ESP32-S3
 *
 * Architecture:
 *   - Center node broadcasts discovery PINGs and sends data to known peers
 *   - Peripheral nodes (Left, Right, GPS) listen and respond
 *   - Thread-safe receive queue decouples WiFi callback from app logic
 *
 * @see esp_now.h — ESP-IDF ESP-NOW API
 * @see opendash_i2c_protocol.h — Message format (unchanged, reused as payload)
 *
 * Licensed under Sovereign Individual License v1.0 — see LICENSE file
 */

#ifndef OPENDASH_ESPNOW_H
#define OPENDASH_ESPNOW_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "opendash_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ────────────────────────────────────────────────────────────────────────────
 * Constants
 * ──────────────────────────────────────────────────────────────────────────── */

/** @brief Maximum ESP-NOW payload (hardware limit). */
#define OPENDASH_ESPNOW_MAX_DATA    250

/** @brief Inbound message queue depth. */
#define OPENDASH_ESPNOW_QUEUE_SIZE  32

/** @brief WiFi channel used by all OpenDash nodes (must match). */
#define OPENDASH_ESPNOW_CHANNEL     1

/** @brief Broadcast MAC address constant. */
extern const uint8_t OPENDASH_ESPNOW_BROADCAST[6];

/* ────────────────────────────────────────────────────────────────────────────
 * Types
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Received ESP-NOW message event.
 *
 * Queued by the WiFi callback for safe processing on the app task.
 */
typedef struct {
    uint8_t  src_mac[6];                        /**< Sender's WiFi MAC */
    uint8_t  data[OPENDASH_ESPNOW_MAX_DATA];    /**< Raw payload bytes */
    int      len;                                /**< Payload length */
    int      rssi;                               /**< Signal strength (dBm) */
} opendash_espnow_event_t;

/* ────────────────────────────────────────────────────────────────────────────
 * Initialization
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Initialize WiFi (STA, no AP connection) and ESP-NOW.
 *
 * Creates the receive queue and registers send/receive callbacks.
 * Must be called AFTER nvs_flash_init() (WiFi needs NVS for cal data).
 *
 * @param[in] self_node  This node's type (for logging).
 * @return ESP_OK on success.
 */
esp_err_t opendash_espnow_init(opendash_node_t self_node);

/**
 * @brief Shut down ESP-NOW and WiFi.
 */
esp_err_t opendash_espnow_deinit(void);

/* ────────────────────────────────────────────────────────────────────────────
 * Sending
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Send data to a specific peer.
 *
 * The peer must first be registered with opendash_espnow_add_peer().
 *
 * @param[in] dst_mac  6-byte destination MAC address.
 * @param[in] data     Payload bytes (typically a serialized protocol message).
 * @param[in] len      Payload length (max OPENDASH_ESPNOW_MAX_DATA).
 * @return ESP_OK on success, ESP_ERR_INVALID_SIZE if too large.
 */
esp_err_t opendash_espnow_send(const uint8_t *dst_mac,
                                const uint8_t *data, size_t len);

/**
 * @brief Broadcast data to all peers.
 *
 * Uses the broadcast MAC address (FF:FF:FF:FF:FF:FF).
 * All ESP-NOW nodes on the same channel will receive the message.
 *
 * @param[in] data  Payload bytes.
 * @param[in] len   Payload length.
 * @return ESP_OK on success.
 */
esp_err_t opendash_espnow_broadcast(const uint8_t *data, size_t len);

/* ────────────────────────────────────────────────────────────────────────────
 * Receiving
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Receive the next message from the inbound queue.
 *
 * Messages are queued by the WiFi callback for safe app-level processing.
 *
 * @param[out] evt      Event struct to populate.
 * @param[in]  wait_ms  Max wait time (0 = non-blocking, portMAX_DELAY = forever).
 * @return true if a message was received, false on timeout.
 */
bool opendash_espnow_recv(opendash_espnow_event_t *evt, uint32_t wait_ms);

/* ────────────────────────────────────────────────────────────────────────────
 * Peer Management
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Register a peer for unicast communication.
 *
 * Idempotent — safe to call multiple times with the same MAC.
 * Peers are automatically discovered via broadcast and added.
 *
 * @param[in] mac  6-byte peer MAC address.
 * @return ESP_OK on success or if peer already exists.
 */
esp_err_t opendash_espnow_add_peer(const uint8_t *mac);

/* ────────────────────────────────────────────────────────────────────────────
 * Utility
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Get this node's WiFi STA MAC address.
 *
 * @param[out] mac_out  6-byte buffer to receive the MAC.
 */
void opendash_espnow_get_mac(uint8_t *mac_out);

/* ────────────────────────────────────────────────────────────────────────────
 * Send Status Hook (for node health ACK tracking)
 *
 * The master can register a callback to receive MAC-layer ACK/NACK results.
 * This enables Layer 2 of the node health system: if a send to a peer is
 * ACKed by the hardware, we know the peer's radio is alive.
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Send status callback type.
 *
 * @param mac      Destination MAC that was sent to.
 * @param success  true if MAC-layer ACK received, false if no ACK.
 */
typedef void (*opendash_espnow_send_status_cb_t)(const uint8_t *mac, bool success);

/**
 * @brief Register a callback for send status notifications.
 *
 * Only one callback can be registered at a time (center master only).
 * Set to NULL to disable.
 *
 * @param cb  Callback function, or NULL to unregister.
 */
void opendash_espnow_set_send_status_cb(opendash_espnow_send_status_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* OPENDASH_ESPNOW_H */
