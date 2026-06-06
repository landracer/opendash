/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_boost.h
 * @brief OpenDash Active Boost Controller — per-gear maps, ESP-NOW transport.
 *
 * Architecture
 * ============
 *   Center (master)               MOS-4CH-A (slave)
 *   ───────────────               ─────────────────
 *   • Holds canonical config      • Runs PID + safety overlay
 *   • UI editor for maps          • Drives N75 PWM on configured channel
 *   • Pushes maps gear-by-gear    • Persists last-known config to NVS
 *     via ESP-NOW                 • Streams telemetry back at ~5 Hz
 *   • Sends live engine data
 *     (RPM, boost, EGT, AFR,
 *      fuel pressure, throttle,
 *      gear) at ≥10 Hz
 *
 * Heritage
 * ========
 * Ported from Stephan Martin / Dominik Gummel's MultiDisplay
 * RPMBoostController (GPL-3.0). The wire format mirrors MD's per-gear
 * per-mode map upload pattern so the same maps can be authored in
 * either ecosystem.
 *
 * Wire transport
 * ==============
 *   ALL boost frames travel over ESP-NOW (no I2C, no UART). The opcodes
 *   live in opendash_i2c_protocol.h purely because that header is the
 *   shared wire-format catalogue (legacy filename) — the bytes go out
 *   on opendash_espnow_send().
 *
 * Fail-safe
 * =========
 *   If live data is older than OPENDASH_BOOST_DATA_TIMEOUT_MS the output
 *   collapses to 0 (wastegate fully open). Same on lean AFR, low fuel
 *   pressure, mode==OFF, or throttle/RPM below the gating thresholds.
 */

#ifndef OPENDASH_BOOST_H
#define OPENDASH_BOOST_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ────────────────────────────────────────────────────────────────────────────
 * Sizing / Limits
 * ──────────────────────────────────────────────────────────────────────────── */

#define OPENDASH_BOOST_GEARS            6     /**< Number of gear maps */
#define OPENDASH_BOOST_MAP_POINTS       16    /**< RPM points per gear map */
#define OPENDASH_BOOST_MODES            3     /**< LOW + MED + HIGH map slots */
#define OPENDASH_BOOST_RPM_MIN          0
#define OPENDASH_BOOST_RPM_MAX          8000  /**< Highest RPM in the map */
#define OPENDASH_BOOST_DATA_TIMEOUT_MS  600   /**< Stale-data lockout */
#define OPENDASH_BOOST_NVS_NAMESPACE    "boost"

/* ────────────────────────────────────────────────────────────────────────────
 * Modes
 * ──────────────────────────────────────────────────────────────────────────── */

typedef enum {
    OPENDASH_BOOST_MODE_OFF    = 0,  /**< Bypass — output forced to 0 */
    OPENDASH_BOOST_MODE_LOW    = 1,  /**< Conservative low-boost map (street) */
    OPENDASH_BOOST_MODE_MED    = 2,  /**< Balanced mid-boost map (spirited / mixed conditions) */
    OPENDASH_BOOST_MODE_HIGH   = 3,  /**< Aggressive high-boost map (track / race fuel) */

    /* Backward-compat aliases for v1 callers — do not use in new code */
    OPENDASH_BOOST_MODE_NORMAL = OPENDASH_BOOST_MODE_LOW,
    OPENDASH_BOOST_MODE_RACE   = OPENDASH_BOOST_MODE_HIGH,
} opendash_boost_mode_t;

/* Map slot index for the [mode] axis: LOW=0, MED=1, HIGH=2.
 * OFF (0) collapses to duty 0 and bypasses the map lookup entirely. */
#define OPENDASH_BOOST_SLOT_LOW    0
#define OPENDASH_BOOST_SLOT_MED    1
#define OPENDASH_BOOST_SLOT_HIGH   2
/* Aliases for v1 source compat. */
#define OPENDASH_BOOST_SLOT_NORMAL OPENDASH_BOOST_SLOT_LOW
#define OPENDASH_BOOST_SLOT_RACE   OPENDASH_BOOST_SLOT_HIGH

/* ────────────────────────────────────────────────────────────────────────────
 * Scalar Parameters (single ESP-NOW frame)
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief All non-map parameters. Fits comfortably in one ESP-NOW frame.
 *
 * Pushed via OPENDASH_CMD_BOOST_SET_PARAMS, returned via
 * OPENDASH_CMD_BOOST_PARAMS_REPORT. Persisted to NVS as a single blob.
 */
