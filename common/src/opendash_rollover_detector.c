/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_rollover_detector.c
 * @brief Shared distributed rollover detector (RIGHT / POD1 / POD2).
 *
 * One implementation, linked by every gyro-equipped detector node. The node
 * provides a read callback (its IMU → roll angle + rate); this module runs the
 * detection state machine and broadcasts a VOTE frame the center fuses.
 *
 * Design notes:
 *   - Thresholds are pulled live from the center-pushed parachute config
 *     (roll_deploy_deg / sustain_ms / roll_rate_deg_s / enabled), so tuning is
 *     done from the center config screen with no reflash.
 *   - Votes are BROADCAST (not unicast): the safety signal does not depend on
 *     which center MAC the node has learned, sidestepping the "first bit7-clear
 *     frame" center-latch hazard entirely.
 *   - ZERO airtime when upright: a vote is emitted only on a state change, on a
 *     manual-button hold, and at a refresh cadence while a roll/manual is active.
 *   - The manual-release button is a HARD operator override (center fires on a
 *     single manual==1 vote, still interlocked by armed + enabled + online).
 */

#include "opendash_rollover.h"
#include "opendash_parachute.h"
#include "opendash_i2c_protocol.h"
#include "opendash_espnow.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "rollover_det";

#define ROLLOVER_NVS_NS   "rollover"
#define ROLLOVER_NVS_KEY  "roll_off"

static opendash_node_t            s_self = OPENDASH_NODE_COUNT;
static opendash_rollover_read_fn  s_read = NULL;

/* Detection state */
static bool     s_rolling        = false;
static bool     s_manual         = false;
static int64_t  s_above_since_us = 0;   /* when |roll| first crossed the deploy angle */
static int64_t  s_last_vote_us   = 0;
static uint32_t s_seq            = 0;

/* Zero/cal: roll baseline captured at the resting mounting angle (NVS-persisted)
 * and the most recent RAW roll sample, so a CALIBRATE can snapshot it. */
static float          s_roll_offset   = 0.0f;
static volatile float s_last_raw_roll = 0.0f;

/* Manual-release button */
static int      s_btn_gpio       = -1;
static int64_t  s_btn_since_us   = 0;

static int btn_gpio_for(opendash_node_t n)
{
    switch (n) {
        case OPENDASH_NODE_RIGHT: return OPENDASH_ROLLOVER_BTN_GPIO_RIGHT;
        case OPENDASH_NODE_POD1:  return OPENDASH_ROLLOVER_BTN_GPIO_POD1;
        case OPENDASH_NODE_POD2:  return OPENDASH_ROLLOVER_BTN_GPIO_POD2;
        default:                  return -1;
    }
}

