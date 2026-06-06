/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file boost_client.c
 * @brief Implementation of the center-side boost controller client.
 *
 * The boost loop closes on the slave (mos-4ch-a by default). The center is
 * only the data fountain + map author. This file owns:
 *
 *   1. boost_live_push_task — 10 Hz: snapshot the engine DP cache from
 *      espnow_master, marshal an opendash_boost_live_t, unicast it.
 *
 *   2. RX hook (registered with espnow_master_set_aux_rx_callback) — receives
 *      BOOST_TELEMETRY + the four REPORT echoes, caches them locally so the
 *      UI can pre-fill the editor without re-querying.
 *
 *   3. Authoring helpers — convert UI edits into wire frames and send.
 */

#include "boost_client.h"
#include "espnow_master.h"
#include "system_config.h"
#include "opendash_i2c_protocol.h"
#include "opendash_data_model.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "boost_client";

/* Pulled from espnow_master.c — central DP cache snapshot. */
extern void espnow_master_snapshot_engine(float *rpm, float *boost_kpa, float *egt_c,
                                           float *afr, float *fuel_kpa,
                                           float *throttle_pct, float *gear,
                                           uint32_t *last_update_ms);

/* ============================================================================
 *  Local mirror — copies of slave state populated by REPORT frames.
 * ==========================================================================*/

static SemaphoreHandle_t s_lock = NULL;

static opendash_boost_params_t    s_params;
static bool                        s_params_valid = false;

static opendash_boost_telemetry_t s_telem;
static bool                        s_telem_valid = false;

static uint8_t  s_duty[OPENDASH_BOOST_MODES][OPENDASH_BOOST_GEARS][OPENDASH_BOOST_MAP_POINTS];
static bool     s_duty_valid[OPENDASH_BOOST_MODES][OPENDASH_BOOST_GEARS];

static uint16_t s_setp[OPENDASH_BOOST_MODES][OPENDASH_BOOST_GEARS][OPENDASH_BOOST_MAP_POINTS];
static bool     s_setp_valid[OPENDASH_BOOST_MODES][OPENDASH_BOOST_GEARS];

static bool s_initialized = false;

/* ============================================================================
 *  RX hook — runs in espnow_master's processing task.
 * ==========================================================================*/

static void on_rx_frame(const opendash_espnow_event_t *evt,
                         const opendash_i2c_msg_t *msg)
{
    (void)evt;
    switch (msg->cmd) {
        case OPENDASH_CMD_BOOST_TELEMETRY:
            if (msg->length >= sizeof(opendash_boost_telemetry_t)) {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                memcpy(&s_telem, msg->payload, sizeof(s_telem));
                s_telem_valid = true;
                xSemaphoreGive(s_lock);
            }
            break;

        case OPENDASH_CMD_BOOST_PARAMS_REPORT:
            if (msg->length >= sizeof(opendash_boost_params_t)) {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                memcpy(&s_params, msg->payload, sizeof(s_params));
                s_params_valid = true;
                xSemaphoreGive(s_lock);
            }
            break;

        case OPENDASH_CMD_BOOST_DUTY_REPORT:
            if (msg->length >= sizeof(opendash_boost_duty_row_t)) {
                const opendash_boost_duty_row_t *r = (const opendash_boost_duty_row_t *)msg->payload;
                if (r->mode < OPENDASH_BOOST_MODES && r->gear < OPENDASH_BOOST_GEARS) {
                    xSemaphoreTake(s_lock, portMAX_DELAY);
                    memcpy(s_duty[r->mode][r->gear], r->duty, sizeof(r->duty));
                    s_duty_valid[r->mode][r->gear] = true;
                    xSemaphoreGive(s_lock);
                }
            }
            break;

        case OPENDASH_CMD_BOOST_SETP_REPORT:
            if (msg->length >= sizeof(opendash_boost_setpoint_row_t)) {
                const opendash_boost_setpoint_row_t *r = (const opendash_boost_setpoint_row_t *)msg->payload;
                if (r->mode < OPENDASH_BOOST_MODES && r->gear < OPENDASH_BOOST_GEARS) {
                    xSemaphoreTake(s_lock, portMAX_DELAY);
                    memcpy(s_setp[r->mode][r->gear], r->setpoint_cbar, sizeof(r->setpoint_cbar));
                    s_setp_valid[r->mode][r->gear] = true;
                    xSemaphoreGive(s_lock);
                }
            }
            break;

        case OPENDASH_CMD_BOOST_THROTTLE_REPORT:
            /* Not mirrored locally yet — accept silently. */
            break;

        default:
            ESP_LOGD(TAG, "Aux RX: cmd 0x%02X (%u bytes)", msg->cmd, msg->length);
            break;
    }
}

