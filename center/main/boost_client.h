/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file boost_client.h
 * @brief Center-side client for the remote (slave-resident) boost controller.
 *
 * Responsibilities:
 *   - Push a 10 Hz live engine snapshot to the boost slave.
 *   - Forward UI map / param edits to the slave.
 *   - Consume telemetry + map-echo frames coming back.
 *
 * Target slave is selected by g_boost_target_node (see system_config.h).
 */

#ifndef OPENDASH_CENTER_BOOST_CLIENT_H
#define OPENDASH_CENTER_BOOST_CLIENT_H

#include "esp_err.h"
#include "opendash_common.h"
#include "opendash_boost.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Start the client (NVS, tasks, RX hook). Idempotent. */
esp_err_t boost_client_init(void);

/* ── Authoring helpers — all unicast to g_boost_target_node ── */
esp_err_t boost_client_set_params      (const opendash_boost_params_t *p);
esp_err_t boost_client_set_mode        (opendash_boost_mode_t mode);
esp_err_t boost_client_set_duty_row    (uint8_t mode_slot, uint8_t gear, const uint8_t  duty[OPENDASH_BOOST_MAP_POINTS]);
esp_err_t boost_client_set_setpoint_row(uint8_t mode_slot, uint8_t gear, const uint16_t setp_cbar[OPENDASH_BOOST_MAP_POINTS]);
esp_err_t boost_client_set_throttle    (const opendash_boost_throttle_curve_t *c);

/** @brief Ask the slave to dump params + every map row (used when UI opens). */
esp_err_t boost_client_request_pull_all(void);

/** @brief Latest telemetry received from the slave (zeroed before first frame). */
void boost_client_get_telemetry(opendash_boost_telemetry_t *out);

/**
 * @brief Age in milliseconds of the most recent telemetry frame.
 *
 * Returns UINT32_MAX when no telemetry has ever been received. Use this from
 * the UI to detect total RX silence (slave offline / out of range / radio
 * congestion) as distinct from the slave's own DATA_STALE flag, which only
 * fires when the slave stops getting live data from the center.
 */
uint32_t boost_client_telemetry_age_ms(void);

/**
 * @brief MOS channels (bit0=CH1..bit3=CH4) the boost output owns on @p node.
 *
 * Returns 0 for any node that is not the current boost target, or before the
 * slave's params have been mirrored. Used by the deployment UI to grey out
 * channels already claimed by boost so the two subsystems never overlap on the
 * same MOS-FET.
 */
uint8_t boost_client_reserved_mask(opendash_node_t node);

/** @brief Snapshot the local copy of a duty row (filled by REPORT frames). */
bool boost_client_peek_duty_row    (uint8_t mode_slot, uint8_t gear, uint8_t  out[OPENDASH_BOOST_MAP_POINTS]);
bool boost_client_peek_setpoint_row(uint8_t mode_slot, uint8_t gear, uint16_t out[OPENDASH_BOOST_MAP_POINTS]);
bool boost_client_peek_params      (opendash_boost_params_t *out);

#ifdef __cplusplus
}
#endif

#endif /* OPENDASH_CENTER_BOOST_CLIENT_H */
