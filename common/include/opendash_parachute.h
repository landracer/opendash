/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_parachute.h
 * @brief OpenDash Parachute / Rollover Deployment — Shared Configuration
 *
 * SINGLE SOURCE OF TRUTH for every tunable number in the distributed
 * parachute deployment system. Every safety-capable node (CENTER, GPS,
 * POD1, POD2, the MOS actuator nodes, and the integrated rAtTrax-BMS Logger)
 * pulls its thresholds from here so behaviour is identical across the system.
 *
 * DESIGN (see TODO.md §11.7):
 *   - Distributed voting, single physical actuator (a MOS-4CH channel).
 *   - Any gyro-equipped node is a VOTER and may also issue a false-positive
 *     VETO/override.
 *   - Solo-capable: a lone node uses its own vote (quorum collapses to 1).
 *   - rAtTrax-BMS is integrated, NOT integral — both run standalone.
 *
 * ALL trigger values below are intentionally up-front and per-application
 * tunable. Each is wrapped in `#ifndef` so a node (or build flag) may override
 * it before including this header. Some vehicles flip easily, some need a
 * narrower window before a "trigger event" is declared — change here, rebuild.
 *
 * @note Default deploy angle starts SENSITIVE (45°). The rAtTrax-BMS flip
 *       threshold (80°) is a separate alignment constant. These are STARTING
 *       points to be refined on data.
 */

#ifndef OPENDASH_PARACHUTE_H
#define OPENDASH_PARACHUTE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Config schema version (bump when fields/semantics change). */
#define OPENDASH_PARACHUTE_CONFIG_VERSION  1

/* Config flag bits (opendash_parachute_config_t.flags).
 * Default 0 = LATCH ON until disarm/clear (what most users want). Set the bit
 * to pulse the channel for pulse_ms instead. Bit-flags keep the wire/NVS struct
 * the same size and backward-compatible with the old `reserved` byte (0). */
#define OPENDASH_PARACHUTE_FLAG_FIRE_PULSE  (1u << 0)  /**< 1 = pulse pulse_ms; 0 = latch on */
#define OPENDASH_PARACHUTE_FLAG_AUTO_DETECT (1u << 1)  /**< 1 = autonomous rollover-vote deploy enabled; 0 = manual only */

/** Mask of all defined config flag bits (sanitize drops anything outside this). */
#define OPENDASH_PARACHUTE_FLAG_MASK \
    (OPENDASH_PARACHUTE_FLAG_FIRE_PULSE | OPENDASH_PARACHUTE_FLAG_AUTO_DETECT)

/* ════════════════════════════════════════════════════════════════════════════
 * 1. ROLL / INVERSION DEPLOY THRESHOLDS  (degrees)
 *    Inversion deploys. Tunable per application — some vehicles flip easily.
 * ════════════════════════════════════════════════════════════════════════════ */

/** Sustained roll/inversion angle that declares a deploy event.
 *  Default is intentionally SENSITIVE (45°) — refine upward on real data. */
#ifndef OPENDASH_PARACHUTE_ROLL_DEPLOY_DEG
#define OPENDASH_PARACHUTE_ROLL_DEPLOY_DEG       45.0f
#endif

/** Pre-arm WARNING band — alert / pre-tension, no deploy. */
#ifndef OPENDASH_PARACHUTE_ROLL_WARNING_DEG
#define OPENDASH_PARACHUTE_ROLL_WARNING_DEG      25.0f
#endif

/** Roll must hold past the deploy angle for at least this long (ms). */
#ifndef OPENDASH_PARACHUTE_ROLL_SUSTAIN_MS
#define OPENDASH_PARACHUTE_ROLL_SUSTAIN_MS       200
#endif

/** Fast tip-over: roll RATE above this (deg/s) deploys without waiting sustain. */
#ifndef OPENDASH_PARACHUTE_ROLL_RATE_DEG_S
#define OPENDASH_PARACHUTE_ROLL_RATE_DEG_S       300.0f
#endif