/* ============================================================================
 *  10 Hz live push.
 * ==========================================================================*/

static void boost_live_push_task(void *arg)
{
    ESP_LOGI(TAG, "boost_live_push_task started @ 10 Hz");
    const TickType_t period = pdMS_TO_TICKS(100);
    TickType_t last = xTaskGetTickCount();

    while (1) {
        vTaskDelayUntil(&last, period);

        /* Don't waste airtime when the target node isn't reachable. */
        if (g_boost_target_node == OPENDASH_NODE_INVALID) continue;
        if (!espnow_master_node_online(g_boost_target_node)) continue;

        float rpm, boost_kpa, egt_c, afr, fuel_kpa, throttle_pct, gear;
        uint32_t age_ms;
        espnow_master_snapshot_engine(&rpm, &boost_kpa, &egt_c, &afr,
                                       &fuel_kpa, &throttle_pct, &gear, &age_ms);

        /* Convert center's "kPa gauge" into centi-bar gauge for the wire.
         * 1 bar = 100 kPa, so cBar = kPa * 1.0 (already centi-bar conv). */
        opendash_boost_live_t live = {
            .rpm            = (uint16_t)rpm,
            .boost_cbar     = (int16_t)boost_kpa,
            .egt_c          = (int16_t)egt_c,
            .afr_x10        = (uint16_t)(afr * 10.0f),
            .fuel_press_kpa = (uint16_t)fuel_kpa,
            .throttle_pct   = (uint8_t)(throttle_pct > 100.0f ? 100 : (throttle_pct < 0.0f ? 0 : throttle_pct)),
            .gear           = (uint8_t)gear,
        };

        espnow_master_send_raw(g_boost_target_node,
                               OPENDASH_CMD_BOOST_LIVE_DATA,
                               &live, sizeof(live));
    }
}

/* ============================================================================
 *  Public API
 * ==========================================================================*/

esp_err_t boost_client_init(void)
{
    if (s_initialized) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    espnow_master_set_aux_rx_callback(on_rx_frame);

    xTaskCreatePinnedToCore(boost_live_push_task, "boost_live", 4096, NULL, 4, NULL, 0);

    /* Ask the slave to dump its current state so the UI can render fresh. */
    boost_client_request_pull_all();

    s_initialized = true;
    ESP_LOGI(TAG, "boost_client up — target node %u", g_boost_target_node);
    return ESP_OK;
}

