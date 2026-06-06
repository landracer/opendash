/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file main.c
 * @brief OpenDash 4-Channel MOS FET Controller A — Entry Point
 *
 * Hardware: ESP32 4-Way MOS FET Module
 *
 * Headless controller (no display). Receives commands from center
 * via ESP-NOW. Supports both ON/OFF switching and PWM duty control
 * for variable-speed loads (fans, pumps, LED bars, etc.).
 *
 * Role: ESP-NOW node (OPENDASH_NODE_MOS_4CH_A)
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

static const char *TAG = "mos_4ch_a";

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
        { .gpio_num = 16, .enabled = true, .label = "MOS A CH1" },  /* CH0 — GPIO16 */
        { .gpio_num = 17, .enabled = true, .label = "MOS A CH2" },  /* CH1 — GPIO17 */
        { .gpio_num = 26, .enabled = true, .label = "MOS A CH3" },  /* CH2 — GPIO26 */
        { .gpio_num = 27, .enabled = true, .label = "MOS A CH4" },  /* CH3 — GPIO27 */
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
    uint8_t payload[3] = { OPENDASH_NODE_MOS_4CH_A, 0x01, 0x00 };
    opendash_i2c_msg_t msg;
    opendash_i2c_build_msg(&msg, OPENDASH_CMD_STATUS_REPORT, payload, sizeof(payload));
    uint8_t tx_buf[OPENDASH_ESPNOW_MAX_DATA];
    uint16_t len = 0;
    if (opendash_i2c_serialize(&msg, tx_buf, &len) == OPENDASH_OK) {
        opendash_espnow_broadcast(tx_buf, len);
    }
}

