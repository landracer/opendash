/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_relay.h
 * @brief OpenDash Relay / MOS Controller — Common Driver
 *
 * Shared relay and MOS FET control logic used by all relay controller nodes.
 * Supports both on/off relays and PWM-capable MOS FET modules.
 *
 * GPIO pin assignments are left configurable (define at compile time or
 * runtime) because the actual pinouts of these ESP32 relay modules need
 * to be discovered with a multimeter when the hardware arrives.
 *
 * Supported hardware:
 *   - 4-channel HD relay ESP32 module (high-amp: fans, pumps)
 *     http://www.chinalctech.com/cpzx/Programmer/Relay_Module/788.html
 *   - 8-channel relay ESP32 module (low-amp devices)
 *     http://www.chinalctech.com/cpzx/Programmer/Relay_Module/540.html
 *   - 4-channel MOS FET ESP32 module (PWM or on/off)
 *     http://www.chinalctech.com/cpzx/Programmer/Relay_Module/867.html
 */

#ifndef OPENDASH_RELAY_H
#define OPENDASH_RELAY_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum channels supported per controller module. */
#define OPENDASH_RELAY_MAX_CHANNELS  8

/** @brief Relay controller type. */
typedef enum {
    OPENDASH_RELAY_TYPE_RELAY     = 0,  /**< Standard relay (on/off only) */
    OPENDASH_RELAY_TYPE_MOS_FET   = 1,  /**< MOS FET (supports PWM duty cycle) */
} opendash_relay_type_t;

/** @brief Active-low or active-high relay logic. */
typedef enum {
    OPENDASH_RELAY_ACTIVE_LOW  = 0,   /**< GPIO LOW = relay ON (most relay modules) */
    OPENDASH_RELAY_ACTIVE_HIGH = 1,   /**< GPIO HIGH = relay ON */
} opendash_relay_polarity_t;

/** @brief Single channel configuration. */
typedef struct {
    int      gpio_num;       /**< GPIO pin number (-1 = not assigned) */
    bool     enabled;        /**< Whether this channel is configured */
    char     label[24];      /**< Human-readable label (e.g., "RAD FAN 1") */
} opendash_relay_channel_config_t;

/** @brief Relay controller configuration. */
typedef struct {
    opendash_relay_type_t     type;         /**< Relay or MOS FET */
    opendash_relay_polarity_t polarity;     /**< Active-low or active-high */
    uint8_t                   num_channels; /**< Number of channels (1-8) */
    uint32_t                  pwm_freq_hz;  /**< PWM frequency for MOS FET (0 = on/off only) */
    opendash_relay_channel_config_t channels[OPENDASH_RELAY_MAX_CHANNELS];
} opendash_relay_config_t;

/** @brief Channel runtime state. */
typedef struct {
    bool    is_on;          /**< Current on/off state */
    uint8_t pwm_duty;       /**< PWM duty cycle 0-255 (MOS FET only) */
} opendash_relay_channel_state_t;

/**
 * @brief Initialize the relay controller.
 *
 * Configures GPIO pins (and LEDC PWM channels for MOS FET modules).
 * All channels start in OFF state.
 *
 * @param[in] config  Pointer to relay controller configuration.
 * @return ESP_OK on success.
 */
esp_err_t opendash_relay_init(const opendash_relay_config_t *config);

/**
 * @brief Set a channel ON or OFF.
 *
 * @param[in] channel  Channel index (0-based).
 * @param[in] on       true = ON, false = OFF.
 * @return ESP_OK on success.
 */
esp_err_t opendash_relay_set(uint8_t channel, bool on);

/**
 * @brief Set MOS FET channel PWM duty cycle.
 *
 * Only valid for OPENDASH_RELAY_TYPE_MOS_FET controllers.
 * For relay-type controllers, duty > 0 is treated as ON.
 *
 * @param[in] channel  Channel index (0-based).
 * @param[in] duty     Duty cycle 0-255 (0 = OFF, 255 = 100%).
 * @return ESP_OK on success.
 */
esp_err_t opendash_relay_set_pwm(uint8_t channel, uint8_t duty);

/**
 * @brief Get channel state.
 *
 * @param[in]  channel  Channel index (0-based).
 * @param[out] state    Pointer to state structure to fill.
 * @return ESP_OK on success.
 */
esp_err_t opendash_relay_get_state(uint8_t channel, opendash_relay_channel_state_t *state);

/**
 * @brief Get all channel states at once.
 *
 * @param[out] states      Array of state structures (must be at least num_channels).
 * @param[out] num_channels  Number of channels populated.
 * @return ESP_OK on success.
 */
esp_err_t opendash_relay_get_all_states(opendash_relay_channel_state_t *states, uint8_t *num_channels);

/**
 * @brief Emergency: turn all channels OFF immediately.
 *
 * @return ESP_OK on success.
 */
esp_err_t opendash_relay_all_off(void);

/**
 * @brief Get the stored configuration.
 *
 * @return Pointer to current configuration, or NULL if not initialized.
 */
const opendash_relay_config_t *opendash_relay_get_config(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENDASH_RELAY_H */