typedef struct __attribute__((packed)) {
    uint16_t version;                /**< OPENDASH_BOOST_PARAMS_VERSION */
    uint8_t  mode;                   /**< opendash_boost_mode_t */
    uint8_t  use_pid;                /**< 0 = open-loop duty, 1 = PID overlay */
    uint8_t  output_channel;         /**< MOS channel feeding the N75 (0..3) */
    uint8_t  reserved0[3];

    /* PID gains — aggressive (large error from setpoint) */
    float    aKp;
    float    aKi;
    float    aKd;
    /* PID gains — conservative (close to setpoint) */
    float    cKp;
    float    cKi;
    float    cKd;
    /* PID activation thresholds (factor of setpoint) */
    float    aggressive_threshold;   /**< actual > setpoint*this → PID on */
    float    conservative_threshold; /**< actual > setpoint*this → use cKp/Ki/Kd */

    /* Safety limits */
    float    overboost_bar;          /**< Hard overboost cut (×0.5) */
    float    egt_warn_c;             /**< Soft EGT pull (×0.75) */
    float    egt_critical_c;         /**< Hard EGT pull (×0.5) */
    float    afr_lean_limit;         /**< AFR > this → cut to 0 */
    float    fuel_pressure_min_kpa;  /**< Fuel pressure < this → cut to 0 */

    /* Gating */
    uint8_t  throttle_min_pct;       /**< Boost only flows above this throttle */
    uint8_t  reserved1;
    uint16_t rpm_min;                /**< Minimum RPM for non-zero output */
} opendash_boost_params_t;

#define OPENDASH_BOOST_PARAMS_VERSION  2

/* ────────────────────────────────────────────────────────────────────────────
 * Per-(mode,gear) Map Rows (one ESP-NOW frame each)
 * ──────────────────────────────────────────────────────────────────────────── */

/** @brief Duty cycle row payload: [mode:1][gear:1][16 × uint8]. */
typedef struct __attribute__((packed)) {
    uint8_t mode;                                    /**< OPENDASH_BOOST_SLOT_* */
    uint8_t gear;                                    /**< 0..GEARS-1 */
    uint8_t duty[OPENDASH_BOOST_MAP_POINTS];         /**< 0..255 PWM */
} opendash_boost_duty_row_t;

/** @brief Setpoint row payload: [mode:1][gear:1][16 × uint16 cBar]. */
typedef struct __attribute__((packed)) {
    uint8_t  mode;                                   /**< OPENDASH_BOOST_SLOT_* */
    uint8_t  gear;                                   /**< 0..GEARS-1 */
    uint16_t setpoint_cbar[OPENDASH_BOOST_MAP_POINTS]; /**< centi-bar */
} opendash_boost_setpoint_row_t;

/** @brief Throttle reduction curve (16 × uint16, fixed-point ×1000). */
typedef struct __attribute__((packed)) {
    uint16_t reduction_x1000[OPENDASH_BOOST_MAP_POINTS]; /**< 0..1000 = 0..100% */
} opendash_boost_throttle_curve_t;

/* ────────────────────────────────────────────────────────────────────────────
 * Live Engine Data Feed
 * ──────────────────────────────────────────────────────────────────────────── */

/** @brief Snapshot pushed Center→MOS at ≥10 Hz via OPENDASH_CMD_BOOST_LIVE_DATA. */
typedef struct __attribute__((packed)) {
    uint16_t rpm;                    /**< Engine RPM */
    int16_t  boost_cbar;             /**< Manifold pressure, centi-bar gauge */
    int16_t  egt_c;                  /**< Hottest EGT, °C */
    uint16_t afr_x10;                /**< AFR ×10 (e.g. 145 = 14.5:1) */
    uint16_t fuel_press_kpa;         /**< Fuel rail pressure, kPa */
    uint8_t  throttle_pct;           /**< 0..100 */
    uint8_t  gear;                   /**< 1..GEARS, 0 = unknown/neutral */
} opendash_boost_live_t;