esp_err_t boost_client_set_params(const opendash_boost_params_t *p)
{
    if (!p) return ESP_ERR_INVALID_ARG;
    /* Pre-cache locally so the UI reflects the new params even before the
     * slave's echo arrives. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(&s_params, p, sizeof(*p));
    s_params_valid = true;
    xSemaphoreGive(s_lock);
    return espnow_master_send_raw(g_boost_target_node,
                                   OPENDASH_CMD_BOOST_SET_PARAMS, p, sizeof(*p));
}

esp_err_t boost_client_set_mode(opendash_boost_mode_t mode)
{
    uint8_t m = (uint8_t)mode;
    return espnow_master_send_raw(g_boost_target_node,
                                   OPENDASH_CMD_BOOST_SET_MODE, &m, sizeof(m));
}

esp_err_t boost_client_set_duty_row(uint8_t mode_slot, uint8_t gear,
                                     const uint8_t duty[OPENDASH_BOOST_MAP_POINTS])
{
    if (mode_slot >= OPENDASH_BOOST_MODES || gear >= OPENDASH_BOOST_GEARS) return ESP_ERR_INVALID_ARG;
    opendash_boost_duty_row_t row = { .mode = mode_slot, .gear = gear };
    memcpy(row.duty, duty, sizeof(row.duty));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_duty[mode_slot][gear], duty, sizeof(row.duty));
    s_duty_valid[mode_slot][gear] = true;
    xSemaphoreGive(s_lock);

    return espnow_master_send_raw(g_boost_target_node,
                                   OPENDASH_CMD_BOOST_SET_DUTY_ROW, &row, sizeof(row));
}

esp_err_t boost_client_set_setpoint_row(uint8_t mode_slot, uint8_t gear,
                                          const uint16_t setp_cbar[OPENDASH_BOOST_MAP_POINTS])
{
    if (mode_slot >= OPENDASH_BOOST_MODES || gear >= OPENDASH_BOOST_GEARS) return ESP_ERR_INVALID_ARG;
    opendash_boost_setpoint_row_t row = { .mode = mode_slot, .gear = gear };
    memcpy(row.setpoint_cbar, setp_cbar, sizeof(row.setpoint_cbar));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_setp[mode_slot][gear], setp_cbar, sizeof(row.setpoint_cbar));
    s_setp_valid[mode_slot][gear] = true;
    xSemaphoreGive(s_lock);

    return espnow_master_send_raw(g_boost_target_node,
                                   OPENDASH_CMD_BOOST_SET_SETP_ROW, &row, sizeof(row));
}

esp_err_t boost_client_set_throttle(const opendash_boost_throttle_curve_t *c)
{
    if (!c) return ESP_ERR_INVALID_ARG;
    return espnow_master_send_raw(g_boost_target_node,
                                   OPENDASH_CMD_BOOST_SET_THROTTLE, c, sizeof(*c));
}

esp_err_t boost_client_request_pull_all(void)
{
    return espnow_master_send_raw(g_boost_target_node,
                                   OPENDASH_CMD_BOOST_PULL_ALL, NULL, 0);
}

void boost_client_get_telemetry(opendash_boost_telemetry_t *out)
{
    if (!out) return;
    if (!s_lock) { memset(out, 0, sizeof(*out)); return; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_telem_valid) *out = s_telem;
    else               memset(out, 0, sizeof(*out));
    xSemaphoreGive(s_lock);
}

bool boost_client_peek_duty_row(uint8_t mode_slot, uint8_t gear,
                                  uint8_t out[OPENDASH_BOOST_MAP_POINTS])
{
    if (mode_slot >= OPENDASH_BOOST_MODES || gear >= OPENDASH_BOOST_GEARS) return false;
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_duty_valid[mode_slot][gear]) {
        memcpy(out, s_duty[mode_slot][gear], OPENDASH_BOOST_MAP_POINTS);
        ok = true;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

bool boost_client_peek_setpoint_row(uint8_t mode_slot, uint8_t gear,
                                      uint16_t out[OPENDASH_BOOST_MAP_POINTS])
{
    if (mode_slot >= OPENDASH_BOOST_MODES || gear >= OPENDASH_BOOST_GEARS) return false;
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_setp_valid[mode_slot][gear]) {
        memcpy(out, s_setp[mode_slot][gear], OPENDASH_BOOST_MAP_POINTS * sizeof(uint16_t));
        ok = true;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

bool boost_client_peek_params(opendash_boost_params_t *out)
{
    if (!out) return false;
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_params_valid) { *out = s_params; ok = true; }
    xSemaphoreGive(s_lock);
    return ok;
}
