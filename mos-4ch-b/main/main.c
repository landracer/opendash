/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file main.c
 * @brief OpenDash 4-Channel MOS FET Controller B — Entry Point
 *
 * Hardware: ESP32 4-Way MOS FET Module
 *
 * Headless controller (no display). Receives commands from center
 * via ESP-NOW. Supports both ON/OFF switching and PWM duty control
 * for variable-speed loads (fans, pumps, LED bars, etc.).
 *
 * Role: ESP-NOW node (OPENDASH_NODE_MOS_4CH_B)
 *   - Receives SET_RELAY commands from center (with PWM duty byte)
 *   - Responds to PING with STATUS_REPORT
 *   - Reports channel states via RELAY_STATUS
 *
 * ┌─────────────────────────────────────────────────┐
 * │  GPIO ASSIGNMENTS — TBD                         │
 * │  Update gpio_num fields in mos_config below.    │
 * │  Set gpio_num = -1 for unassigned channels.     │
 * └─────────────────────────────────────────────────┘
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"

#include "driver/gpio.h"

#include "opendash_common.h"
#include "opendash_espnow.h"
#include "opendash_i2c_protocol.h"
#include "opendash_relay.h"
#include "opendash_boost.h"
#include "opendash_bt_ota.h"
#include "opendash_parachute.h"
#include "parachute_gpio.h"

/* Boost output → MOS channel 3 (configurable). PWM duty 0..255 written every
 * compute tick.  When the boost subsystem is OFF the channel is left to the
 * normal relay-set path (set_pwm 0 each tick still releases it). */
#define MOS_BOOST_PWM_CHANNEL  3
/* ════════════════════════════════════════════════════════════════════════════
 * Status LED — GPIO23
 *
 * Pattern (repeats every ~3s):
 *   1. Heartbeat double-pulse (board alive + ESP-NOW OK)
 *   2. Pause
 *   3. N quick flashes = N active channels (1–3)
 *      OR one long solid pulse if ALL 4 channels are ON
 *      OR nothing extra if 0 channels active
 *   4. Pause, repeat
 *
 * Steady rapid flash = fault (ESP-NOW not initialized)
 * ════════════════════════════════════════════════════════════════════════════ */
#define STATUS_LED_GPIO  GPIO_NUM_23

static const char *TAG = "mos_4ch_b";

/* ════════════════════════════════════════════════════════════════════════════
 * MOS FET Channel Configuration
 *
 * USER: Update gpio_num values after physical pinout discovery!
 * MOS FETs support PWM (0-255 duty) via SET_RELAY payload[2].
 * PWM frequency set to 1 kHz — suitable for most DC loads.
 * ════════════════════════════════════════════════════════════════════════════ */

static const opendash_relay_config_t mos_config = {
    .type         = OPENDASH_RELAY_TYPE_MOS_FET,
    .polarity     = OPENDASH_RELAY_ACTIVE_HIGH,  /* MOS FET modules are typically active-high */
    .num_channels = 4,
    .pwm_freq_hz  = 1000,  /* 1 kHz PWM frequency */
    .channels = {
        { .gpio_num = 16, .enabled = true, .label = "MOS B CH1" },  /* CH0 — GPIO16 */
        { .gpio_num = 17, .enabled = true, .label = "MOS B CH2" },  /* CH1 — GPIO17 */
        { .gpio_num = 26, .enabled = true, .label = "MOS B CH3" },  /* CH2 — GPIO26 */
        { .gpio_num = 27, .enabled = true, .label = "MOS B CH4" },  /* CH3 — GPIO27 */
    },
};

/* ════════════════════════════════════════════════════════════════════════════
 * ESP-NOW Communication
 * ════════════════════════════════════════════════════════════════════════════ */

static uint8_t s_center_mac[6];
static bool    s_center_mac_known = false;

/* ── Heartbeat send — used only for center discovery, never for channel state ── */
static void send_heartbeat_broadcast(void)
{
    uint8_t payload[3] = { OPENDASH_NODE_MOS_4CH_B, 0x01, 0x00 };
    opendash_i2c_msg_t msg;
    opendash_i2c_build_msg(&msg, OPENDASH_CMD_STATUS_REPORT, payload, sizeof(payload));
    uint8_t tx_buf[OPENDASH_ESPNOW_MAX_DATA];
    uint16_t len = 0;
    if (opendash_i2c_serialize(&msg, tx_buf, &len) == OPENDASH_OK) {
        opendash_espnow_broadcast(tx_buf, len);
    }
}

/* ── Parachute status echo → center (config + live actuator state) ── */
static bool               s_para_fired       = false;  /* channel deploy latched this boot */
static uint8_t            s_para_fired_mask  = 0;       /* channels we energized            */
static esp_timer_handle_t s_para_pulse_timer = NULL;    /* pulse-mode auto-off              */

