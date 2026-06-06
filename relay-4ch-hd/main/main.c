/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file main.c
 * @brief OpenDash 4-Channel HD Relay Controller — Entry Point
 *
 * Hardware: ESP32 4-CH HD Relay Module (LCTech)
 * http://www.chinalctech.com/cpzx/Programmer/Relay_Module/788.html
 *
 * Headless controller (no display). Receives relay commands from
 * center display via ESP-NOW and controls 4 high-amp relays.
 *
 * Intended loads: radiator fans, water pumps, fuel pumps, etc.
 *
 * Role: ESP-NOW node (OPENDASH_NODE_RELAY_4CH)
 *   - Receives SET_RELAY commands from center
 *   - Responds to PING with STATUS_REPORT
 *   - Reports channel states via RELAY_STATUS
 *
 * ┌─────────────────────────────────────────────────┐
 * │  GPIO ASSIGNMENTS — TBD                         │
 * │                                                 │
 * │  The actual GPIO→relay mapping must be          │
 * │  discovered with a multimeter when the          │
 * │  hardware arrives. Update the gpio_num          │
 * │  fields in relay_config below.                  │
 * │                                                 │
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
#include "opendash_bt_ota.h"
#include "opendash_i2c_protocol.h"
#include "opendash_relay.h"

static const char *TAG = "relay_4ch_hd";

/* ════════════════════════════════════════════════════════════════════════════
 * Status LED — GPIO5
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
#define STATUS_LED_GPIO  GPIO_NUM_5

/* ════════════════════════════════════════════════════════════════════════════
 * Relay Channel Configuration
 *
 * USER: Update gpio_num values after physical pinout discovery!
 * Set enabled = true for channels with known GPIOs.
 * Labels are displayed on the center display relay control screen.
 * ════════════════════════════════════════════════════════════════════════════ */

static const opendash_relay_config_t relay_config = {
    .type         = OPENDASH_RELAY_TYPE_RELAY,
    .polarity     = OPENDASH_RELAY_ACTIVE_LOW,  /* Most relay modules are active-low */
    .num_channels = 4,
    .pwm_freq_hz  = 0,  /* N/A for relay */
    .channels = {
        /* ── Channel labels shown on center relay control screen ─── */
        { .gpio_num = 12, .enabled = true, .label = "RAD FAN 1"   },  /* CH0 */
        { .gpio_num = 13, .enabled = true, .label = "RAD FAN 2"   },  /* CH1 */
        { .gpio_num = 14, .enabled = true, .label = "WATER PUMP"  },  /* CH2 */
        { .gpio_num = 15, .enabled = true, .label = "FUEL PUMP"   },  /* CH3 */
    },
};

/* ════════════════════════════════════════════════════════════════════════════
 * ESP-NOW Communication
 * ════════════════════════════════════════════════════════════════════════════ */

static uint8_t s_center_mac[6];
static bool    s_center_mac_known = false;

/* ── Heartbeat send — used only for center discovery, never for relay state ── */
static void send_heartbeat_broadcast(void)
{
    uint8_t payload[3] = { OPENDASH_NODE_RELAY_4CH, 0x01, 0x00 };
    opendash_i2c_msg_t msg;
    opendash_i2c_build_msg(&msg, OPENDASH_CMD_STATUS_REPORT, payload, sizeof(payload));
    uint8_t tx_buf[OPENDASH_ESPNOW_MAX_DATA];
    uint16_t len = 0;
    if (opendash_i2c_serialize(&msg, tx_buf, &len) == OPENDASH_OK) {
        opendash_espnow_broadcast(tx_buf, len);
    }
}

/**
 * @brief Process a single received ESP-NOW message.
 */