/* ────────────────────────────────────────────────────────────────────────────
 * Telemetry Reported Back
 * ──────────────────────────────────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint8_t  mode;                   /**< Current opendash_boost_mode_t */
    uint8_t  pid_active;             /**< 1 = PID engaged */
    uint8_t  aggressive;             /**< 1 = aggressive PID gains in use */
    uint8_t  safety_flags;           /**< Bitmask of OPENDASH_BOOST_SAFE_* */
    uint16_t setpoint_cbar;          /**< Current setpoint */
    int16_t  boost_cbar;             /**< Last seen manifold pressure */
    uint16_t rpm;                    /**< Last seen RPM */
    uint8_t  gear;                   /**< Last seen gear */
    uint8_t  duty;                   /**< Final PWM duty 0..255 */
} opendash_boost_telemetry_t;

/* Safety attenuation flags */
#define OPENDASH_BOOST_SAFE_OVERBOOST   (1u << 0)
#define OPENDASH_BOOST_SAFE_EGT_WARN    (1u << 1)
#define OPENDASH_BOOST_SAFE_EGT_CRIT    (1u << 2)
#define OPENDASH_BOOST_SAFE_AFR_LEAN    (1u << 3)
#define OPENDASH_BOOST_SAFE_FUEL_LOW    (1u << 4)
#define OPENDASH_BOOST_SAFE_DATA_STALE  (1u << 5)
#define OPENDASH_BOOST_SAFE_THROTTLE    (1u << 6)
#define OPENDASH_BOOST_SAFE_MODE_OFF    (1u << 7)

/* ────────────────────────────────────────────────────────────────────────────
 * Slave-side API (mos-4ch-a)
 * ──────────────────────────────────────────────────────────────────────────── */

/** @brief Populate @p p with built-in safe defaults. */
void opendash_boost_default_params(opendash_boost_params_t *p);

/** @brief Populate a duty row with a sensible spool→peak→taper curve. */
void opendash_boost_default_duty_row(uint8_t mode, uint8_t gear,
                                      uint8_t out[OPENDASH_BOOST_MAP_POINTS]);

/** @brief Populate a setpoint row with the matching default target curve. */
void opendash_boost_default_setpoint_row(uint8_t mode, uint8_t gear,
                                          uint16_t out[OPENDASH_BOOST_MAP_POINTS]);

/** @brief Initialize the controller. Loads params + maps from NVS if present,
 *  otherwise installs defaults and persists them. */
esp_err_t opendash_boost_init(void);

/** @brief Replace scalar params and persist. */
esp_err_t opendash_boost_set_params(const opendash_boost_params_t *p);

/** @brief Snapshot scalar params. */
esp_err_t opendash_boost_get_params(opendash_boost_params_t *out);

/** @brief Install a single duty row [mode][gear]. Persists incrementally. */
esp_err_t opendash_boost_set_duty_row(uint8_t mode, uint8_t gear,
                                       const uint8_t duty[OPENDASH_BOOST_MAP_POINTS]);

/** @brief Install a single setpoint row [mode][gear]. */
esp_err_t opendash_boost_set_setpoint_row(uint8_t mode, uint8_t gear,
                                           const uint16_t setpoint_cbar[OPENDASH_BOOST_MAP_POINTS]);

/** @brief Read back a duty row. */
esp_err_t opendash_boost_get_duty_row(uint8_t mode, uint8_t gear,
                                       uint8_t out[OPENDASH_BOOST_MAP_POINTS]);

/** @brief Read back a setpoint row. */
esp_err_t opendash_boost_get_setpoint_row(uint8_t mode, uint8_t gear,
                                           uint16_t out[OPENDASH_BOOST_MAP_POINTS]);

/** @brief Replace the throttle reduction curve. */
esp_err_t opendash_boost_set_throttle_curve(const opendash_boost_throttle_curve_t *c);

/** @brief Read the throttle reduction curve. */
esp_err_t opendash_boost_get_throttle_curve(opendash_boost_throttle_curve_t *out);

/** @brief Feed a fresh live-data sample. Resets the stale-data timer. */
void opendash_boost_feed_live(const opendash_boost_live_t *live);

/** @brief Run one PID + safety compute step. Call at ≥20 Hz. */
uint8_t opendash_boost_compute(opendash_boost_telemetry_t *out_telem);

/** @brief Latest telemetry without recomputing. */
void opendash_boost_get_telemetry(opendash_boost_telemetry_t *out);

#ifdef __cplusplus
}
#endif

#endif /* OPENDASH_BOOST_H */