static void send_parachute_status(void)
{
    if (!s_center_mac_known) return;
    opendash_parachute_status_t st;
    memset(&st, 0, sizeof(st));
    opendash_parachute_config_get(&st.cfg);
    /* Channels ARE the actuator on a MOS; reflect a channel deploy as DEPLOYED
     * even though the shared GPIO actuator path is unassigned/inhibited. */
    st.act_state = s_para_fired ? (uint8_t)OPENDASH_PARACHUTE_ACT_DEPLOYED
                                : (uint8_t)opendash_parachute_actuator_state();
    st.armed     = opendash_parachute_actuator_is_armed()   ? 1 : 0;
    st.deployed  = (s_para_fired || opendash_parachute_actuator_is_deployed()) ? 1 : 0;

    opendash_i2c_msg_t m;
    opendash_i2c_build_msg(&m, OPENDASH_CMD_PARACHUTE_STATUS,
                            (const uint8_t *)&st, sizeof(st));
    uint8_t bb[OPENDASH_ESPNOW_MAX_DATA];
    uint16_t bl = 0;
    if (opendash_i2c_serialize(&m, bb, &bl) == OPENDASH_OK) {
        opendash_espnow_send(s_center_mac, bb, bl);
    }
}

/* ── Parachute channel fire path (MOS-specific) ──────────────────────────────
 * The selected MOS power channel(s) ARE the deploy actuator. The shared module
 * owns ARM state; here we energize channels under a hard interlock and latch
 * the deploy. LATCH (default) holds the channel on until disarm/reboot; PULSE
 * releases it after pulse_ms. Idempotent — never re-fires once latched. */
static void parachute_channels_off(uint8_t mask)
{
    for (int c = 0; c < 4; c++) {
        if (mask & (1u << c)) opendash_relay_set(c, false);
    }
}

static void parachute_pulse_off_cb(void *arg)
{
    (void)arg;
    parachute_channels_off(s_para_fired_mask);
    ESP_LOGW(TAG, "Parachute pulse complete — channels 0x%X de-energized (DEPLOYED latched)",
             s_para_fired_mask);
}