/** Hysteresis band on angle transitions (deg). */
#ifndef OPENDASH_PARACHUTE_TILT_HYSTERESIS_DEG
#define OPENDASH_PARACHUTE_TILT_HYSTERESIS_DEG   5.0f
#endif

/* ════════════════════════════════════════════════════════════════════════════
 * 2. SPEED GATE
 *    No deploy below this speed (pit / trailer / low-speed false-deploy guard).
 * ════════════════════════════════════════════════════════════════════════════ */

/** Minimum vehicle speed (mph) before ANY deploy vote is valid. */
#ifndef OPENDASH_PARACHUTE_MIN_SPEED_MPH
#define OPENDASH_PARACHUTE_MIN_SPEED_MPH         150.0f
#endif

/* ════════════════════════════════════════════════════════════════════════════
 * 3. ORIENTATION FUSION  (per voter node)
 *    Complementary filter starting point; upgrade path Madgwick → EKF → DMP.
 * ════════════════════════════════════════════════════════════════════════════ */

/** Complementary filter coefficient: angle = A*(angle+gyro*dt) + (1-A)*accel. */
#ifndef OPENDASH_PARACHUTE_FUSION_ALPHA
#define OPENDASH_PARACHUTE_FUSION_ALPHA          0.98f
#endif

/** IMU sample rate target (Hz). Valid window 100–400 Hz. */
#ifndef OPENDASH_PARACHUTE_SAMPLE_RATE_HZ
#define OPENDASH_PARACHUTE_SAMPLE_RATE_HZ        200
#endif

/** Hard latency budget: roll onset → fire signal (ms). */
#ifndef OPENDASH_PARACHUTE_LATENCY_BUDGET_MS
#define OPENDASH_PARACHUTE_LATENCY_BUDGET_MS     80
#endif

/* ════════════════════════════════════════════════════════════════════════════
 * 4. FALSE-POSITIVE REJECTION  (rough racing surfaces must NOT deploy)
 * ════════════════════════════════════════════════════════════════════════════ */

/** Window length (samples) for the median/low-pass vibration filter. */
#ifndef OPENDASH_PARACHUTE_FILTER_WINDOW
#define OPENDASH_PARACHUTE_FILTER_WINDOW         7
#endif

/** Reject a roll event if accel-magnitude variance exceeds this (g^2) —
 *  high variance ⇒ surface chatter / impact, not a true rollover. */
#ifndef OPENDASH_PARACHUTE_ACCEL_VAR_REJECT
#define OPENDASH_PARACHUTE_ACCEL_VAR_REJECT      4.0f
#endif

/** Roll must dominate: reject if |pitch-rate| or |yaw-rate| exceeds this
 *  fraction of |roll-rate| (co-spike ⇒ impact/jump, not roll). */
#ifndef OPENDASH_PARACHUTE_ROLL_DOMINANCE
#define OPENDASH_PARACHUTE_ROLL_DOMINANCE        0.6f
#endif

/* ════════════════════════════════════════════════════════════════════════════
 * 5. WHEEL-SPEED ROLL CORROBORATION
 *    From rAtTrax-BMS 4-wheel RPM, OR any /relay module when BMS is absent.
 * ════════════════════════════════════════════════════════════════════════════ */

/** Left↔right wheel-speed divergence fraction that corroborates a roll. */
#ifndef OPENDASH_PARACHUTE_WHEEL_DIFF_THRESHOLD
#define OPENDASH_PARACHUTE_WHEEL_DIFF_THRESHOLD  0.35f
#endif

/** Only evaluate wheel-fault corroboration above this wheel speed (RPM). */
#ifndef OPENDASH_PARACHUTE_WHEEL_FAULT_SPEED_MIN
#define OPENDASH_PARACHUTE_WHEEL_FAULT_SPEED_MIN 50.0f
#endif

/* ════════════════════════════════════════════════════════════════════════════
 * 6. VOTING / FIRE PROTOCOL
 * ════════════════════════════════════════════════════════════════════════════ */

