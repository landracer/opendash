/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file espnow_master.h
 * @brief OpenDash Center Display — Channel-Based ESP-NOW Master Controller
 *
 * Replaces the legacy polling master with an event-driven, channel-based
 * dispatcher.  The master no longer broadcasts PINGs.  Instead:
 *
 *   1. Slave nodes send an ANNOUNCE at boot (one-time registration)
 *   2. Slave nodes push data on change via DATA_RESPONSE
 *   3. Master routes inbound data to the correct channel queue
 *   4. Per-channel tasks drain their queues and update the UI
 *   5. Offline detection uses data-absence timeout (no heartbeat needed)
 *   6. Commands (relay, OTA, reboot) go through CHANNEL_CONTROL
 *
 * ARCHITECTURE RULE: NO POLLING / NO PINGING.
 *
 * @see channel_management.h  for the routing engine.
 * @see channel_config.h      for timing and buffer tuning.
 * @see node_definitions.h    for node→channel mapping.
 * @see opendash_espnow.h     for the transport layer.
 * @see opendash_i2c_protocol.h for message format.
 */

#ifndef ESPNOW_MASTER_H
#define ESPNOW_MASTER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "opendash_common.h"
#include "opendash_i2c_protocol.h"
#include "channel_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ────────────────────────────────────────────────────────────────────────────
 * Node Status Snapshot (read-only, updated by dispatcher)
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Bitmask snapshot of all node online/offline states.
 *
 * Updated atomically by the dispatcher task.  UI code reads this
 * without locking — worst case it's one cycle stale.
 */
typedef struct {
    bool left_online;
    bool right_online;
    bool gps_online;
    bool bms_online;
    bool pod1_online;
    bool pod2_online;
    bool relay_4ch_online;
    bool relay_8ch_a_online;
    bool relay_8ch_b_online;
    bool mos_4ch_a_online;
    bool mos_4ch_b_online;
} espnow_master_node_status_t;

/* ────────────────────────────────────────────────────────────────────────────
 * Lifecycle
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Initialize ESP-NOW transport + channel manager.
 *
 * Call after nvs_flash_init() and before espnow_master_start().
 * Does NOT start any tasks — just initializes data structures.
 *
 * @return ESP_OK on success.
 */
esp_err_t espnow_master_init(void);

/**
 * @brief Start the channel-based dispatcher and per-channel worker tasks.
 *
 * Creates:
 *   - Dispatcher task (core 0, priority 4): drains ESP-NOW rx queue,
 *     identifies sender, routes to channel queues
 *   - Per-channel worker tasks: drain their queue, process data,
 *     update UI, forward to consumers
 *   - Timeout checker (1 Hz): marks stale nodes offline
 *
 * @return ESP_OK on success.
 */
esp_err_t espnow_master_start(void);

/* ────────────────────────────────────────────────────────────────────────────
 * Status
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Get a snapshot of all node online/offline states.
 */
void espnow_master_get_status(espnow_master_node_status_t *status);

/* ────────────────────────────────────────────────────────────────────────────
 * Outbound Commands (from center to slaves)
 *
 * All of these go through CHANNEL_CONTROL for immediate delivery.
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Push a data point value to a specific slave node.
 *
 * Serializes as SET_DATA_POINT and sends via the node's registered channel.
 * Uses delta tracking — if the value hasn't changed, the send is suppressed.
 *
 * @param node    Target node.
 * @param dp_id   Data point ID (from opendash_data_model.h).
 * @param value   Float value.
 * @return ESP_OK, or ESP_ERR_INVALID_ARG if node is unknown.
 */
esp_err_t espnow_master_send_data_point(opendash_node_t node,
                                         uint16_t dp_id, float value);

/**
 * @brief Send a relay ON/OFF/PWM command.
 *
 * Goes through CHANNEL_CONTROL for immediate delivery with max retries.
 *
 * @param node      Target relay/MOS node.
 * @param channel   Relay channel number (0-based).
 * @param state     0=OFF, 1=ON.
 * @param pwm_duty  PWM duty (0-255, MOS nodes only; ignored for relays).
 * @return ESP_OK on success.
 */
esp_err_t espnow_master_send_relay_command(opendash_node_t node,
                                            uint8_t channel, uint8_t state,
                                            uint8_t pwm_duty);

/**
 * @brief Send a system subcommand (reboot, OTA, self-test, etc.).
 *
 * @param node    Target node.
 * @param subcmd  System subcommand byte.
 * @return ESP_OK on success.
 */
esp_err_t espnow_master_send_system_subcmd(opendash_node_t node, uint8_t subcmd);

#ifdef __cplusplus
}
#endif

#endif /* ESPNOW_MASTER_H */