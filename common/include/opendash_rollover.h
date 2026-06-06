/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_rollover.h
 * @brief OpenDash distributed rollover-detection — detector + fusion tunables.
 *
 * Companion to opendash_parachute.h. That header owns the *thresholds*
 * (roll angle / rate / sustain), which are center-pushed at runtime. THIS
 * header owns the *distributed-detection plumbing*: which nodes vote, how long
 * a vote stays fresh, how often a detector refreshes an active vote, which IMU
 * axis is "roll", and the per-node physical manual-release button GPIO.
 *
 * SAFETY MODEL (see TODO.md §11.7 and the deploy panel):
 *   - Detectors are the gyro-equipped, low-tasked nodes: RIGHT, POD1, POD2.
 *   - A detector broadcasts a VOTE (OPENDASH_CMD_PARACHUTE_VOTE) when its local
 *     roll-detect state changes and refreshes it while rolling.
 *   - Center auto-deploys ONLY when ALL required detectors have a fresh
 *     `rolling` vote AND the system is enabled + AUTO_DETECT + armed + the MOS
 *     online (unanimous; one silent/dissenting node blocks autonomous fire).
 *   - A physical manual-release button on any detector is a hard operator
 *     override (still interlocked by enabled + armed + online).
 *
 * Every value is #ifndef-wrapped so a node or build flag may override it.
 */

#ifndef OPENDASH_ROLLOVER_H
#define OPENDASH_ROLLOVER_H

#include "opendash_common.h"   /* opendash_node_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ════════════════════════════════════════════════════════════════════════════
 * 1. DETECTOR SET  (unanimous quorum)
 *    The gyro-equipped nodes with task headroom. ALL must vote `rolling` for an
 *    autonomous deploy. RIGHT carries a QMI8658 (driver ported for this feature;
 *    it otherwise mirrors LEFT). GPS/LEFT are intentionally excluded (tasked).
 * ════════════════════════════════════════════════════════════════════════════ */

/** Ordered list of detector nodes. Keep in sync with DETECTOR_COUNT. */
#define OPENDASH_ROLLOVER_DETECTORS \
    { OPENDASH_NODE_RIGHT, OPENDASH_NODE_POD1, OPENDASH_NODE_POD2 }

/** Number of detectors that must ALL agree to fire (unanimous quorum). */
#define OPENDASH_ROLLOVER_DETECTOR_COUNT  3

/* ════════════════════════════════════════════════════════════════════════════
 * 2. VOTE FRESHNESS  (center-side)
 * ════════════════════════════════════════════════════════════════════════════ */

/** A cached vote older than this (ms since last RX) is treated as no vote. */
#ifndef OPENDASH_ROLLOVER_VOTE_EXPIRY_MS
#define OPENDASH_ROLLOVER_VOTE_EXPIRY_MS   600
#endif

/** Center fusion reconciler tick period (ms). */
#ifndef OPENDASH_ROLLOVER_FUSE_PERIOD_MS
#define OPENDASH_ROLLOVER_FUSE_PERIOD_MS   100
#endif

/* ════════════════════════════════════════════════════════════════════════════
 * 3. DETECTOR BEHAVIOUR  (node-side)
 * ════════════════════════════════════════════════════════════════════════════ */

/** While a roll is active, re-broadcast the vote at least this often (ms) so the
 *  center's freshness window never lapses mid-event. Must be < VOTE_EXPIRY_MS. */
#ifndef OPENDASH_ROLLOVER_VOTE_REFRESH_MS
#define OPENDASH_ROLLOVER_VOTE_REFRESH_MS  150
#endif

/** Detector roll-evaluation period (ms). 10 ms = 100 Hz, matches the IMU task. */
#ifndef OPENDASH_ROLLOVER_EVAL_PERIOD_MS
#define OPENDASH_ROLLOVER_EVAL_PERIOD_MS   10
#endif

/** Live roll ANGLE (deg) from an imu_data_t — the accel-derived tilt. */
#ifndef OPENDASH_ROLLOVER_ROLL_ANGLE
#define OPENDASH_ROLLOVER_ROLL_ANGLE(imu)  ((imu).roll)
#endif

/** Live roll RATE (deg/s) from an imu_data_t — gyro about the vehicle roll axis.
 *  Change the member here if a board is mounted on a different axis. */