static void offset_load(void)
{
    nvs_handle_t h;
    if (nvs_open(ROLLOVER_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    float    v  = 0.0f;
    size_t   sz = sizeof(v);
    if (nvs_get_blob(h, ROLLOVER_NVS_KEY, &v, &sz) == ESP_OK && sz == sizeof(v)) {
        s_roll_offset = v;
    }
    nvs_close(h);
}

static void offset_save(float v)
{
    nvs_handle_t h;
    if (nvs_open(ROLLOVER_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, ROLLOVER_NVS_KEY, &v, sizeof(v));
    nvs_commit(h);
    nvs_close(h);
}

static void broadcast_vote(bool rolling, bool manual,
                           float roll_deg, float roll_rate, uint8_t reason)
{
    opendash_parachute_vote_t v = {
        .node      = (uint8_t)s_self,
        .rolling   = rolling ? 1 : 0,
        .manual    = manual ? 1 : 0,
        .reason    = reason,
        .roll_deg  = roll_deg,
        .roll_rate = roll_rate,
        .seq       = ++s_seq,
    };

    opendash_i2c_msg_t m;
    opendash_i2c_build_msg(&m, OPENDASH_CMD_PARACHUTE_VOTE,
                           (const uint8_t *)&v, sizeof(v));

    uint8_t  buf[OPENDASH_ESPNOW_MAX_DATA];
    uint16_t len = 0;
    if (opendash_i2c_serialize(&m, buf, &len) == OPENDASH_OK) {
        opendash_espnow_broadcast(buf, len);
        s_last_vote_us = esp_timer_get_time();
    }
}

void opendash_rollover_detector_calibrate(void)
{
    if (s_self >= OPENDASH_NODE_COUNT) return;   /* detector not started yet */

    /* Snapshot the live resting roll as the new zero baseline + persist it. */
    s_roll_offset = s_last_raw_roll;
    offset_save(s_roll_offset);

    /* Drop any in-progress roll state so the fresh baseline takes effect now. */
    s_above_since_us = 0;
    s_rolling        = false;

    ESP_LOGW(TAG, "CALIBRATE: roll zeroed (offset=%.1f deg)", (double)s_roll_offset);

    /* Confirm the now-level state to center immediately, clearing any stale vote. */
    broadcast_vote(false, s_manual, 0.0f, 0.0f, OPENDASH_PARACHUTE_REASON_ROLL_ANGLE);
}

void opendash_rollover_detector_send_status(void)
{
    opendash_parachute_config_t cfg;
    if (opendash_parachute_config_get(&cfg) != ESP_OK) {
        opendash_parachute_config_default(&cfg);
    }

    /* A detector has no actuator; report SAFE/disarmed/not-deployed. Center
     * confirms the push by comparing the cfg portion only. */
    opendash_parachute_status_t st;
    memset(&st, 0, sizeof(st));
    st.cfg       = cfg;
    st.act_state = OPENDASH_PARACHUTE_ACT_SAFE;
    st.armed     = 0;
    st.deployed  = 0;

    opendash_i2c_msg_t m;
    opendash_i2c_build_msg(&m, OPENDASH_CMD_PARACHUTE_STATUS,
                           (const uint8_t *)&st, sizeof(st));

    uint8_t  buf[OPENDASH_ESPNOW_MAX_DATA];
    uint16_t len = 0;
    if (opendash_i2c_serialize(&m, buf, &len) == OPENDASH_OK) {
        opendash_espnow_broadcast(buf, len);
    }
}

static void evaluate(float roll_deg, float roll_rate)
{
    opendash_parachute_config_t cfg;
    if (opendash_parachute_config_get(&cfg) != ESP_OK) return;

    /* Measure roll relative to the calibrated resting baseline. */
    roll_deg -= s_roll_offset;

    const int64_t now = esp_timer_get_time();
    const float   a   = fabsf(roll_deg);
    const float   r   = fabsf(roll_rate);

    /* Hysteresis: trip at the deploy angle, only clear once below it minus the
     * hysteresis band — stops chatter right at the threshold. */
    const float on_deg  = cfg.roll_deploy_deg;
    const float off_deg = cfg.roll_deploy_deg - OPENDASH_PARACHUTE_TILT_HYSTERESIS_DEG;

    bool angle_trip = false;
    if (a >= on_deg) {
        if (s_above_since_us == 0) s_above_since_us = now;
        if ((now - s_above_since_us) / 1000 >= (int64_t)cfg.sustain_ms) angle_trip = true;
    } else if (a < off_deg) {
        s_above_since_us = 0;
    }

    /* Fast tip-over: roll RATE past threshold fires without waiting on sustain. */
    const bool rate_trip = (cfg.roll_rate_deg_s > 0.0f) && (r >= cfg.roll_rate_deg_s);

    uint8_t reason = rate_trip ? OPENDASH_PARACHUTE_REASON_ROLL_RATE
                               : OPENDASH_PARACHUTE_REASON_ROLL_ANGLE;

    bool now_rolling = (cfg.enabled != 0) && (angle_trip || rate_trip);
    /* Recovery: once rolling, only clear when truly back below the band. */
    if (s_rolling && a < off_deg && !rate_trip) now_rolling = false;
    if (cfg.enabled == 0) now_rolling = false;

    /* ── Manual-release button ─────────────────────────────────────────────── */
    bool now_manual = false;
    if (s_btn_gpio >= 0) {
        const bool pressed =
            (gpio_get_level(s_btn_gpio) == OPENDASH_ROLLOVER_BTN_ACTIVE_LEVEL);
        if (pressed) {
            if (s_btn_since_us == 0) s_btn_since_us = now;
            if ((now - s_btn_since_us) / 1000 >= (int64_t)OPENDASH_ROLLOVER_BTN_HOLD_MS) {
                now_manual = true;
            }
        } else {
            s_btn_since_us = 0;
        }
    }

    const bool changed   = (now_rolling != s_rolling) || (now_manual != s_manual);
    const bool active    = now_rolling || now_manual;
    const bool refresh   = active &&
        ((now - s_last_vote_us) / 1000 >= (int64_t)OPENDASH_ROLLOVER_VOTE_REFRESH_MS);

    if (now_rolling != s_rolling) {
        ESP_LOGW(TAG, "roll %s (|roll|=%.0f rate=%.0f thr=%.0f/%ums)",
                 now_rolling ? "DETECTED" : "cleared",
                 a, r, cfg.roll_deploy_deg, cfg.sustain_ms);
    }
    if (now_manual && !s_manual) {
        ESP_LOGW(TAG, "MANUAL release button held -> deploy request");
    }

    s_rolling = now_rolling;
    s_manual  = now_manual;

    if (changed || refresh) {
        broadcast_vote(s_rolling, s_manual, roll_deg, roll_rate,
                       s_manual ? OPENDASH_PARACHUTE_REASON_MANUAL : reason);
    }
}

static void detector_task(void *arg)
{
    ESP_LOGI(TAG, "Rollover detector running (eval every %d ms)",
             OPENDASH_ROLLOVER_EVAL_PERIOD_MS);
    TickType_t last = xTaskGetTickCount();
    while (1) {
        float roll = 0.0f, rate = 0.0f;
        if (s_read && s_read(&roll, &rate)) {
            s_last_raw_roll = roll;
            evaluate(roll, rate);
        }
        vTaskDelayUntil(&last, pdMS_TO_TICKS(OPENDASH_ROLLOVER_EVAL_PERIOD_MS));
    }
}

esp_err_t opendash_rollover_detector_start(opendash_node_t self,
                                           opendash_rollover_read_fn read)
{
    if (!read || self >= OPENDASH_NODE_COUNT) return ESP_ERR_INVALID_ARG;
    s_self = self;
    s_read = read;

    offset_load();
    if (s_roll_offset != 0.0f) {
        ESP_LOGI(TAG, "Loaded roll zero offset %.1f deg", (double)s_roll_offset);
    }

    s_btn_gpio = btn_gpio_for(self);
    if (s_btn_gpio >= 0) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << s_btn_gpio,
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = OPENDASH_ROLLOVER_BTN_ACTIVE_LEVEL ? 0 : 1,
            .pull_down_en = OPENDASH_ROLLOVER_BTN_ACTIVE_LEVEL ? 1 : 0,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&io);
        ESP_LOGI(TAG, "Manual-release button on GPIO %d (active-%s)",
                 s_btn_gpio, OPENDASH_ROLLOVER_BTN_ACTIVE_LEVEL ? "high" : "low");
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        detector_task, "rollover_det", 4096, NULL, 6, NULL, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create detector task");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Rollover detector started for node %d", (int)self);
    return ESP_OK;
}
