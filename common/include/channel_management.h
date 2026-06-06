/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file channel_management.h
 * @brief Channel dispatcher and node registry for event-driven ESP-NOW
 *
 * The channel manager is the core routing engine of the new architecture.
 * It replaces the old PING-based polling loop with:
 *
 *   1. Per-channel inbound queues (filled by the ESP-NOW receive callback)
 *   2. A node registry (populated by ANNOUNCE messages at boot)
 *   3. Offline detection via data-absence timeout (no heartbeat needed)
 *   4. Delta tracking for data points (only forward changed values)
 *
 * ARCHITECTURE RULE: NO POLLING / NO PINGING.
 */

#ifndef CHANNEL_MANAGEMENT_H
#define CHANNEL_MANAGEMENT_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "opendash_common.h"
#include "channel_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ────────────────────────────────────────────────────────────────────────────
 * Structures
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Per-channel runtime statistics.
 */
typedef struct {
    bool     active;            /**< Channel has at least one registered node */
    uint8_t  channel_id;        /**< Channel index (0-3) */
    uint32_t interval_ms;       /**< Processing interval */
    uint32_t last_process_ms;   /**< Timestamp of last queue drain */
    uint32_t msgs_received;     /**< Total inbound messages on this channel */
    uint32_t msgs_sent;         /**< Total outbound messages on this channel */
    uint32_t errors;            /**< Send failures */
    uint32_t queue_high_water;  /**< Peak queue occupancy */
} channel_stats_t;

/**
 * @brief Registered node entry in the channel dispatcher.
 *
 * Populated when a node sends an ANNOUNCE message at boot or when
 * center discovers a node via an incoming STATUS_REPORT / DATA_RESPONSE.
 */
typedef struct {
    opendash_node_t node_type;  /**< Node type enum */
    uint8_t  channel_id;        /**< Assigned channel */
    uint8_t  mac[6];            /**< WiFi MAC address */
    bool     mac_known;         /**< MAC has been learned */
    bool     online;            /**< Currently considered online */
    uint32_t last_seen_ms;      /**< Timestamp of most recent data from node */
    int8_t   last_rssi;         /**< Last RSSI (dBm) */
    uint8_t  capabilities;      /**< NODE_CAP_* bitfield */
} channel_node_t;

/**
 * @brief Queued inbound message with metadata.
 */
typedef struct {
    uint8_t  src_mac[6];
    uint8_t  data[CHANNEL_QUEUE_ITEM_SIZE];
    uint16_t len;
    int8_t   rssi;
    uint8_t  channel_id;        /**< Which channel this message belongs to */
} channel_inbound_msg_t;

/* ────────────────────────────────────────────────────────────────────────────
 * Lifecycle
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Initialize the channel management system.
 *
 * Creates per-channel FreeRTOS queues and zeroes the node registry.
 * Call once at boot, after opendash_espnow_init().
 */
esp_err_t channel_mgr_init(void);

/**
 * @brief Tear down the channel management system.
 */
esp_err_t channel_mgr_deinit(void);

/* ────────────────────────────────────────────────────────────────────────────
 * Node Registry
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Register a node into the dispatcher.
 *
 * Called when:
 *   - A node's ANNOUNCE message is received at boot
 *   - A DATA_RESPONSE is received from an unknown MAC (auto-discover)
 *
 * @param node_type  The opendash_node_t of the node.
 * @param mac        6-byte MAC address.
 * @param channel_id Assigned channel (typically from NODE_DEFAULT_CHANNEL).
 * @param caps       Capability bitfield (NODE_CAP_*).
 * @return ESP_OK on success.
 */
esp_err_t channel_mgr_register_node(opendash_node_t node_type,
                                     const uint8_t mac[6],
                                     uint8_t channel_id,
                                     uint8_t caps);

/**
 * @brief Unregister a node (e.g., after a factory reset command).
 */
esp_err_t channel_mgr_unregister_node(opendash_node_t node_type);

/**
 * @brief Get read-only pointer to a node's registry entry.
 *
 * @param node_type  Which node.
 * @return Pointer, or NULL if invalid.
 */