static void dispatch_message(const opendash_espnow_event_t *evt,
                              const opendash_i2c_msg_t *msg)
{
    if (!s_center_mac_known) {
        memcpy(s_center_mac, evt->src_mac, 6);
        s_center_mac_known = true;
        opendash_espnow_add_peer(s_center_mac);
        ESP_LOGI(TAG, "Center discovered @ " MACSTR, MAC2STR(s_center_mac));
        /* Boot announce: send actual relay state so center clears stale cache */
        uint8_t boot_mask = 0;
        for (int i = 0; i < 4; i++) {
            opendash_relay_channel_state_t st;
            if (opendash_relay_get_state(i, &st) == ESP_OK && st.is_on) boot_mask |= (1u << i);
        }
        uint8_t bp[3] = { 0xCA, OPENDASH_NODE_MOS_4CH_A, boot_mask };
        opendash_i2c_msg_t bm;
        opendash_i2c_build_msg(&bm, OPENDASH_CMD_RELAY_STATUS, bp, sizeof(bp));
        uint8_t bb[OPENDASH_ESPNOW_MAX_DATA]; uint16_t bl = 0;
        if (opendash_i2c_serialize(&bm, bb, &bl) == OPENDASH_OK) {
            opendash_espnow_send(s_center_mac, bb, bl);
        }
        ESP_LOGI(TAG, "Boot state announce: mask 0x%02X", boot_mask);
    }

    switch (msg->cmd) {
        case OPENDASH_CMD_SET_RELAY: {
            /* ── MASK FORMAT: [0xCA, node_id, mask, seq]
             * bits 0-3 = channels 0-3. Master owns full state.
             * Sends immediate RELAY_STATUS confirmation to center. */
            if (msg->length >= 3 && msg->payload[0] == 0xCA) {
                uint8_t target = msg->payload[1];
                if (target != OPENDASH_NODE_MOS_4CH_A) break;
                uint8_t mask = msg->payload[2];
                for (int i = 0; i < 4; i++) {
                    opendash_relay_set(i, (mask >> i) & 1);
                }
                ESP_LOGI(TAG, "MOS-A mask 0x%02X applied", mask);
                /* Immediate confirmation → center knows state without audit delay */
                if (s_center_mac_known) {
                    uint8_t ack_pay[3] = { 0xCA, OPENDASH_NODE_MOS_4CH_A, mask };
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
            uint8_t audit_pay[3] = { 0xCA, OPENDASH_NODE_MOS_4CH_A, actual_mask };
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
                default:
                    break;
            }
            break;
        }

        /* ════════════════════════════════════════════════════════════════
         * Boost controller authoring + telemetry (CMDs 0x20…0x2A)
         *
         * The boost subsystem closes the loop locally on this node — the
         * center is purely the map author + UI. All boost frames travel
         * over ESP-NOW; we just happen to reuse the I2C protocol opcode
         * catalogue (legacy filename) for the on-wire byte layout.
         * ════════════════════════════════════════════════════════════════ */
        case OPENDASH_CMD_BOOST_LIVE_DATA: {
            /* 10 Hz snapshot from center: RPM/boost/EGT/AFR/fuel/throttle/gear */
            if (msg->length >= sizeof(opendash_boost_live_t)) {
                opendash_boost_live_t live;
                memcpy(&live, msg->payload, sizeof(live));
                opendash_boost_feed_live(&live);
            }
            break;
        }
        case OPENDASH_CMD_BOOST_SET_PARAMS: {
            if (msg->length >= sizeof(opendash_boost_params_t)) {
                opendash_boost_params_t p;
                memcpy(&p, msg->payload, sizeof(p));
                if (opendash_boost_set_params(&p) == ESP_OK) {
                    ESP_LOGI(TAG, "boost: params updated (mode=%u ch=%u)", p.mode, p.output_channel);
                }
            }
            break;
        }
        case OPENDASH_CMD_BOOST_SET_DUTY_ROW: {
            if (msg->length >= sizeof(opendash_boost_duty_row_t)) {
                const opendash_boost_duty_row_t *row = (const opendash_boost_duty_row_t *)msg->payload;
                opendash_boost_set_duty_row(row->mode, row->gear, row->duty);
            }
            break;
        }
        case OPENDASH_CMD_BOOST_SET_SETP_ROW: {
            if (msg->length >= sizeof(opendash_boost_setpoint_row_t)) {
                const opendash_boost_setpoint_row_t *row = (const opendash_boost_setpoint_row_t *)msg->payload;
                opendash_boost_set_setpoint_row(row->mode, row->gear, row->setpoint_cbar);
            }
            break;
        }
        case OPENDASH_CMD_BOOST_SET_THROTTLE: {
            if (msg->length >= sizeof(opendash_boost_throttle_curve_t)) {
                opendash_boost_throttle_curve_t c;
                memcpy(&c, msg->payload, sizeof(c));
                opendash_boost_set_throttle_curve(&c);
            }
            break;
        }
        case OPENDASH_CMD_BOOST_SET_MODE: {
            /* Quick mode flip: don't re-send the whole params blob. */
            if (msg->length >= 1) {
                opendash_boost_params_t p;
                if (opendash_boost_get_params(&p) == ESP_OK) {
                    p.mode = msg->payload[0];
                    opendash_boost_set_params(&p);
                    ESP_LOGI(TAG, "boost: mode → %u", p.mode);
                }
            }
            break;
        }
        case OPENDASH_CMD_BOOST_TELEMETRY_REQ: {
            /* One-shot telemetry on demand (e.g. when UI opens). */
            if (!s_center_mac_known) break;
            opendash_boost_telemetry_t t;
            opendash_boost_get_telemetry(&t);
            opendash_i2c_msg_t reply;
            opendash_i2c_build_msg(&reply, OPENDASH_CMD_BOOST_TELEMETRY, (uint8_t *)&t, sizeof(t));
            uint8_t buf[OPENDASH_ESPNOW_MAX_DATA]; uint16_t len = 0;
            if (opendash_i2c_serialize(&reply, buf, &len) == OPENDASH_OK) {
                opendash_espnow_send(s_center_mac, buf, len);
            }
            break;
        }
        case OPENDASH_CMD_BOOST_GET_DUTY_ROW: {
            if (!s_center_mac_known || msg->length < 2) break;
            opendash_boost_duty_row_t row = { .mode = msg->payload[0], .gear = msg->payload[1] };
            if (opendash_boost_get_duty_row(row.mode, row.gear, row.duty) == ESP_OK) {
                opendash_i2c_msg_t reply;
                opendash_i2c_build_msg(&reply, OPENDASH_CMD_BOOST_DUTY_REPORT, (uint8_t *)&row, sizeof(row));
                uint8_t buf[OPENDASH_ESPNOW_MAX_DATA]; uint16_t len = 0;
                if (opendash_i2c_serialize(&reply, buf, &len) == OPENDASH_OK) {
                    opendash_espnow_send(s_center_mac, buf, len);
                }
            }
            break;
        }
        case OPENDASH_CMD_BOOST_GET_SETP_ROW: {
            if (!s_center_mac_known || msg->length < 2) break;
            opendash_boost_setpoint_row_t row = { .mode = msg->payload[0], .gear = msg->payload[1] };
            if (opendash_boost_get_setpoint_row(row.mode, row.gear, row.setpoint_cbar) == ESP_OK) {
                opendash_i2c_msg_t reply;
                opendash_i2c_build_msg(&reply, OPENDASH_CMD_BOOST_SETP_REPORT, (uint8_t *)&row, sizeof(row));
                uint8_t buf[OPENDASH_ESPNOW_MAX_DATA]; uint16_t len = 0;
                if (opendash_i2c_serialize(&reply, buf, &len) == OPENDASH_OK) {
                    opendash_espnow_send(s_center_mac, buf, len);
                }
            }
            break;
        }
        case OPENDASH_CMD_BOOST_GET_THROTTLE: {
            if (!s_center_mac_known) break;
            opendash_boost_throttle_curve_t c;
            if (opendash_boost_get_throttle_curve(&c) == ESP_OK) {
                opendash_i2c_msg_t reply;
                opendash_i2c_build_msg(&reply, OPENDASH_CMD_BOOST_THROTTLE_REPORT, (uint8_t *)&c, sizeof(c));
                uint8_t buf[OPENDASH_ESPNOW_MAX_DATA]; uint16_t len = 0;
                if (opendash_i2c_serialize(&reply, buf, &len) == OPENDASH_OK) {
                    opendash_espnow_send(s_center_mac, buf, len);
                }
            }
            break;
        }
        case OPENDASH_CMD_BOOST_PULL_ALL: {
            /* Dump params + every row. The center UI uses this when it
             * first opens the boost editor so it can pre-fill the grid. */
            if (!s_center_mac_known) break;
            opendash_boost_params_t p;
            if (opendash_boost_get_params(&p) == ESP_OK) {
                opendash_i2c_msg_t r;
                opendash_i2c_build_msg(&r, OPENDASH_CMD_BOOST_PARAMS_REPORT, (uint8_t *)&p, sizeof(p));
                uint8_t buf[OPENDASH_ESPNOW_MAX_DATA]; uint16_t len = 0;
                if (opendash_i2c_serialize(&r, buf, &len) == OPENDASH_OK) {
                    opendash_espnow_send(s_center_mac, buf, len);
                }
            }
            for (uint8_t m = 0; m < OPENDASH_BOOST_MODES; ++m) {
                for (uint8_t g = 0; g < OPENDASH_BOOST_GEARS; ++g) {
                    opendash_boost_duty_row_t drow = { .mode = m, .gear = g };
                    if (opendash_boost_get_duty_row(m, g, drow.duty) == ESP_OK) {
                        opendash_i2c_msg_t r;
                        opendash_i2c_build_msg(&r, OPENDASH_CMD_BOOST_DUTY_REPORT, (uint8_t *)&drow, sizeof(drow));
                        uint8_t buf[OPENDASH_ESPNOW_MAX_DATA]; uint16_t len = 0;
                        if (opendash_i2c_serialize(&r, buf, &len) == OPENDASH_OK) {
                            opendash_espnow_send(s_center_mac, buf, len);
                        }
                    }
                    opendash_boost_setpoint_row_t srow = { .mode = m, .gear = g };
                    if (opendash_boost_get_setpoint_row(m, g, srow.setpoint_cbar) == ESP_OK) {
                        opendash_i2c_msg_t r;
                        opendash_i2c_build_msg(&r, OPENDASH_CMD_BOOST_SETP_REPORT, (uint8_t *)&srow, sizeof(srow));
                        uint8_t buf[OPENDASH_ESPNOW_MAX_DATA]; uint16_t len = 0;
                        if (opendash_i2c_serialize(&r, buf, &len) == OPENDASH_OK) {
                            opendash_espnow_send(s_center_mac, buf, len);
                        }
                    }
                    /* tiny inter-frame gap so we don't flood the radio */
                    vTaskDelay(pdMS_TO_TICKS(5));
                }
            }
            break;
        }

        default:
            ESP_LOGD(TAG, "Unhandled cmd: 0x%02X", msg->cmd);
            break;
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 * Boost loop tasks
 *
 * boost_compute_task — 50 Hz: pull params → run PID + safety → drive PWM.
 * boost_telemetry_task — 5 Hz: snapshot telemetry → unicast to center.
 *
 * These are deliberately split so a slow telemetry sender (radio backoff)
 * never delays a compute tick.
 * ════════════════════════════════════════════════════════════════════════════ */

static void boost_compute_task(void *arg)
{
    ESP_LOGI(TAG, "boost_compute_task started @ 50 Hz");
    const TickType_t period = pdMS_TO_TICKS(20);
    TickType_t last = xTaskGetTickCount();
    while (1) {
        vTaskDelayUntil(&last, period);

        opendash_boost_telemetry_t t;
        uint8_t duty = opendash_boost_compute(&t);

        /* Pull the active output channel from params each tick so that the
         * UI can re-route the solenoid at runtime without a reboot. */
        opendash_boost_params_t p;
        if (opendash_boost_get_params(&p) == ESP_OK && p.output_channel < 4) {
            opendash_relay_set_pwm(p.output_channel, duty);
        }
    }
}

static void boost_telemetry_task(void *arg)
{
    ESP_LOGI(TAG, "boost_telemetry_task started @ 5 Hz");
    const TickType_t period = pdMS_TO_TICKS(200);
    while (1) {
        vTaskDelay(period);
        if (!s_center_mac_known) continue;
        opendash_boost_telemetry_t t;
        opendash_boost_get_telemetry(&t);
        opendash_i2c_msg_t m;
        opendash_i2c_build_msg(&m, OPENDASH_CMD_BOOST_TELEMETRY, (uint8_t *)&t, sizeof(t));
        uint8_t buf[OPENDASH_ESPNOW_MAX_DATA]; uint16_t len = 0;
        if (opendash_i2c_serialize(&m, buf, &len) == OPENDASH_OK) {
            opendash_espnow_send(s_center_mac, buf, len);
        }
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

        /* Heartbeat every 57s — staggered: R4=45 R8A=49 R8B=53 MA=57 MB=61.
         * MOS is otherwise silent. No RELAY_STATUS broadcasts. */
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if ((now_ms - last_heartbeat_ms) >= 57000) {
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
    ESP_LOGI(TAG, "  OpenDash 4-Channel MOS FET Controller A");
    ESP_LOGI(TAG, "  Node: OPENDASH_NODE_MOS_4CH_A");
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
    ESP_ERROR_CHECK(opendash_espnow_init(OPENDASH_NODE_MOS_4CH_A));

    xTaskCreatePinnedToCore(espnow_task, "espnow_task", 4096, NULL, 5, NULL, 0);

    /* ── Boost controller (active per-gear N75 control) ──
     * Init first so the compute task always sees valid params/maps,
     * then spawn the two helper tasks on the APP core. */
    esp_err_t boost_ret = opendash_boost_init();
    if (boost_ret == ESP_OK) {
        xTaskCreatePinnedToCore(boost_compute_task,   "boost_compute",   4096, NULL, 6, NULL, 1);
        xTaskCreatePinnedToCore(boost_telemetry_task, "boost_telemetry", 4096, NULL, 4, NULL, 1);
        ESP_LOGI(TAG, "Boost controller online (output ch from params)");
    } else {
        ESP_LOGE(TAG, "Boost init failed: %s — N75 control disabled", esp_err_to_name(boost_ret));
    }

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

    ESP_LOGI(TAG, "MOS 4CH-A ready — waiting for commands (PWM capable)");

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
        if (!s_center_mac_known) {
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
