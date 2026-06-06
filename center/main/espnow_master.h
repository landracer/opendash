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
#include "opendash_espnow.h"
#include "opendash_parachute.h"
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

/* ────────────────────────────────────────────────────────────────────────────
 * OBD / DTC Support (relayed via Left pod)
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Relay an OBD command to the Left pod (which sends it to MD via UART).
 *
 * @param obd_cmd  OBD command byte (0x43=clear DTC, 0x44=read DTC, 0x56=VIN).
 * @return ESP_OK on success.
 */
esp_err_t espnow_master_send_obd_command(uint8_t obd_cmd);

/**
 * @brief Get cached DTC data received from the Left pod.
 *
 * @param codes   Output array of DTC code strings (5 chars each, null-safe).
 * @param count   Output DTC count.
 * @param valid   Output validity flag.
 */
void espnow_master_get_dtc_data(char codes[][6], uint8_t *count, bool *valid);

/* ────────────────────────────────────────────────────────────────────────────
 * Additive helpers used by boost_client.c (and other future subsystems)
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Send an arbitrary opcode + payload to a slave (unicast).
 *
 * Builds the standard opendash_i2c framing and dispatches via the node's
 * registered channel. Use this for low-rate control/telemetry that doesn't
 * fit the SET_DATA_POINT/SET_RELAY shaped slots.
 *
 * @param node     Target node.
 * @param cmd      Opcode (see opendash_i2c_protocol.h).
 * @param payload  Pointer to payload bytes (may be NULL if @p length == 0).
 * @param length   Payload byte count (0..OPENDASH_ESPNOW_MAX_DATA-2).
 * @return ESP_OK on success.
 */
esp_err_t espnow_master_send_raw(opendash_node_t node, uint8_t cmd,
                                  const void *payload, uint16_t length);

/* ────────────────────────────────────────────────────────────────────────────
 * Parachute / Deployment System (Center → MOS, with STATUS echo back)
 *
 * The MOS node owns and NVS-persists the deployment config. Center pushes
 * updates and reads back a STATUS echo (cached here) for the DEVICE MGMT UI.
 * ARM is a separate, NON-persisted toggle.
 * ──────────────────────────────────────────────────────────────────────────── */

/** @brief Push a full deployment config to a MOS node (OPENDASH_CMD_PARACHUTE_SET_CONFIG). */
esp_err_t espnow_master_send_parachute_config(opendash_node_t node,
                                               const opendash_parachute_config_t *cfg);

/** @brief Arm/disarm the deployment actuator on a MOS node (OPENDASH_CMD_PARACHUTE_SET_ARM). */
esp_err_t espnow_master_send_parachute_arm(opendash_node_t node, bool armed);

/** @brief Request a STATUS echo from a MOS node (OPENDASH_CMD_PARACHUTE_PULL_ALL). */
esp_err_t espnow_master_send_parachute_pull(opendash_node_t node);

/** @brief Request a manual/interlocked DEPLOY on a MOS node (OPENDASH_CMD_PARACHUTE_DEPLOY).
 *  The MOS only fires if enabled + ARMED + a channel is selected. */
esp_err_t espnow_master_send_parachute_deploy(opendash_node_t node);

/** Zero/cal the gyro detectors' roll to their current resting angle (no payload). */
esp_err_t espnow_master_send_parachute_calibrate(opendash_node_t node);

/**
 * @brief Read the last cached parachute STATUS echo from a MOS node.
 * @param node  Target MOS node.
 * @param out   Destination status struct.
 * @return true if a status has been received since boot, false otherwise.
 */
bool espnow_master_get_parachute_status(opendash_node_t node,
                                         opendash_parachute_status_t *out);

/**
 * @brief MOS channels (bit0=CH1..bit3=CH4) the deployment system has committed
 *        on @p node, from the last cached STATUS echo.
 *
 * Returns 0 if no status has been received. Used by the boost UI to grey out
 * channels already claimed by the safety system so the two never overlap.
 */
uint8_t espnow_master_parachute_reserved_mask(opendash_node_t node);

/** @brief True if the node has been seen recently. Wrapper over node_health. */
bool espnow_master_node_online(opendash_node_t node);

/**
 * @brief Distributed-rollover fusion status for the deploy-panel indicator.
 *
 * The autonomous-deploy decision itself runs continuously inside the master
 * (independent of the UI); this getter only exposes the live tally for display.
 *
 * @param manual           If non-NULL, set true when any detector is holding a
 *                         fresh manual-release vote.
 * @param detectors_total  If non-NULL, receives the total detector count.
 * @return Number of detectors currently voting `rolling` with a fresh vote.
 */
int espnow_master_rollover_status(bool *manual, int *detectors_total);

/**
 * @brief Receive hook for opcodes not handled by the built-in pipeline.
 *
 * The dispatcher invokes this callback for any successfully-deserialized
 * inbound frame whose @p cmd falls outside the standard set the dispatcher
 * already routes (currently: anything in the BOOST report range 0x90-0x94).
 *
 * Runs in the dispatcher task — keep the callback short and non-blocking.
 */
typedef void (*espnow_master_rx_cb_t)(const opendash_espnow_event_t *evt,
                                       const opendash_i2c_msg_t *msg);

/** @brief Install (or replace) the auxiliary RX callback. Pass NULL to remove. */
void espnow_master_set_aux_rx_callback(espnow_master_rx_cb_t cb);

/**
 * @brief Snapshot the engine DP cache that the dispatcher latches passively.
 *
 * Used by boost_client to assemble the 10 Hz live frame. Any @c out_*
 * pointer may be NULL to skip that channel.
 *
 * @param[out] rpm           Last seen engine RPM.
 * @param[out] boost_cbar    Manifold pressure in centi-bar (gauge).
 * @param[out] egt_c         Hottest EGT in °C.
 * @param[out] afr           Air/fuel ratio (e.g. 14.7).
 * @param[out] fuel_kpa      Fuel rail pressure in kPa.
 * @param[out] throttle_pct  Throttle position 0..100.
 * @param[out] gear          Current gear (0 = unknown/neutral).
 * @param[out] last_update_ms Age of the freshest cached value (ms).
 */
void espnow_master_snapshot_engine(float *rpm, float *boost_cbar, float *egt_c,
                                    float *afr, float *fuel_kpa,
                                    float *throttle_pct, float *gear,
                                    uint32_t *last_update_ms);

#ifdef __cplusplus
}
#endif

#endif /* ESPNOW_MASTER_H */