static void dispatch_message(const opendash_espnow_event_t *evt,
                              const opendash_i2c_msg_t *msg)
{
    /* Only latch on center-originated cmds (bit 7 clear). Slave broadcasts
     * (STATUS_REPORT 0x82, DATA_RESPONSE 0x81, etc.) use bit 7 set and must
     * NOT be mistaken for center. */
    if (!s_center_mac_known && (msg->cmd & 0x80) == 0) {
        memcpy(s_center_mac, evt->src_mac, 6);
        s_center_mac_known = true;
        opendash_espnow_add_peer(s_center_mac);
        ESP_LOGI(TAG, "Center discovered @ " MACSTR, MAC2STR(s_center_mac));
        /* Boot announce: send actual relay state (0x00 on fresh boot) so
         * center clears any stale cached state from before power cycle. */
        uint8_t boot_mask = 0;
        for (int i = 0; i < 4; i++) {
            opendash_relay_channel_state_t st;
            if (opendash_relay_get_state(i, &st) == ESP_OK && st.is_on) boot_mask |= (1u << i);
        }
        uint8_t bp[3] = { 0xCA, OPENDASH_NODE_RELAY_4CH, boot_mask };
        opendash_i2c_msg_t bm;
        opendash_i2c_build_msg(&bm, OPENDASH_CMD_RELAY_STATUS, bp, sizeof(bp));
        uint8_t bb[OPENDASH_ESPNOW_MAX_DATA]; uint16_t bl = 0;
        if (opendash_i2c_serialize(&bm, bb, &bl) == OPENDASH_OK) {
            opendash_espnow_send(s_center_mac, bb, bl);
        }
        ESP_LOGI(TAG, "Boot state announce: mask 0x%02X", boot_mask);
    }

    switch (msg->cmd) {
        /* ── Set Relay Channel ─────────────────────────────── */
        case OPENDASH_CMD_SET_RELAY: {
            /* ── MASK FORMAT: [0xCA, node_id, mask, seq]
             * bits 0-3 = channels 0-3. Master owns full state.
             * Sends immediate RELAY_STATUS confirmation to center. */
            if (msg->length >= 3 && msg->payload[0] == 0xCA) {
                uint8_t target = msg->payload[1];
                if (target != OPENDASH_NODE_RELAY_4CH) break;
                uint8_t mask = msg->payload[2];
                for (int i = 0; i < 4; i++) {
                    opendash_relay_set(i, (mask >> i) & 1);
                }
                ESP_LOGI(TAG, "HD relay mask 0x%02X applied", mask);
                /* Immediate confirmation → center knows state without audit delay */
                if (s_center_mac_known) {
                    uint8_t ack_pay[3] = { 0xCA, OPENDASH_NODE_RELAY_4CH, mask };
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
            /* ── LEGACY per-channel format: [channel, state] ── */
            if (msg->length >= 2) {
                uint8_t channel = msg->payload[0];
                uint8_t state   = msg->payload[1];

                esp_err_t ret = opendash_relay_set(channel, state != 0);
                if (ret == ESP_OK) {
                    ESP_LOGI(TAG, "Relay CH%d → %s", channel, state ? "ON" : "OFF");
                } else {
                    ESP_LOGW(TAG, "Relay CH%d set failed: %s", channel, esp_err_to_name(ret));
                }
                /* No send_relay_status() — master owns state, no ACK needed */
            }
            break;
        }

        /* ── Request Relay Status (audit response) ──────────────── */
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
            uint8_t audit_pay[3] = { 0xCA, OPENDASH_NODE_RELAY_4CH, actual_mask };
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

        /* ── System Commands ───────────────────────────────── */
        case OPENDASH_CMD_SYSTEM: {
            if (msg->length < 1) break;

            switch (msg->payload[0]) {
                case OPENDASH_SUBCMD_PING:
                    /* Relay ignores PING — uses autonomous heartbeat instead.
                     * Eliminates broadcast storm from responding to every PING. */
                    break;
                case OPENDASH_SUBCMD_REBOOT:
                    ESP_LOGW(TAG, "Reboot requested — turning all relays OFF first");
                    opendash_relay_all_off();
                    vTaskDelay(pdMS_TO_TICKS(100));
                    esp_restart();
                    break;
                case OPENDASH_SUBCMD_ENTER_BT_OTA:
                    ESP_LOGW(TAG, "Entering BLE OTA mode — all relays OFF");
                    opendash_relay_all_off();
                    opendash_bt_ota_enter(OPENDASH_NODE_RELAY_4CH,
                                          s_center_mac_known ? s_center_mac : NULL);
                    break;
                case OPENDASH_SUBCMD_SELF_TEST:
                    ESP_LOGW(TAG, "Self-test: cycling all relay channels");
                    for (uint8_t ch = 0; ch < OPENDASH_RELAY_MAX_CHANNELS; ch++) {
                        opendash_relay_set(ch, true);
                        vTaskDelay(pdMS_TO_TICKS(250));
                        opendash_relay_set(ch, false);
                        vTaskDelay(pdMS_TO_TICKS(150));
                    }
                    ESP_LOGI(TAG, "Self-test complete");
                    break;
                default:
                    break;
            }
            break;
        }

        default:
            ESP_LOGD(TAG, "Unhandled cmd: 0x%02X", msg->cmd);
            break;
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 * ESP-NOW Message Processing Task
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
            opendash_err_t ret = opendash_i2c_deserialize(evt.data, evt.len, &msg);
            if (ret == OPENDASH_OK) {
                dispatch_message(&evt, &msg);
            }
        }

        /* Heartbeat every 45s — staggered: R4=45 R8A=49 R8B=53 MA=57 MB=61.
         * Relay is otherwise silent. No RELAY_STATUS broadcasts. */
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if ((now_ms - last_heartbeat_ms) >= 45000) {
            last_heartbeat_ms = now_ms;
            send_heartbeat_broadcast();
        }

        vTaskDelay(pdMS_TO_TICKS(10));  /* 100 Hz poll */
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 * Main Entry Point
 * ════════════════════════════════════════════════════════════════════════════ */

void app_main(void)
{
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════");
    ESP_LOGI(TAG, "  OpenDash 4-Channel HD Relay Controller");
    ESP_LOGI(TAG, "  Version: %s", OPENDASH_VERSION_STR);
    ESP_LOGI(TAG, "  Hardware: ESP32 4-CH HD Relay (LCTech)");
    ESP_LOGI(TAG, "  Node: OPENDASH_NODE_RELAY_4CH (ESP-NOW)");
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════");

    /* ── Step 1: NVS ── */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "[1/3] NVS initialized");

    /* ── Step 2: Relay Hardware ── */
    ret = opendash_relay_init(&relay_config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Relay init failed (GPIOs not assigned yet?) — continuing");
    }
    ESP_LOGI(TAG, "[2/3] Relay controller initialized");

    /* Log channel labels */
    for (int i = 0; i < relay_config.num_channels; i++) {
        ESP_LOGI(TAG, "  CH%d: [%s] GPIO=%d %s",
                 i, relay_config.channels[i].label,
                 relay_config.channels[i].gpio_num,
                 relay_config.channels[i].enabled ? "READY" : "NOT ASSIGNED");
    }

    /* ── Step 3: ESP-NOW ── */
    ESP_ERROR_CHECK(opendash_espnow_init(OPENDASH_NODE_RELAY_4CH));
    ESP_LOGI(TAG, "[3/3] ESP-NOW initialized");

    /* Start ESP-NOW task */
    xTaskCreatePinnedToCore(
        espnow_task, "espnow_task", 4096, NULL,
        5, NULL, 0);

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

    ESP_LOGI(TAG, "═══════════════════════════════════════════════════");
    ESP_LOGI(TAG, "  Relay 4CH-HD ready!");
    ESP_LOGI(TAG, "  Waiting for commands from center via ESP-NOW");
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════");

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
            /* BLE OTA mode: 3 quick pulses then long pause (~3s cycle).
             * Distinct from heartbeat (2 pulses) and fault (rapid flash). */
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
            ESP_LOGI(TAG, "Uptime: %lus | Center: %s | Active: %d/4 | CH: %d%d%d%d",
                     (unsigned long)uptime_s,
                     s_center_mac_known ? "ONLINE" : "searching",
                     active_count,
                     led_num > 0 ? led_states[0].is_on : 0,
                     led_num > 1 ? led_states[1].is_on : 0,
                     led_num > 2 ? led_states[2].is_on : 0,
                     led_num > 3 ? led_states[3].is_on : 0);
        }
    }
}