const channel_node_t *channel_mgr_get_node(opendash_node_t node_type);

/**
 * @brief Check and update online status for all nodes.
 *
 * Scans the node registry and marks nodes offline if their last_seen_ms
 * exceeds the channel's offline timeout.  Call periodically (e.g., every 1s).
 */
void channel_mgr_check_timeouts(void);

/* ────────────────────────────────────────────────────────────────────────────
 * Inbound Message Routing
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Route an inbound ESP-NOW message to the correct channel queue.
 *
 * Called from the ESP-NOW receive path (after deserialization identifies
 * the sender's node type).  The message is placed into the appropriate
 * channel's FreeRTOS queue for deferred processing.
 *
 * @param msg   Inbound message metadata + payload.
 * @return ESP_OK if queued, ESP_ERR_NO_MEM if queue full.
 */
esp_err_t channel_mgr_route_inbound(const channel_inbound_msg_t *msg);

/**
 * @brief Drain one message from a channel's inbound queue.
 *
 * @param channel_id   Which channel to drain.
 * @param out_msg      Buffer to receive the message.
 * @param timeout_ms   0 = non-blocking.
 * @return true if a message was retrieved.
 */
bool channel_mgr_recv(uint8_t channel_id,
                       channel_inbound_msg_t *out_msg,
                       uint32_t timeout_ms);

/* ────────────────────────────────────────────────────────────────────────────
 * Outbound Sending
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Send a serialized message to a specific node with channel-aware
 *        retry policy.
 *
 * @param node_type  Target node.
 * @param data       Serialized protocol frame.
 * @param len        Frame length.
 * @return ESP_OK on success (after retries if needed).
 */
esp_err_t channel_mgr_send_to_node(opendash_node_t node_type,
                                    const uint8_t *data, uint16_t len);

/**
 * @brief Pause normal data traffic to a node for a short control window.
 *
 * Used before high-priority system commands such as ENTER_BT_OTA so the
 * target node is not immediately re-saturated by normal telemetry fan-out.
 * The pause only affects the standard channel-aware send path.
 *
 * @param node_type    Target node.
 * @param duration_ms  Pause length in milliseconds.
 * @return ESP_OK on success.
 */
esp_err_t channel_mgr_pause_node_traffic(opendash_node_t node_type,
                                          uint32_t duration_ms);

/**
 * @brief Force-send a frame to a node using control-channel retry policy.
 *
 * This bypasses the normal per-node dead-window checks so high-priority
 * commands can still be attempted while the regular data plane is paused or
 * quarantined.
 *
 * @param node_type     Target node.
 * @param data          Serialized protocol frame.
 * @param len           Frame length.
 * @param max_retries   Retry count to use for the control send.
 * @return ESP_OK on success.
 */
esp_err_t channel_mgr_force_send_to_node(opendash_node_t node_type,
                                          const uint8_t *data,
                                          uint16_t len,
                                          uint8_t max_retries);

/**
 * @brief Broadcast a message on a specific channel (all nodes on that channel).
 *
 * @param channel_id  Target channel.
 * @param data        Serialized protocol frame.
 * @param len         Frame length.
 * @return ESP_OK on success.
 */
esp_err_t channel_mgr_broadcast_channel(uint8_t channel_id,
                                         const uint8_t *data, uint16_t len);

/* ────────────────────────────────────────────────────────────────────────────
 * Delta Tracking
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Record a data point value for delta detection.
 *
 * @param dp_id  Data point ID.
 * @param value  Current value.
 * @return true if the value actually changed from the previously recorded value.
 */
bool channel_mgr_dp_changed(uint16_t dp_id, float value);

/* ────────────────────────────────────────────────────────────────────────────
 * Statistics
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Get runtime statistics for a channel.
 */
esp_err_t channel_mgr_get_stats(uint8_t channel_id, channel_stats_t *out);

/**
 * @brief Log a summary of all channel statistics (ESP_LOGD level).
 */
void channel_mgr_log_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* CHANNEL_MANAGEMENT_H */