/** Minimum positive votes to fire. Collapses to 1 when a node is solo. */
#ifndef OPENDASH_PARACHUTE_VOTE_QUORUM
#define OPENDASH_PARACHUTE_VOTE_QUORUM           2
#endif

/** Per-node minimum confidence (0–100) for a vote to count. */
#ifndef OPENDASH_PARACHUTE_VOTE_MIN_CONFIDENCE
#define OPENDASH_PARACHUTE_VOTE_MIN_CONFIDENCE   70
#endif

/** Votes must arrive within this window to be tallied together (ms). */
#ifndef OPENDASH_PARACHUTE_VOTE_WINDOW_MS
#define OPENDASH_PARACHUTE_VOTE_WINDOW_MS        150
#endif

/* ════════════════════════════════════════════════════════════════════════════
 * 7. ACTUATOR  (MOS-4CH channel drives the deploy charge)
 * ════════════════════════════════════════════════════════════════════════════ */

/** Energize duration of the deploy output (ms). 0 = latch on (no auto-release). */
#ifndef OPENDASH_PARACHUTE_DEPLOY_PULSE_MS
#define OPENDASH_PARACHUTE_DEPLOY_PULSE_MS       500
#endif

/* ════════════════════════════════════════════════════════════════════════════
 * 7b. DEPLOYMENT CONFIG DEFAULTS  (center-pushed, MOS-persisted)
 *     Installed on a MOS node the first time it boots without a saved config.
 *     Intentionally SAFE on a fresh unit: system disabled, no channels armed.
 * ════════════════════════════════════════════════════════════════════════════ */

/** Default deploy-system enable on a fresh MOS (0 = off until user enables). */
#ifndef OPENDASH_PARACHUTE_DEFAULT_ENABLED
#define OPENDASH_PARACHUTE_DEFAULT_ENABLED       0
#endif

/** Default fired-channel mask (bit0..bit3 = CH1..CH4). 0 = none until chosen. */
#ifndef OPENDASH_PARACHUTE_DEFAULT_CHANNEL_MASK
#define OPENDASH_PARACHUTE_DEFAULT_CHANNEL_MASK   0x00
#endif

/* ════════════════════════════════════════════════════════════════════════════
 * 8. rAtTrax-BMS ALIGNMENT  (keep both projects' numbers in lock-step)
 * ════════════════════════════════════════════════════════════════════════════ */

#ifndef OPENDASH_PARACHUTE_BMS_TILT_CRITICAL_DEG
#define OPENDASH_PARACHUTE_BMS_TILT_CRITICAL_DEG 60.0f   /* emergency brake */
#endif
#ifndef OPENDASH_PARACHUTE_BMS_TILT_WARNING_DEG
#define OPENDASH_PARACHUTE_BMS_TILT_WARNING_DEG  45.0f
#endif
#ifndef OPENDASH_PARACHUTE_BMS_SOC_CRITICAL_PCT
#define OPENDASH_PARACHUTE_BMS_SOC_CRITICAL_PCT  5.0f
#endif
#ifndef OPENDASH_PARACHUTE_BMS_TEMP_CRITICAL_C
#define OPENDASH_PARACHUTE_BMS_TEMP_CRITICAL_C   60.0f
#endif
#ifndef OPENDASH_PARACHUTE_VESC_TEMP_CRITICAL_C
#define OPENDASH_PARACHUTE_VESC_TEMP_CRITICAL_C  85.0f
#endif

/* ════════════════════════════════════════════════════════════════════════════
 * 9. TYPES
 * ════════════════════════════════════════════════════════════════════════════ */

/** @brief Which node a vote/veto originated from. */
typedef enum {
    OPENDASH_PARACHUTE_SRC_UNKNOWN = 0,
    OPENDASH_PARACHUTE_SRC_CENTER,
    OPENDASH_PARACHUTE_SRC_GPS,
    OPENDASH_PARACHUTE_SRC_POD1,
    OPENDASH_PARACHUTE_SRC_POD2,
    OPENDASH_PARACHUTE_SRC_BMS,
    OPENDASH_PARACHUTE_SRC_RELAY,
} opendash_parachute_source_t;

