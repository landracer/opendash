/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_relay.c
 * @brief OpenDash Relay / MOS Controller — Implementation
 *
 * GPIO-based relay and MOS FET control. Supports:
 *   - On/off relay control (active-low or active-high)
 *   - PWM duty cycle for MOS FET modules via LEDC
 *   - Emergency all-off
 *   - State reporting back to center via ESP-NOW
 *
 * GPIO pins are configurable — set to -1 for unassigned channels.
 * Discovery of actual pin assignments required when hardware arrives.
 */

#include "opendash_relay.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "opendash_relay";

static opendash_relay_config_t s_config;
static opendash_relay_channel_state_t s_states[OPENDASH_RELAY_MAX_CHANNELS];
static bool s_initialized = false;

esp_err_t opendash_relay_init(const opendash_relay_config_t *config)
{
    if (config == NULL) return ESP_ERR_INVALID_ARG;
    if (config->num_channels == 0 || config->num_channels > OPENDASH_RELAY_MAX_CHANNELS) {
        ESP_LOGE(TAG, "Invalid channel count: %d", config->num_channels);
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(&s_config, config, sizeof(opendash_relay_config_t));
    memset(s_states, 0, sizeof(s_states));

    /* Configure LEDC timer for MOS FET PWM (if applicable) */
    if (s_config.type == OPENDASH_RELAY_TYPE_MOS_FET && s_config.pwm_freq_hz > 0) {
        ledc_timer_config_t timer_conf = {
            .speed_mode       = LEDC_LOW_SPEED_MODE,
            .duty_resolution  = LEDC_TIMER_8_BIT,  /* 0-255 duty */
            .timer_num        = LEDC_TIMER_0,
            .freq_hz          = s_config.pwm_freq_hz,
            .clk_cfg          = LEDC_AUTO_CLK,
        };
        esp_err_t ret = ledc_timer_config(&timer_conf);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "LEDC timer config failed: %s", esp_err_to_name(ret));
            return ret;
        }
        ESP_LOGI(TAG, "LEDC timer configured: %lu Hz, 8-bit resolution",
                 (unsigned long)s_config.pwm_freq_hz);
    }

    /* Configure each channel */
    for (int i = 0; i < s_config.num_channels; i++) {
        int gpio = s_config.channels[i].gpio_num;
        if (gpio < 0 || !s_config.channels[i].enabled) {
            ESP_LOGW(TAG, "Channel %d: not assigned (gpio=%d)", i, gpio);
            continue;
        }

        if (s_config.type == OPENDASH_RELAY_TYPE_MOS_FET && s_config.pwm_freq_hz > 0) {
            /* MOS FET with PWM: use LEDC */
            ledc_channel_config_t ch_conf = {
                .gpio_num   = gpio,
                .speed_mode = LEDC_LOW_SPEED_MODE,
                .channel    = (ledc_channel_t)i,
                .timer_sel  = LEDC_TIMER_0,
                .duty       = 0,  /* Start OFF */
                .hpoint     = 0,
            };
            esp_err_t ret = ledc_channel_config(&ch_conf);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "LEDC channel %d (GPIO %d) config failed: %s",
                         i, gpio, esp_err_to_name(ret));
                continue;
            }
            ESP_LOGI(TAG, "Channel %d: GPIO %d — MOS FET PWM [%s]",
                     i, gpio, s_config.channels[i].label);
        } else {
            /* Standard relay or MOS on/off: simple GPIO */
            gpio_config_t io_conf = {
                .pin_bit_mask = (1ULL << gpio),
                .mode         = GPIO_MODE_OUTPUT,
                .pull_up_en   = GPIO_PULLUP_DISABLE,
                .pull_down_en = GPIO_PULLDOWN_DISABLE,
                .intr_type    = GPIO_INTR_DISABLE,
            };
            esp_err_t ret = gpio_config(&io_conf);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "GPIO %d config failed: %s", gpio, esp_err_to_name(ret));
                continue;
            }

            /* Set initial OFF state */
            int off_level = (s_config.polarity == OPENDASH_RELAY_ACTIVE_LOW) ? 1 : 0;
            gpio_set_level((gpio_num_t)gpio, off_level);

            ESP_LOGI(TAG, "Channel %d: GPIO %d — Relay %s [%s]",
                     i, gpio,
                     s_config.polarity == OPENDASH_RELAY_ACTIVE_LOW ? "active-LOW" : "active-HIGH",
                     s_config.channels[i].label);
        }
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Relay controller initialized: %d channels, type=%s",
             s_config.num_channels,
             s_config.type == OPENDASH_RELAY_TYPE_MOS_FET ? "MOS_FET" : "RELAY");
    return ESP_OK;
}