static esp_err_t parachute_fire(opendash_parachute_reason_t reason)
{
    if (s_para_fired) return ESP_OK;   /* idempotent lockout */

    opendash_parachute_config_t pc;
    opendash_parachute_config_get(&pc);

    /* HARD INTERLOCK — every condition must hold or nothing energizes. */
    if (!pc.enabled) {
        ESP_LOGW(TAG, "DEPLOY refused: system DISABLED (reason=%d)", (int)reason);
        return ESP_ERR_INVALID_STATE;
    }
    if ((pc.channel_mask & 0x0F) == 0) {
        ESP_LOGW(TAG, "DEPLOY refused: no channel selected (reason=%d)", (int)reason);
        return ESP_ERR_INVALID_STATE;
    }
    if (!opendash_parachute_actuator_is_armed()) {
        ESP_LOGW(TAG, "DEPLOY refused: DISARMED (reason=%d)", (int)reason);
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t mask = pc.channel_mask & 0x0F;
    bool pulse   = (pc.flags & OPENDASH_PARACHUTE_FLAG_FIRE_PULSE) != 0;
    ESP_LOGW(TAG, "!!! PARACHUTE DEPLOY — energizing channels 0x%X, reason=%d, mode=%s !!!",
             mask, (int)reason, pulse ? "PULSE" : "LATCH");
    for (int c = 0; c < 4; c++) {
        if (mask & (1u << c)) opendash_relay_set(c, true);
    }
    s_para_fired      = true;
    s_para_fired_mask = mask;

    if (pulse) {
        uint32_t pulse_ms = pc.pulse_ms ? pc.pulse_ms : OPENDASH_PARACHUTE_DEPLOY_PULSE_MS;
        if (s_para_pulse_timer == NULL) {
            const esp_timer_create_args_t targs = {
                .callback = parachute_pulse_off_cb, .name = "para_ch_pulse" };
            esp_timer_create(&targs, &s_para_pulse_timer);
        }
        if (s_para_pulse_timer) {
            esp_timer_start_once(s_para_pulse_timer, (uint64_t)pulse_ms * 1000ULL);
        }
    }
    return ESP_OK;
}

/* Disarm path's safe-reset: de-energize any fired channel(s) and clear the
 * latch so the bench can re-arm/re-test. Re-fire still needs an explicit DEPLOY. */
static void parachute_disarm_reset(void)
{
    if (s_para_pulse_timer) esp_timer_stop(s_para_pulse_timer);
    if (s_para_fired_mask) parachute_channels_off(s_para_fired_mask);
    s_para_fired      = false;
    s_para_fired_mask = 0;
}

static void dispatch_message(const opendash_espnow_event_t *evt,
                              const opendash_i2c_msg_t *msg)
{
    /* Learn and track the center's MAC from genuine center→MOS commands ONLY.
     * These commands are issued exclusively by the center and arrive as unicast
     * addressed to this node, so their src MAC is always the live center.
     *
     * We deliberately do NOT latch onto every bit7-clear frame: other nodes also
     * broadcast bit7-clear frames (notably GPS TIME_SYNC, cmd=SYSTEM). The old
     * "first bit7-clear frame" latch grabbed GPS as the center, so our unicast
     * replies (relay ACKs, parachute 0x95 echoes) were sent to GPS and the
     * center's PUSH/REFRESH never confirmed. SYSTEM is excluded for this reason. */
    bool is_center_cmd = false;
    switch (msg->cmd) {
        case OPENDASH_CMD_SET_RELAY:
        case OPENDASH_CMD_REQUEST_RELAY_STATUS:
        case OPENDASH_CMD_PARACHUTE_SET_CONFIG:
        case OPENDASH_CMD_PARACHUTE_SET_ARM:
        case OPENDASH_CMD_PARACHUTE_PULL_ALL:
        case OPENDASH_CMD_PARACHUTE_DEPLOY:
            is_center_cmd = true;
            break;
        default:
            break;
    }
    if (is_center_cmd &&
        (!s_center_mac_known || memcmp(s_center_mac, evt->src_mac, 6) != 0)) {
        bool first = !s_center_mac_known;
        memcpy(s_center_mac, evt->src_mac, 6);
        s_center_mac_known = true;
        opendash_espnow_add_peer(s_center_mac);
        if (first) {
            ESP_LOGI(TAG, "Center discovered @ " MACSTR, MAC2STR(s_center_mac));
            /* Announce identity so the center auto-registers our MAC (via
             * STATUS_REPORT node_type) immediately instead of waiting for the
             * next periodic heartbeat. */
            send_heartbeat_broadcast();
            /* Boot announce: send actual relay state so center clears stale cache */
            uint8_t boot_mask = 0;
            for (int i = 0; i < 4; i++) {
                opendash_relay_channel_state_t st;
                if (opendash_relay_get_state(i, &st) == ESP_OK && st.is_on) boot_mask |= (1u << i);
            }
            uint8_t bp[3] = { 0xCA, OPENDASH_NODE_MOS_4CH_B, boot_mask };
            opendash_i2c_msg_t bm;
            opendash_i2c_build_msg(&bm, OPENDASH_CMD_RELAY_STATUS, bp, sizeof(bp));
            uint8_t bb[OPENDASH_ESPNOW_MAX_DATA]; uint16_t bl = 0;
            if (opendash_i2c_serialize(&bm, bb, &bl) == OPENDASH_OK) {
                opendash_espnow_send(s_center_mac, bb, bl);
            }
            ESP_LOGI(TAG, "Boot state announce: mask 0x%02X", boot_mask);
        } else {
            ESP_LOGW(TAG, "Center MAC re-synced -> " MACSTR, MAC2STR(s_center_mac));
        }
    }

    switch (msg->cmd) {
        case OPENDASH_CMD_SET_RELAY: {
            /* ── MASK FORMAT: [0xCA, node_id, mask, seq]
             * bits 0-3 = channels 0-3. Master owns full state.
             * Sends immediate RELAY_STATUS confirmation to center. */
            if (msg->length >= 3 && msg->payload[0] == 0xCA) {
                uint8_t target = msg->payload[1];
                if (target != OPENDASH_NODE_MOS_4CH_B) break;
                uint8_t mask = msg->payload[2];
                for (int i = 0; i < 4; i++) {
                    opendash_relay_set(i, (mask >> i) & 1);
                }
                ESP_LOGI(TAG, "MOS-B mask 0x%02X applied", mask);
                /* Immediate confirmation → center knows state without audit delay */
                if (s_center_mac_known) {
                    uint8_t ack_pay[3] = { 0xCA, OPENDASH_NODE_MOS_4CH_B, mask };
                    opendash_i2c_msg_t ack_msg;
                    opendash_i2c_build_msg(&ack_msg, OPENDASH_CMD_RELAY_STATUS, ack_pay, sizeof(ack_pay));
                    uint8_t ack_buf[OPENDASH_ESPNOW_MAX_DATA];
                    uint16_t ack_len = 0;
                    if (opendash_i2c_serialize(&ack_msg, ack_buf, &ack_len) == OPENDASH_OK) {
                        opendash_espnow_send(s_center_mac, ack_buf, ack_len);
                    }
                }
                break;
            }
            /* ── LEGACY per-channel format: [channel, state, pwm_duty] ── */
            if (msg->length >= 2) {
                uint8_t channel  = msg->payload[0];
                uint8_t state    = msg->payload[1];
                uint8_t pwm_duty = (msg->length >= 3) ? msg->payload[2] : 255;

                esp_err_t ret;
                if (state && pwm_duty < 255) {
                    ret = opendash_relay_set_pwm(channel, pwm_duty);
                    if (ret == ESP_OK) {
                        ESP_LOGI(TAG, "MOS CH%d → PWM duty %d/255", channel, pwm_duty);
                    }
                } else {
                    ret = opendash_relay_set(channel, state != 0);
                    if (ret == ESP_OK) {
                        ESP_LOGI(TAG, "MOS CH%d → %s", channel, state ? "ON (100%%)" : "OFF");
                    }
                }

                if (ret != ESP_OK) {
                    ESP_LOGW(TAG, "MOS CH%d set failed: %s", channel, esp_err_to_name(ret));
                }
                /* No send_relay_status() — master owns state, no ACK needed */
            }
            break;
        }

        case OPENDASH_CMD_REQUEST_RELAY_STATUS: {
            /* Compact audit response: [0xCA, node_id, actual_mask]
             * Sent unicast to center — no broadcast, no storm risk. */
            if (!s_center_mac_known) break;
            opendash_relay_channel_state_t audit_st[OPENDASH_RELAY_MAX_CHANNELS];
            uint8_t audit_num = 0;
            opendash_relay_get_all_states(audit_st, &audit_num);
            uint8_t actual_mask = 0;
            for (int i = 0; i < audit_num && i < 4; i++) {
                if (audit_st[i].is_on) actual_mask |= (1u << i);
            }
            uint8_t audit_pay[3] = { 0xCA, OPENDASH_NODE_MOS_4CH_B, actual_mask };
            opendash_i2c_msg_t audit_resp;
            opendash_i2c_build_msg(&audit_resp, OPENDASH_CMD_RELAY_STATUS, audit_pay, sizeof(audit_pay));
            uint8_t audit_buf[OPENDASH_ESPNOW_MAX_DATA];
            uint16_t audit_len = 0;
            if (opendash_i2c_serialize(&audit_resp, audit_buf, &audit_len) == OPENDASH_OK) {
                opendash_espnow_send(s_center_mac, audit_buf, audit_len);
                ESP_LOGI(TAG, "Audit reply: mask 0x%02X", actual_mask);
            }
            break;
        }

        case OPENDASH_CMD_SYSTEM: {
            if (msg->length < 1) break;
            switch (msg->payload[0]) {
                case OPENDASH_SUBCMD_PING:
                    /* MOS ignores PING — uses autonomous heartbeat instead.
                     * Eliminates broadcast storm from responding to every PING. */
                    break;
                case OPENDASH_SUBCMD_REBOOT:
                    ESP_LOGW(TAG, "Reboot — all MOS channels OFF");
                    opendash_relay_all_off();
                    vTaskDelay(pdMS_TO_TICKS(100));
                    esp_restart();
                    break;
                case OPENDASH_SUBCMD_ENTER_BT_OTA:
                    ESP_LOGW(TAG, "Entering BLE OTA mode — MOS off, boost stopped");
                    for (int _c = 0; _c < 4; _c++) opendash_relay_set_pwm(_c, 0);
                    opendash_relay_all_off();
                    opendash_espnow_deinit();
                    opendash_bt_ota_start(OPENDASH_NODE_MOS_4CH_B);
                    vTaskDelay(pdMS_TO_TICKS(100));
                    esp_restart();
                    break;
                default:
                    break;
            }
            break;
        }

        /* ──────────────────────────────────────────────────────
         * Parachute / Deployment System — center→MOS (0x27..0x29)
         * Config is NVS-persisted here; ARM is transient (reboot = DISARMED).
         * Firing (channel energize) is NOT wired yet — actuator stays inhibited.
         * ────────────────────────────────────────── */
        case OPENDASH_CMD_PARACHUTE_SET_CONFIG: {
            if (msg->length < sizeof(opendash_parachute_config_t)) break;
            opendash_parachute_config_t pc;
            memcpy(&pc, msg->payload, sizeof(pc));
            opendash_parachute_config_set(&pc);
            opendash_parachute_config_get(&pc);   /* read back sanitized */
            ESP_LOGI(TAG, "Deploy cfg set: %s ch=0x%X spd>=%.0fmph roll>=%.0fdeg "
                          "rate>=%.0fdeg/s sustain=%ums pulse=%ums",
                     pc.enabled ? "ENABLED" : "disabled", pc.channel_mask,
                     pc.min_speed_mph, pc.roll_deploy_deg, pc.roll_rate_deg_s,
                     pc.sustain_ms, pc.pulse_ms);
            send_parachute_status();
            break;
        }
        case OPENDASH_CMD_PARACHUTE_SET_ARM: {
            if (msg->length < 1) break;
            bool armed = msg->payload[0] != 0;
            opendash_parachute_actuator_set_armed(armed);
            if (!armed) parachute_disarm_reset();  /* safe-reset fired channels */
            ESP_LOGW(TAG, "Deploy actuator %s by center",
                     armed ? "ARMED" : "DISARMED");
            send_parachute_status();
            break;
        }
        case OPENDASH_CMD_PARACHUTE_PULL_ALL: {
            send_parachute_status();
            break;
        }
        case OPENDASH_CMD_PARACHUTE_DEPLOY: {
            esp_err_t fr = parachute_fire(OPENDASH_PARACHUTE_REASON_MANUAL);
            ESP_LOGW(TAG, "Manual DEPLOY command from center -> %s", esp_err_to_name(fr));
            send_parachute_status();
            break;
        }

        default:
            ESP_LOGD(TAG, "Unhandled cmd: 0x%02X", msg->cmd);
            break;

        /* ──────────────────────────────────────────────────────────────
         * Boost controller — center→slave control opcodes (0x20..0x26)
         * ────────────────────────────────────────────────────────────── */
        case OPENDASH_CMD_BOOST_LIVE_DATA: {
            if (msg->length < sizeof(opendash_boost_live_t)) break;
            opendash_boost_live_t live;
            memcpy(&live, msg->payload, sizeof(live));
            opendash_boost_feed_live(&live);
            break;
        }
        case OPENDASH_CMD_BOOST_SET_PARAMS: {
            if (msg->length < sizeof(opendash_boost_params_t)) break;
            opendash_boost_params_t p;
            memcpy(&p, msg->payload, sizeof(p));
            opendash_boost_set_params(&p);
            if (s_center_mac_known) {
                opendash_i2c_msg_t rm;
                opendash_i2c_build_msg(&rm, OPENDASH_CMD_BOOST_PARAMS_REPORT,
                                        (const uint8_t *)&p, sizeof(p));
                uint8_t bb[OPENDASH_ESPNOW_MAX_DATA]; uint16_t bl = 0;
                if (opendash_i2c_serialize(&rm, bb, &bl) == OPENDASH_OK) {
                    opendash_espnow_send(s_center_mac, bb, bl);
                }
            }
            break;
        }
        case OPENDASH_CMD_BOOST_SET_MODE: {
            if (msg->length < 1) break;
            opendash_boost_params_t p;
            if (opendash_boost_get_params(&p) == ESP_OK) {
                p.mode = msg->payload[0];
                opendash_boost_set_params(&p);
            }
            break;
        }
        case OPENDASH_CMD_BOOST_SET_DUTY_ROW: {
            if (msg->length < sizeof(opendash_boost_duty_row_t)) break;
            const opendash_boost_duty_row_t *r =
                (const opendash_boost_duty_row_t *)msg->payload;
            opendash_boost_set_duty_row(r->mode, r->gear, r->duty);
            if (s_center_mac_known) {
                opendash_i2c_msg_t rm;
                opendash_i2c_build_msg(&rm, OPENDASH_CMD_BOOST_DUTY_REPORT,
                                        (const uint8_t *)r, sizeof(*r));
                uint8_t bb[OPENDASH_ESPNOW_MAX_DATA]; uint16_t bl = 0;
                if (opendash_i2c_serialize(&rm, bb, &bl) == OPENDASH_OK) {
                    opendash_espnow_send(s_center_mac, bb, bl);
                }
            }
            break;
        }
        case OPENDASH_CMD_BOOST_SET_SETP_ROW: {
            if (msg->length < sizeof(opendash_boost_setpoint_row_t)) break;
            opendash_boost_setpoint_row_t r;
            memcpy(&r, msg->payload, sizeof(r));
            uint16_t setp[OPENDASH_BOOST_MAP_POINTS];
            memcpy(setp, r.setpoint_cbar, sizeof(setp));
            opendash_boost_set_setpoint_row(r.mode, r.gear, setp);
            if (s_center_mac_known) {
                opendash_i2c_msg_t rm;
                opendash_i2c_build_msg(&rm, OPENDASH_CMD_BOOST_SETP_REPORT,
                                        (const uint8_t *)&r, sizeof(r));
                uint8_t bb[OPENDASH_ESPNOW_MAX_DATA]; uint16_t bl = 0;
                if (opendash_i2c_serialize(&rm, bb, &bl) == OPENDASH_OK) {
                    opendash_espnow_send(s_center_mac, bb, bl);
                }
            }
            break;
        }
        case OPENDASH_CMD_BOOST_SET_THROTTLE: {
            if (msg->length < sizeof(opendash_boost_throttle_curve_t)) break;
            opendash_boost_throttle_curve_t c;
            memcpy(&c, msg->payload, sizeof(c));
            opendash_boost_set_throttle_curve(&c);
            if (s_center_mac_known) {
                opendash_i2c_msg_t rm;
                opendash_i2c_build_msg(&rm, OPENDASH_CMD_BOOST_THROTTLE_REPORT,
                                        (const uint8_t *)&c, sizeof(c));
                uint8_t bb[OPENDASH_ESPNOW_MAX_DATA]; uint16_t bl = 0;
                if (opendash_i2c_serialize(&rm, bb, &bl) == OPENDASH_OK) {
                    opendash_espnow_send(s_center_mac, bb, bl);
                }
            }
            break;
        }
        case OPENDASH_CMD_BOOST_PULL_ALL: {
            if (!s_center_mac_known) break;
            uint8_t bb[OPENDASH_ESPNOW_MAX_DATA]; uint16_t bl;
            opendash_i2c_msg_t rm;

            opendash_boost_params_t p;
            if (opendash_boost_get_params(&p) == ESP_OK) {
                opendash_i2c_build_msg(&rm, OPENDASH_CMD_BOOST_PARAMS_REPORT,
                                        (const uint8_t *)&p, sizeof(p));
                bl = 0;
                if (opendash_i2c_serialize(&rm, bb, &bl) == OPENDASH_OK) {
                    opendash_espnow_send(s_center_mac, bb, bl);
                }
            }
            for (uint8_t mode = 1; mode <= OPENDASH_BOOST_MODES; ++mode) {
                for (uint8_t gear = 0; gear < OPENDASH_BOOST_GEARS; ++gear) {
                    opendash_boost_duty_row_t dr = { .mode = mode, .gear = gear };
                    if (opendash_boost_get_duty_row(mode, gear, dr.duty) == ESP_OK) {
                        opendash_i2c_build_msg(&rm, OPENDASH_CMD_BOOST_DUTY_REPORT,
                                                (const uint8_t *)&dr, sizeof(dr));
                        bl = 0;
                        if (opendash_i2c_serialize(&rm, bb, &bl) == OPENDASH_OK) {
                            opendash_espnow_send(s_center_mac, bb, bl);
                        }
                    }
                    opendash_boost_setpoint_row_t sr = { .mode = mode, .gear = gear };
                    uint16_t setp[OPENDASH_BOOST_MAP_POINTS];
                    if (opendash_boost_get_setpoint_row(mode, gear, setp) == ESP_OK) {
                        memcpy(sr.setpoint_cbar, setp, sizeof(setp));
                        opendash_i2c_build_msg(&rm, OPENDASH_CMD_BOOST_SETP_REPORT,
                                                (const uint8_t *)&sr, sizeof(sr));
                        bl = 0;
                        if (opendash_i2c_serialize(&rm, bb, &bl) == OPENDASH_OK) {
                            opendash_espnow_send(s_center_mac, bb, bl);
                        }
                    }
                    vTaskDelay(pdMS_TO_TICKS(5));
                }
            }
            opendash_boost_throttle_curve_t tc;
            if (opendash_boost_get_throttle_curve(&tc) == ESP_OK) {
                opendash_i2c_build_msg(&rm, OPENDASH_CMD_BOOST_THROTTLE_REPORT,
                                        (const uint8_t *)&tc, sizeof(tc));
                bl = 0;
                if (opendash_i2c_serialize(&rm, bb, &bl) == OPENDASH_OK) {
                    opendash_espnow_send(s_center_mac, bb, bl);
                }
            }
            break;
        }
    }
}

/* ═════════════════════════════════════════════════════════════════════════════
 * Boost Controller Tasks
 * ════════════════════════════════════════════════════════════════════════════ */

static void boost_compute_task(void *pv)
{
    (void)pv;
    ESP_LOGI(TAG, "Boost compute task @ 50 Hz (output mask honored)");
    esp_task_wdt_add(NULL);
    TickType_t last = xTaskGetTickCount();
    uint8_t prev_mask = 0;
    while (1) {
        esp_task_wdt_reset();
        opendash_boost_telemetry_t t;
        uint8_t duty = opendash_boost_compute(&t);
        uint8_t mask = opendash_boost_get_output_mask();
        /* Release any channel we drove last tick but no longer own (e.g. it was
         * reassigned to the safety system) so it doesn't latch at the old duty. */
        uint8_t released = (uint8_t)(prev_mask & ~mask);
        for (int c = 0; c < 4; c++) if (released & (1u << c)) opendash_relay_set_pwm(c, 0);
        for (int c = 0; c < 4; c++) if (mask     & (1u << c)) opendash_relay_set_pwm(c, duty);
        prev_mask = mask;
        vTaskDelayUntil(&last, pdMS_TO_TICKS(20));   /* 50 Hz */
    }
}

static void boost_telemetry_task(void *pv)
{
    (void)pv;
    ESP_LOGI(TAG, "Boost telemetry task @ 5 Hz");
    esp_task_wdt_add(NULL);
    TickType_t last = xTaskGetTickCount();
    while (1) {
        esp_task_wdt_reset();
        if (s_center_mac_known) {
            opendash_boost_telemetry_t t;
            opendash_boost_get_telemetry(&t);
            opendash_i2c_msg_t rm;
            opendash_i2c_build_msg(&rm, OPENDASH_CMD_BOOST_TELEMETRY,
                                    (const uint8_t *)&t, sizeof(t));
            uint8_t bb[OPENDASH_ESPNOW_MAX_DATA]; uint16_t bl = 0;
            if (opendash_i2c_serialize(&rm, bb, &bl) == OPENDASH_OK) {
                opendash_espnow_send(s_center_mac, bb, bl);
            }
        }
        vTaskDelayUntil(&last, pdMS_TO_TICKS(200));  /* 5 Hz */
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 * ESP-NOW Task
 * ════════════════════════════════════════════════════════════════════════════ */

static void espnow_task(void *pvParameters)
{
    ESP_LOGI(TAG, "ESP-NOW processing task started");
    esp_task_wdt_add(NULL);

    /* Immediate heartbeat on boot for fast discovery */
    vTaskDelay(pdMS_TO_TICKS(500));
    send_heartbeat_broadcast();
    ESP_LOGI(TAG, "Initial heartbeat broadcast sent");

    uint32_t last_heartbeat_ms = (uint32_t)(esp_timer_get_time() / 1000);

    while (1) {
        esp_task_wdt_reset();
        opendash_espnow_event_t evt;
        while (opendash_espnow_recv(&evt, 0)) {
            opendash_i2c_msg_t msg;
            if (opendash_i2c_deserialize(evt.data, evt.len, &msg) == OPENDASH_OK) {
                dispatch_message(&evt, &msg);
            }
        }

        /* Heartbeat every 61s — staggered: R4=45 R8A=49 R8B=53 MA=57 MB=61.
         * MOS is otherwise silent. No RELAY_STATUS broadcasts. */
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if ((now_ms - last_heartbeat_ms) >= 61000) {
            last_heartbeat_ms = now_ms;
            send_heartbeat_broadcast();
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 * Main Entry Point
 * ════════════════════════════════════════════════════════════════════════════ */

void app_main(void)
{
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════");
    ESP_LOGI(TAG, "  OpenDash 4-Channel MOS FET Controller B");
    ESP_LOGI(TAG, "  Node: OPENDASH_NODE_MOS_4CH_B");
    ESP_LOGI(TAG, "  PWM: %d Hz, 8-bit resolution", mos_config.pwm_freq_hz);
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════");

    /* NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* MOS FET hardware (with LEDC PWM) */
    ret = opendash_relay_init(&mos_config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "MOS init failed (GPIOs not assigned?) — continuing");
    }

    for (int i = 0; i < mos_config.num_channels; i++) {
        ESP_LOGI(TAG, "  CH%d: [%s] GPIO=%d %s",
                 i, mos_config.channels[i].label,
                 mos_config.channels[i].gpio_num,
                 mos_config.channels[i].enabled ? "READY" : "NOT ASSIGNED");
    }

    /* ESP-NOW */
    ESP_ERROR_CHECK(opendash_espnow_init(OPENDASH_NODE_MOS_4CH_B));

    /* Boost controller — loads NVS params/maps or installs defaults */
    ESP_ERROR_CHECK(opendash_boost_init());

    /* Parachute deploy actuator — GPIO from parachute_gpio.h (-1 = inhibited).
     * Stays SAFE/DISARMED until an ARM_STATE is received (protocol TBD). */
    const opendash_parachute_actuator_config_t para_cfg = {
        .gpio_num    = OPENDASH_PARACHUTE_DEPLOY_GPIO,
        .active_high = OPENDASH_PARACHUTE_DEPLOY_ACTIVE_HIGH,
        .pulse_ms    = OPENDASH_PARACHUTE_DEPLOY_PULSE_MS,
    };
    opendash_parachute_actuator_init(&para_cfg);

    /* Deployment config — load persisted (or install safe defaults). ARM is
     * NOT persisted: the node always boots DISARMED. */
    opendash_parachute_config_init();
    opendash_parachute_config_t dcfg;
    opendash_parachute_config_get(&dcfg);
    ESP_LOGI(TAG, "Deploy cfg: %s ch=0x%X spd>=%.0fmph roll>=%.0fdeg "
                  "rate>=%.0fdeg/s sustain=%ums pulse=%ums (DISARMED)",
             dcfg.enabled ? "ENABLED" : "disabled", dcfg.channel_mask,
             dcfg.min_speed_mph, dcfg.roll_deploy_deg, dcfg.roll_rate_deg_s,
             dcfg.sustain_ms, dcfg.pulse_ms);

    xTaskCreatePinnedToCore(espnow_task,         "espnow_task",   4096, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(boost_compute_task,  "boost_compute", 4096, NULL, 6, NULL, 1);
    xTaskCreatePinnedToCore(boost_telemetry_task,"boost_telem",   4096, NULL, 4, NULL, 1);

    /* Status LED — GPIO23 */
    gpio_config_t led_io = {
        .pin_bit_mask = (1ULL << STATUS_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_io);
    gpio_set_level(STATUS_LED_GPIO, 0);
    ESP_LOGI(TAG, "Status LED on GPIO%d initialized", STATUS_LED_GPIO);

    ESP_LOGI(TAG, "MOS 4CH-B ready — waiting for commands (PWM capable)");

    /* Combined health monitoring + LED status loop */
    uint32_t uptime_s = 0;
    uint32_t loop_tick = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(50));  /* 20 Hz LED loop */
        loop_tick++;

        /* ── LED status pattern (cycle every ~3s = 60 ticks @ 50ms) ── */
        uint32_t phase = loop_tick % 60;

        opendash_relay_channel_state_t led_states[4];
        uint8_t led_num = 0;
        opendash_relay_get_all_states(led_states, &led_num);
        uint8_t active_count = 0;
        for (int i = 0; i < led_num && i < 4; i++) {
            if (led_states[i].is_on) active_count++;
        }

        bool led_on = false;
        if (opendash_bt_ota_is_active()) {
            /* BLE OTA mode: 3 quick pulses then long pause (~3s cycle). */
            led_on = (phase < 3) || (phase >= 6 && phase < 9) || (phase >= 12 && phase < 15);
        } else if (!s_center_mac_known) {
            /* Fault: rapid steady flash — no center found */
            led_on = (phase % 4 < 2);
        } else {
            /* Phase 0-1: heartbeat pulse 1 */
            if (phase < 2) {
                led_on = true;
            }
            /* Phase 2-3: off gap */
            /* Phase 4-5: heartbeat pulse 2 */
            else if (phase >= 4 && phase < 6) {
                led_on = true;
            }
            /* Phase 6-11: pause after heartbeat */
            /* Phase 12+: channel activity flashes */
            else if (phase >= 12 && active_count > 0) {
                if (active_count == 4) {
                    /* All 4 on: one long solid pulse (12-23) */
                    led_on = (phase < 24);
                } else {
                    /* N quick flashes: 4 ticks on, 4 ticks off per flash */
                    uint32_t flash_phase = phase - 12;
                    uint32_t flash_idx = flash_phase / 8;  /* which flash */
                    uint32_t within    = flash_phase % 8;  /* position in flash */
                    if (flash_idx < active_count) {
                        led_on = (within < 4);  /* first half on, second half off */
                    }
                }
            }
        }
        gpio_set_level(STATUS_LED_GPIO, led_on ? 1 : 0);

        /* ── Uptime log every 10s (200 ticks) ── */
        if ((loop_tick % 200) == 0) {
            uptime_s += 10;
            ESP_LOGI(TAG, "Uptime: %lus | Center: %s | Active: %d/4 | CH: %d(%d%%) %d(%d%%) %d(%d%%) %d(%d%%)",
                     uptime_s,
                     s_center_mac_known ? "ONLINE" : "searching",
                     active_count,
                     led_num > 0 ? led_states[0].is_on : 0, led_num > 0 ? (led_states[0].pwm_duty * 100 / 255) : 0,
                     led_num > 1 ? led_states[1].is_on : 0, led_num > 1 ? (led_states[1].pwm_duty * 100 / 255) : 0,
                     led_num > 2 ? led_states[2].is_on : 0, led_num > 2 ? (led_states[2].pwm_duty * 100 / 255) : 0,
                     led_num > 3 ? led_states[3].is_on : 0, led_num > 3 ? (led_states[3].pwm_duty * 100 / 255) : 0);
        }
    }
}