/** @brief Why a deploy was requested / performed. */
typedef enum {
    OPENDASH_PARACHUTE_REASON_NONE = 0,
    OPENDASH_PARACHUTE_REASON_ROLL_ANGLE,   /**< sustained roll past deploy deg */
    OPENDASH_PARACHUTE_REASON_ROLL_RATE,    /**< fast tip-over (rate)           */
    OPENDASH_PARACHUTE_REASON_INVERSION,    /**< full inversion                 */
    OPENDASH_PARACHUTE_REASON_WHEEL_FAULT,  /**< wheel-speed corroboration      */
    OPENDASH_PARACHUTE_REASON_REMOTE_VOTE,  /**< quorum reached from peers      */
    OPENDASH_PARACHUTE_REASON_MANUAL,       /**< manual command                 */
} opendash_parachute_reason_t;

/** @brief Actuator runtime state. */
typedef enum {
    OPENDASH_PARACHUTE_ACT_UNINIT = 0,
    OPENDASH_PARACHUTE_ACT_SAFE,        /**< initialised, disarmed, de-energized */
    OPENDASH_PARACHUTE_ACT_ARMED,       /**< armed, ready, de-energized          */
    OPENDASH_PARACHUTE_ACT_DEPLOYED,    /**< fired + locked out                  */
} opendash_parachute_act_state_t;

/** @brief Per-node deploy actuator GPIO configuration (set in the MOS header). */
typedef struct {
    int      gpio_num;     /**< Deploy output GPIO (-1 = unassigned ⇒ inhibited). */
    bool     active_high;  /**< true = drive HIGH to deploy.                      */
    uint16_t pulse_ms;     /**< Energize duration; 0 = latch on.                  */
} opendash_parachute_actuator_config_t;

/* ════════════════════════════════════════════════════════════════════════════
 * 10. SHARED ACTUATOR API  (opendash_parachute_actuator.c)
 *
 * Both MOS-4CH-A and MOS-4CH-B link this identical implementation; each
 * supplies its own GPIO via its parachute_gpio.h. Safe no-op when unassigned.
 * Firing is HARD-INHIBITED unless ARMED.
 * ════════════════════════════════════════════════════════════════════════════ */

/** Initialise the deploy output (drives to SAFE/de-energized, disarmed). */
esp_err_t opendash_parachute_actuator_init(const opendash_parachute_actuator_config_t *cfg);

/** Arm / disarm. Disarming forces the output de-energized (inhibit). */
void opendash_parachute_actuator_set_armed(bool armed);

/** True if currently armed. */
bool opendash_parachute_actuator_is_armed(void);

/**
 * @brief Fire the deploy charge.
 * @return ESP_OK if energized (or already deployed);
 *         ESP_ERR_INVALID_STATE if disarmed or GPIO unassigned (inhibited).
 */
esp_err_t opendash_parachute_actuator_fire(opendash_parachute_reason_t reason);

/** True once a deploy has fired (latched until cleared). */
bool opendash_parachute_actuator_is_deployed(void);

/** Current actuator state. */
opendash_parachute_act_state_t opendash_parachute_actuator_state(void);

/** Clear a post-deploy lockout. Requires the actuator to be DISARMED first. */
esp_err_t opendash_parachute_actuator_clear(void);

/* ════════════════════════════════════════════════════════════════════════════
 * 11. DEPLOYMENT CONFIG  (center-pushed, MOS-persisted)  — opendash_parachute.c
 *
 * The MOS node owns the authoritative deployment configuration and persists it
 * to NVS. Center pushes updates (OPENDASH_CMD_PARACHUTE_SET_CONFIG) and reads
 * back a STATUS echo (OPENDASH_CMD_PARACHUTE_STATUS) for verification.
 *
 * NOTE: ARM state is intentionally NOT part of this config and is NOT
 * persisted — a reboot always comes up DISARMED for safety.
 * ════════════════════════════════════════════════════════════════════════════ */