#ifndef OPENDASH_ROLLOVER_ROLL_RATE
#define OPENDASH_ROLLOVER_ROLL_RATE(imu)   ((imu).gyro_x)
#endif

/* ════════════════════════════════════════════════════════════════════════════
 * 4. MANUAL-RELEASE BUTTON  (physical, per detector node)
 *    DEFAULT -1 = NOT wired / disabled, so nothing can fire by accident. Stage a
 *    free GPIO here to enable a hard operator override on that unit.
 *    Free pins (per Explore audit): POD1/POD2 e.g. 0,1,2,3,8,9,10,13,16,17;
 *    RIGHT e.g. 0,4,19,20,22..37,42..44.
 * ════════════════════════════════════════════════════════════════════════════ */

#ifndef OPENDASH_ROLLOVER_BTN_GPIO_RIGHT
#define OPENDASH_ROLLOVER_BTN_GPIO_RIGHT   (-1)
#endif
#ifndef OPENDASH_ROLLOVER_BTN_GPIO_POD1
#define OPENDASH_ROLLOVER_BTN_GPIO_POD1    (-1)
#endif
#ifndef OPENDASH_ROLLOVER_BTN_GPIO_POD2
#define OPENDASH_ROLLOVER_BTN_GPIO_POD2    (-1)
#endif

/** Button active level: 0 = active-low (button to GND + internal pull-up),
 *  1 = active-high (button to 3V3 + internal pull-down). */
#ifndef OPENDASH_ROLLOVER_BTN_ACTIVE_LEVEL
#define OPENDASH_ROLLOVER_BTN_ACTIVE_LEVEL  0
#endif

/** Continuous hold (ms) before a manual release is declared (anti-bump). */
#ifndef OPENDASH_ROLLOVER_BTN_HOLD_MS
#define OPENDASH_ROLLOVER_BTN_HOLD_MS       750
#endif

/* ════════════════════════════════════════════════════════════════════════════
 * 5. DETECTOR MODULE API  (opendash_rollover_detector.c)
 *
 * Shared by every detector node (RIGHT, POD1, POD2). The node supplies a tiny
 * read callback that yields the live roll angle + rate from its own IMU; the
 * module owns the sustain/hysteresis/rate state machine, the manual-release
 * button, and broadcasting the VOTE frame. Thresholds come from the
 * center-pushed parachute config (opendash_parachute_config_get), so the node
 * MUST call opendash_parachute_config_init() before starting the detector.
 * ════════════════════════════════════════════════════════════════════════════ */

#include "esp_err.h"

/** @brief Fill the live roll ANGLE (deg) and roll RATE (deg/s) from the node's
 *  IMU. Return true on success; false (e.g. IMU read timeout) skips the cycle. */
typedef bool (*opendash_rollover_read_fn)(float *roll_deg, float *roll_rate);

/**
 * @brief Start the rollover detector for this node.
 *
 * Configures the manual-release button GPIO (if staged for @p self) and spawns
 * a ~100 Hz task that reads the IMU via @p read, evaluates the roll state, and
 * broadcasts a VOTE on every state change, on a manual-button hold, and at the
 * refresh cadence while a roll/manual condition is active. Emits NOTHING while
 * idle (no airtime when the car is upright).
 *
 * @param self  This node's role (OPENDASH_NODE_RIGHT/POD1/POD2).
 * @param read  IMU read callback.
 * @return ESP_OK on success.
 */
esp_err_t opendash_rollover_detector_start(opendash_node_t self,
                                           opendash_rollover_read_fn read);

/**
 * @brief Broadcast this detector's STATUS echo (config + SAFE actuator fields).
 *
 * Call from the node's command dispatch when center sends SET_CONFIG (0x27) or
 * PULL_ALL (0x29), so the center's push/refresh reconciler can confirm the
 * detector persisted the pushed thresholds (memcmp on the cfg).
 */
void opendash_rollover_detector_send_status(void);

/**
 * @brief Zero/calibrate this detector's roll to the current resting angle.
 *
 * Captures the live raw roll as a mounting-offset baseline (so a non-level
 * install reads ~0 deg at rest) and NVS-persists it, so the offset survives a
 * reboot. Detection thereafter measures roll RELATIVE to this baseline. Call
 * from the node's command dispatch when center sends CALIBRATE (0x2B).
 */
void opendash_rollover_detector_calibrate(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENDASH_ROLLOVER_H */
