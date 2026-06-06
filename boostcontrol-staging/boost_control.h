/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file boost_control.h
 * @brief OpenDash Boost Control System
 *
 * Implements advanced boost control for turbocharged engines with PID control,
 * safety protections, and integration with OpenDash data model.
 */

#ifndef BOOST_CONTROL_H
#define BOOST_CONTROL_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "opendash_common.h"
#include "opendash_data_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ────────────────────────────────────────────────────────────────────────────
 * Configuration
 * ──────────────────────────────────────────────────────────────────────────── */

/** @brief Maximum number of gears supported */
#define BOOST_CONTROL_MAX_GEARS  8

/** @brief Maximum RPM for boost control */
#define BOOST_CONTROL_MAX_RPM    10000

/** @brief Minimum RPM for boost control */
#define BOOST_CONTROL_MIN_RPM    0

/** @brief Default boost limit in BAR */
#define BOOST_CONTROL_DEFAULT_MAX_BOOST  1.8f

/** @brief Default PID parameters for aggressive control */
#define BOOST_CONTROL_DEFAULT_A_KP  4.0f
#define BOOST_CONTROL_DEFAULT_A_KI  1.0f
#define BOOST_CONTROL_DEFAULT_A_KD  0.2f

/** @brief Default PID parameters for conservative control */
#define BOOST_CONTROL_DEFAULT_C_KP  1.0f
#define BOOST_CONTROL_DEFAULT_C_KI  0.25f
#define BOOST_CONTROL_DEFAULT_C_KD  0.05f

/** @brief Default activation thresholds */
#define BOOST_CONTROL_DEFAULT_A_ACTIVATE_THRESHOLD  0.5f
#define BOOST_CONTROL_DEFAULT_C_ACTIVATE_THRESHOLD  0.85f

/* ────────────────────────────────────────────────────────────────────────────
 * Boost Control Modes
 * ──────────────────────────────────────────────────────────────────────────── */

/** @brief Boost control mode */
typedef enum {
    BOOST_MODE_NORMAL = 0,  /**< Normal driving mode */
    BOOST_MODE_RACE   = 1,  /**< Racing mode with more aggressive boost */
} boost_control_mode_t;

/* ────────────────────────────────────────────────────────────────────────────
 * Safety Protection Thresholds
 * ──────────────────────────────────────────────────────────────────────────── */

/** @brief EGT protection thresholds */
#define BOOST_CONTROL_EGT_YELLOW  960   /**< Yellow warning threshold (°C) */
#define BOOST_CONTROL_EGT_CRITICAL  975 /**< Critical threshold (°C) */

/** @brief EFR speed protection */
#define BOOST_CONTROL_EFR_SPEED_REDLINE  120000 /**< Redline speed (RPM) */

/** @brief Fuel pressure protection */
#define BOOST_CONTROL_MIN_FUEL_PRESSURE  200   /**< Minimum fuel pressure (kPa) */

/* ────────────────────────────────────────────────────────────────────────────
 * Boost Control Data Structures
 * ──────────────────────────────────────────────────────────────────────────── */

/** @brief Boost control configuration */
typedef struct {
    boost_control_mode_t mode;              /**< Current control mode */
    bool use_pid;                           /**< Enable PID control */

    /* PID Parameters */
    float aKp, aKi, aKd;                    /**< Aggressive PID parameters */
    float cKp, cKi, cKd;                    /**< Conservative PID parameters */
    float apidActivationThresholdFactor;    /**< Aggressive activation threshold */
    float cpidActivationThresholdFactor;    /**< Conservative activation threshold */

    /* Safety limits */
    float max_boost;                        /**< Maximum boost limit (BAR) */
    float min_fuel_pressure;                /**< Minimum fuel pressure (kPa) */

    /* Gear maps */
    uint8_t duty_cycle_map[BOOST_CONTROL_MAX_GEARS][16];    /**< Duty cycle maps */
    float setpoint_map[BOOST_CONTROL_MAX_GEARS][16];        /**< Setpoint maps */

    /* Throttle correction */
    float throttle_boost_reduction[16];     /**< Throttle correction factors */
} boost_control_config_t;

/** @brief Boost control state */
typedef struct {
    bool pid_active;                        /**< PID is currently active */
    bool aggressive_settings;               /**< Using aggressive PID settings */
    float pid_output;                       /**< PID computed output */
    float requested_boost;                  /**< Requested boost level */
    float boost_output;                     /**< Final boost output (0-255) */
    uint8_t current_gear;                   /**< Current gear */
    uint8_t current_rpm_index;              /**< Current RPM index */
} boost_control_state_t;

esp_err_t boost_control_init(const boost_control_config_t *config);
esp_err_t boost_control_update(const opendash_data_t *data);
esp_err_t boost_control_get_state(boost_control_state_t *state);
esp_err_t boost_control_get_config(boost_control_config_t *config);
esp_err_t boost_control_set_config(const boost_control_config_t *config);
esp_err_t boost_control_set_duty_cycle_map(uint8_t gear, uint8_t mode, const uint8_t *data);
esp_err_t boost_control_set_setpoint_map(uint8_t gear, uint8_t mode, const float *data);
esp_err_t boost_control_toggle_mode(boost_control_mode_t mode);
esp_err_t boost_control_get_output(uint8_t *output);

#ifdef __cplusplus
}
#endif

#endif /* BOOST_CONTROL_H */