esp_err_t opendash_relay_set(uint8_t channel, bool on)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (channel >= s_config.num_channels) return ESP_ERR_INVALID_ARG;
    if (s_config.channels[channel].gpio_num < 0) return ESP_ERR_NOT_SUPPORTED;

    int gpio = s_config.channels[channel].gpio_num;

    if (s_config.type == OPENDASH_RELAY_TYPE_MOS_FET && s_config.pwm_freq_hz > 0) {
        uint32_t duty = on ? 255 : 0;
        ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)channel, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)channel);
        s_states[channel].pwm_duty = on ? 255 : 0;
    } else {
        int level;
        if (s_config.polarity == OPENDASH_RELAY_ACTIVE_LOW) {
            level = on ? 0 : 1;
        } else {
            level = on ? 1 : 0;
        }
        gpio_set_level((gpio_num_t)gpio, level);
    }

    s_states[channel].is_on = on;
    ESP_LOGI(TAG, "Channel %d [%s] → %s",
             channel, s_config.channels[channel].label, on ? "ON" : "OFF");
    return ESP_OK;
}

esp_err_t opendash_relay_set_pwm(uint8_t channel, uint8_t duty)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (channel >= s_config.num_channels) return ESP_ERR_INVALID_ARG;
    if (s_config.channels[channel].gpio_num < 0) return ESP_ERR_NOT_SUPPORTED;

    if (s_config.type == OPENDASH_RELAY_TYPE_MOS_FET && s_config.pwm_freq_hz > 0) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)channel, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)channel);
        s_states[channel].pwm_duty = duty;
        s_states[channel].is_on = (duty > 0);
    } else {
        /* Relay mode: treat any duty > 0 as ON */
        return opendash_relay_set(channel, duty > 0);
    }

    ESP_LOGD(TAG, "Channel %d [%s] PWM → %d/255",
             channel, s_config.channels[channel].label, duty);
    return ESP_OK;
}

esp_err_t opendash_relay_get_state(uint8_t channel, opendash_relay_channel_state_t *state)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (channel >= s_config.num_channels || state == NULL) return ESP_ERR_INVALID_ARG;

    *state = s_states[channel];
    return ESP_OK;
}

esp_err_t opendash_relay_get_all_states(opendash_relay_channel_state_t *states, uint8_t *num_channels)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (states == NULL || num_channels == NULL) return ESP_ERR_INVALID_ARG;

    memcpy(states, s_states, sizeof(opendash_relay_channel_state_t) * s_config.num_channels);
    *num_channels = s_config.num_channels;
    return ESP_OK;
}

esp_err_t opendash_relay_all_off(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    ESP_LOGW(TAG, "EMERGENCY: All channels OFF");
    for (int i = 0; i < s_config.num_channels; i++) {
        if (s_config.channels[i].gpio_num >= 0 && s_config.channels[i].enabled) {
            opendash_relay_set(i, false);
        }
    }
    return ESP_OK;
}

const opendash_relay_config_t *opendash_relay_get_config(void)
{
    return s_initialized ? &s_config : NULL;
}
