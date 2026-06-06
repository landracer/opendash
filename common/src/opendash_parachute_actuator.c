/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_parachute_actuator.c
 * @brief OpenDash Parachute deploy actuator — shared implementation.
 *
 * Generic, hardware-agnostic deploy output used by the MOS-4CH actuator
 * nodes (and available to any node). The physical GPIO is supplied per node
 * via its parachute_gpio.h, so MOS-4CH-A and MOS-4CH-B share this exact code.
 *
 * SAFETY MODEL:
 *   - Output is de-energized on init and whenever DISARMED.
 *   - fire() is HARD-INHIBITED unless ARMED and a valid GPIO is assigned.
 *   - On fire: energize for `pulse_ms` (one-shot esp_timer), then de-energize;
 *     state latches DEPLOYED (lockout) until explicitly cleared while disarmed.
 *
 * @see opendash_parachute.h for all tunable thresholds (single source of truth)
 * @see TODO.md §11.7 for the full deployment-system design
 */

#include "opendash_parachute.h"

#include <string.h>
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "parachute_act";

static struct {
    opendash_parachute_actuator_config_t cfg;
    opendash_parachute_act_state_t       state;
    bool                                 armed;
    bool                                 deployed;
    int64_t                              deploy_time_us;
    esp_timer_handle_t                   pulse_timer;
} s = {
    .cfg   = { .gpio_num = -1, .active_high = true, .pulse_ms = OPENDASH_PARACHUTE_DEPLOY_PULSE_MS },
    .state = OPENDASH_PARACHUTE_ACT_UNINIT,
};

/** Drive the output to its de-energized (safe) level. */
static void drive_safe(void)
{
    if (s.cfg.gpio_num < 0) return;
    gpio_set_level((gpio_num_t)s.cfg.gpio_num, s.cfg.active_high ? 0 : 1);
}

/** Drive the output to its energized (deploy) level. */
static void drive_active(void)
{
    if (s.cfg.gpio_num < 0) return;
    gpio_set_level((gpio_num_t)s.cfg.gpio_num, s.cfg.active_high ? 1 : 0);
}

/** One-shot pulse expiry: de-energize but keep the DEPLOYED lockout latched. */
static void pulse_expired_cb(void *arg)
{
    (void)arg;
    drive_safe();
    ESP_LOGW(TAG, "Deploy pulse complete — output de-energized, DEPLOYED latched");
}

esp_err_t opendash_parachute_actuator_init(const opendash_parachute_actuator_config_t *cfg)
{
    if (cfg == NULL) return ESP_ERR_INVALID_ARG;

    s.cfg            = *cfg;
    s.armed          = false;
    s.deployed       = false;
    s.deploy_time_us = 0;

    if (s.cfg.gpio_num < 0) {
        s.state = OPENDASH_PARACHUTE_ACT_SAFE;
        ESP_LOGW(TAG, "Deploy GPIO unassigned (-1) — actuator INHIBITED (no output). "
                      "Assign on the bench in this node's parachute_gpio.h");
        return ESP_OK;
    }

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << (uint32_t)s.cfg.gpio_num),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config(GPIO%d) failed: %s", s.cfg.gpio_num, esp_err_to_name(ret));
        s.state = OPENDASH_PARACHUTE_ACT_UNINIT;
        return ret;
    }
    drive_safe();

    if (s.pulse_timer == NULL && s.cfg.pulse_ms > 0) {
        const esp_timer_create_args_t targs = {
            .callback = pulse_expired_cb,
            .name     = "para_pulse",
        };
        esp_timer_create(&targs, &s.pulse_timer);
    }

    s.state = OPENDASH_PARACHUTE_ACT_SAFE;
    ESP_LOGI(TAG, "Deploy actuator init: GPIO%d active-%s, pulse=%ums — SAFE/DISARMED",
             s.cfg.gpio_num, s.cfg.active_high ? "HIGH" : "LOW", s.cfg.pulse_ms);
    return ESP_OK;
}

void opendash_parachute_actuator_set_armed(bool armed)
{
    if (s.state == OPENDASH_PARACHUTE_ACT_UNINIT) return;
    if (s.deployed) return;  /* lockout: arm state irrelevant once deployed */

    s.armed = armed;
    if (!armed) {
        drive_safe();  /* disarm always inhibits the output */
        s.state = OPENDASH_PARACHUTE_ACT_SAFE;
    } else {
        s.state = OPENDASH_PARACHUTE_ACT_ARMED;
    }
    ESP_LOGW(TAG, "Actuator %s", armed ? "ARMED" : "DISARMED");
}

bool opendash_parachute_actuator_is_armed(void)
{
    return s.armed;
}

esp_err_t opendash_parachute_actuator_fire(opendash_parachute_reason_t reason)
{
    if (s.deployed) {
        return ESP_OK;  /* idempotent — already fired */
    }
    if (s.cfg.gpio_num < 0) {
        ESP_LOGE(TAG, "FIRE blocked: no deploy GPIO assigned");
        return ESP_ERR_INVALID_STATE;
    }
    if (!s.armed) {
        ESP_LOGW(TAG, "FIRE blocked: actuator DISARMED (reason=%d)", (int)reason);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGW(TAG, "!!! PARACHUTE DEPLOY — reason=%d, GPIO%d HOT !!!",
             (int)reason, s.cfg.gpio_num);
    drive_active();
    s.deployed       = true;
    s.deploy_time_us = esp_timer_get_time();
    s.state          = OPENDASH_PARACHUTE_ACT_DEPLOYED;

    if (s.cfg.pulse_ms > 0 && s.pulse_timer != NULL) {
        esp_timer_start_once(s.pulse_timer, (uint64_t)s.cfg.pulse_ms * 1000ULL);
    }
    return ESP_OK;
}

bool opendash_parachute_actuator_is_deployed(void)
{
    return s.deployed;
}

opendash_parachute_act_state_t opendash_parachute_actuator_state(void)
{
    return s.state;
}

esp_err_t opendash_parachute_actuator_clear(void)
{
    if (s.armed) {
        ESP_LOGW(TAG, "Clear refused: disarm before clearing deploy lockout");
        return ESP_ERR_INVALID_STATE;
    }
    if (s.pulse_timer != NULL) {
        esp_timer_stop(s.pulse_timer);
    }
    drive_safe();
    s.deployed       = false;
    s.deploy_time_us = 0;
    s.state          = OPENDASH_PARACHUTE_ACT_SAFE;
    ESP_LOGI(TAG, "Deploy lockout cleared — SAFE");
    return ESP_OK;
}