/** @brief Center-pushed, MOS-persisted deployment configuration.
 *  Doubles as the ESP-NOW wire payload and the NVS blob — keep packed and
 *  versioned. Deploy fires the selected MOS power channel(s), not a separate
 *  GPIO, so @ref channel_mask selects which of CH1..CH4 energize on deploy. */
typedef struct __attribute__((packed)) {
    uint8_t  version;          /**< OPENDASH_PARACHUTE_CONFIG_VERSION */
    uint8_t  enabled;          /**< 0 = system off, 1 = active on this MOS */
    uint8_t  channel_mask;     /**< bit0..bit3 = CH1..CH4 fired on deploy */
    uint8_t  flags;            /**< OPENDASH_PARACHUTE_FLAG_* (bit0 = pulse, else latch) */
    float    min_speed_mph;    /**< deploy speed gate (mph) */
    float    roll_deploy_deg;  /**< roll/tilt deploy angle (deg) */
    float    roll_rate_deg_s;  /**< roll-rate deploy threshold (deg/s) */
    uint16_t sustain_ms;       /**< roll must hold this long past the angle */
    uint16_t pulse_ms;         /**< deploy energize duration (ms) */
} opendash_parachute_config_t;

/** @brief MOS→center status echo: persisted config + live actuator state. */
typedef struct __attribute__((packed)) {
    opendash_parachute_config_t cfg;  /**< current persisted config           */
    uint8_t  act_state;        /**< opendash_parachute_act_state_t            */
    uint8_t  armed;            /**< 0/1 — live arm state                      */
    uint8_t  deployed;         /**< 0/1 — fired/latched                       */
    uint8_t  reserved;         /**< pad (0)                                   */
} opendash_parachute_status_t;

/** @brief Detector→center rollover VOTE (OPENDASH_CMD_PARACHUTE_VOTE, broadcast).
 *
 *  A gyro-equipped detector node (RIGHT, POD1, POD2, …) broadcasts this whenever
 *  its local roll-detection state changes, and periodically refreshes it while a
 *  roll is active. Center tallies fresh `rolling==1` votes from the REQUIRED
 *  detectors; only a unanimous, un-expired set (plus the armed/enabled interlock)
 *  triggers an autonomous deploy. Broadcast (not unicast) so the safety signal is
 *  independent of which center MAC the detector has learned.
 *
 *  `manual` is a hard operator override: a physical release button on the detector.
 *  Center treats a manual==1 vote like the on-screen HOLD-TO-DEPLOY (still
 *  interlocked by armed + enabled + online), bypassing the unanimous quorum. */
typedef struct __attribute__((packed)) {
    uint8_t  node;        /**< opendash_node_t source (also self-identifies the sender) */
    uint8_t  rolling;     /**< 1 = local roll-detect active, 0 = clear                  */
    uint8_t  manual;      /**< 1 = physical manual-release button held (operator fire)  */
    uint8_t  reason;      /**< opendash_parachute_reason_t                              */
    float    roll_deg;    /**< live |roll| angle (deg) at vote time                     */
    float    roll_rate;   /**< live roll rate (deg/s) at vote time                      */
    uint32_t seq;         /**< monotonic counter (debug / dedupe)                       */
} opendash_parachute_vote_t;

/** Populate @p cfg with the safe factory defaults (disabled, no channels). */
void opendash_parachute_config_default(opendash_parachute_config_t *cfg);

/** Load the persisted config from NVS, or install + save defaults if absent.
 *  Call once at boot (after nvs_flash_init). */
esp_err_t opendash_parachute_config_init(void);

/** Copy the current in-RAM config into @p out. */
esp_err_t opendash_parachute_config_get(opendash_parachute_config_t *out);

/** Replace the config (validated), update RAM, and persist to NVS. */
esp_err_t opendash_parachute_config_set(const opendash_parachute_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* OPENDASH_PARACHUTE_H */
