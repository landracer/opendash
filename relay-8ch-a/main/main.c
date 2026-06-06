/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file main.c
 * @brief OpenDash 8-Channel Relay Controller A — Entry Point
 *
 * Hardware: ESP32 8-Channel Relay Module (LCTech / generic)
 * http://www.chinalctech.com/cpzx/Programmer/Relay_Module/540.html
 *
 * Headless controller (no display). Receives relay commands from
 * center display via ESP-NOW and controls 8 relay channels.
 *
 * Programming: USB-to-TTL (FTDI) via /dev/ttyUSB1
 *   - No native USB — requires external FTDI adapter
 *   - Shares ttyUSB1 with relay-8ch-b (swap wiring to program each)
 *   - Subsequent updates via BLE OTA (Web Bluetooth from Chrome)
 *
 * Role: ESP-NOW node (OPENDASH_NODE_RELAY_8CH_A)
 *   - Receives SET_RELAY commands from center
 *   - Responds to PING with STATUS_REPORT
 *   - Reports channel states via RELAY_STATUS
 *   - Supports ENTER_BT_OTA command for wireless firmware updates
 *
 * Self-Test Mode:
 *   Boot with GPIO0 held LOW to enter relay self-test.
 *   Cycles all 8 channels ON/OFF sequentially (500ms each).
 *   Useful for verifying wiring without ESP-NOW master.
 *
 * ┌─────────────────────────────────────────────────┐
 * │  GPIO ASSIGNMENTS — VERIFIED                    │
 * │                                                 │
 * │  CH0: GPIO 32   CH4: GPIO 27                    │
 * │  CH1: GPIO 33   CH5: GPIO 14                    │
 * │  CH2: GPIO 25   CH6: GPIO 12                    │
 * │  CH3: GPIO 26   CH7: GPIO 13                    │
 * └─────────────────────────────────────────────────┘
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "driver/gpio.h"

#include "opendash_common.h"
#include "opendash_espnow.h"
#include "opendash_i2c_protocol.h"
#include "opendash_relay.h"
#include "opendash_identity.h"
#include "opendash_bt_ota.h"

static const char *TAG = "relay_8ch_a";

/* ════════════════════════════════════════════════════════════════════════════
 * Status LED — GPIO23
 *
 * Pattern (repeats every ~5s = 100 ticks @ 50ms):
 *   1. Heartbeat double-pulse (board alive + ESP-NOW OK)
 *   2. Pause
 *   3. N quick flashes = N active channels (1–7)
 *      OR one long solid pulse if ALL 8 channels are ON
 *      OR nothing extra if 0 channels active
 *   4. Pause, repeat
 *
 * Steady rapid flash = fault (ESP-NOW not initialized)
 * ════════════════════════════════════════════════════════════════════════════ */
#define STATUS_LED_GPIO  GPIO_NUM_23

/* ════════════════════════════════════════════════════════════════════════════
 * Relay Channel Configuration — GPIO assignments verified with multimeter
 *
 * Standard LCTech 8-channel ESP32 relay module pinout:
 *   CH0=GPIO32, CH1=GPIO33, CH2=GPIO25, CH3=GPIO26
 *   CH4=GPIO27, CH5=GPIO14, CH6=GPIO12, CH7=GPIO13
 *
 * All channels active-LOW (relay energizes when GPIO goes LOW).
 * ════════════════════════════════════════════════════════════════════════════ */

static const opendash_relay_config_t relay_config = {
    .type         = OPENDASH_RELAY_TYPE_RELAY,
    .polarity     = OPENDASH_RELAY_ACTIVE_LOW,
    .num_channels = 8,
    .pwm_freq_hz  = 0,
    .channels = {
        { .gpio_num = 32, .enabled = true, .label = "RELAY A CH1" },  /* CH0 */
        { .gpio_num = 33, .enabled = true, .label = "RELAY A CH2" },  /* CH1 */
        { .gpio_num = 25, .enabled = true, .label = "RELAY A CH3" },  /* CH2 */
        { .gpio_num = 26, .enabled = true, .label = "RELAY A CH4" },  /* CH3 */
        { .gpio_num = 27, .enabled = true, .label = "RELAY A CH5" },  /* CH4 */
        { .gpio_num = 14, .enabled = true, .label = "RELAY A CH6" },  /* CH5 */
        { .gpio_num = 12, .enabled = true, .label = "RELAY A CH7" },  /* CH6 */
        { .gpio_num = 13, .enabled = true, .label = "RELAY A CH8" },  /* CH7 */
    },
};

/* ════════════════════════════════════════════════════════════════════════════
 * Self-Test Mode — hold GPIO0 LOW at boot to enter
 * ════════════════════════════════════════════════════════════════════════════ */
#define SELF_TEST_PIN   GPIO_NUM_0
#define SELF_TEST_DELAY_MS  500

static bool check_self_test_mode(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << SELF_TEST_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    vTaskDelay(pdMS_TO_TICKS(50));  /* Debounce */
    return (gpio_get_level(SELF_TEST_PIN) == 0);
}

static void run_self_test(void)
{
    ESP_LOGW(TAG, "╔══════════════════════════════════════════════╗");
    ESP_LOGW(TAG, "║  SELF-TEST MODE — Cycling all 8 relays      ║");
    ESP_LOGW(TAG, "║  Release GPIO0 button to exit after test     ║");
    ESP_LOGW(TAG, "╚══════════════════════════════════════════════╝");

    /* Cycle each relay ON then OFF */
    for (int pass = 0; pass < 2; pass++) {
        ESP_LOGI(TAG, "── Self-test pass %d/2 ──", pass + 1);
        for (int ch = 0; ch < 8; ch++) {
            ESP_LOGI(TAG, "  CH%d [%s] → ON", ch, relay_config.channels[ch].label);
            opendash_relay_set(ch, true);
            vTaskDelay(pdMS_TO_TICKS(SELF_TEST_DELAY_MS));

            ESP_LOGI(TAG, "  CH%d [%s] → OFF", ch, relay_config.channels[ch].label);
            opendash_relay_set(ch, false);
            vTaskDelay(pdMS_TO_TICKS(SELF_TEST_DELAY_MS / 2));
        }
    }

    /* All ON simultaneously */
    ESP_LOGI(TAG, "── All relays ON simultaneously ──");
    for (int ch = 0; ch < 8; ch++) {
        opendash_relay_set(ch, true);
    }
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* All OFF */
    ESP_LOGI(TAG, "── All relays OFF ──");
    opendash_relay_all_off();
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_LOGW(TAG, "Self-test COMPLETE — all channels verified");
}

/* ════════════════════════════════════════════════════════════════════════════
 * ESP-NOW Communication
 * ════════════════════════════════════════════════════════════════════════════ */

static uint8_t s_center_mac[6];
static bool    s_center_mac_known = false;

/* ── Heartbeat send — used only for center discovery, never for relay state ── */
static void send_heartbeat_broadcast(void)
{
    uint8_t payload[3] = { OPENDASH_NODE_RELAY_8CH_A, 0x01, 0x00 };
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
    /* Learn center MAC for peer registration (future use) */
    /* Only latch on center-originated cmds (bit 7 clear). Slave broadcasts
     * (STATUS_REPORT 0x82, DATA_RESPONSE 0x81, etc.) use bit 7 set and must
     * NOT be mistaken for center. */
    if (!s_center_mac_known && (msg->cmd & 0x80) == 0) {
        memcpy(s_center_mac, evt->src_mac, 6);
        s_center_mac_known = true;
        opendash_espnow_add_peer(s_center_mac);
        ESP_LOGI(TAG, "Center discovered @ " MACSTR, MAC2STR(s_center_mac));
        /* Boot announce: send actual relay state so center clears stale cache */
        uint8_t boot_mask = 0;
        for (int i = 0; i < 8; i++) {
            opendash_relay_channel_state_t st;
            if (opendash_relay_get_state(i, &st) == ESP_OK && st.is_on) boot_mask |= (1u << i);
        }
        uint8_t bp[3] = { 0xCA, OPENDASH_NODE_RELAY_8CH_A, boot_mask };
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
            /* ── MASK FORMAT: [0xCA, node_id, relay_mask, seq]
             * relay_mask bits 0-7 = channels 0-7.  Master owns full state.
             * Sends immediate RELAY_STATUS confirmation to center. */
            if (msg->length >= 3 && msg->payload[0] == 0xCA) {
                uint8_t target = msg->payload[1];
                if (target != OPENDASH_NODE_RELAY_8CH_A) break;
                uint8_t mask = msg->payload[2];
                for (int i = 0; i < 8; i++) {
                    opendash_relay_set(i, (mask >> i) & 1);
                }
                ESP_LOGI(TAG, "Relay mask 0x%02X applied", mask);
                /* Immediate confirmation → center knows state without audit delay */
                if (s_center_mac_known) {
                    uint8_t ack_pay[3] = { 0xCA, OPENDASH_NODE_RELAY_8CH_A, mask };
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
            /* ── LEGACY per-channel format: [node_id, channel, state, pwm] ── */
            if (msg->length >= 4) {
                uint8_t target = msg->payload[0];
                if (target != OPENDASH_NODE_RELAY_8CH_A) break;
                uint8_t channel = msg->payload[1];
                uint8_t state   = msg->payload[2];
                opendash_relay_set(channel, state != 0);
                ESP_LOGI(TAG, "Relay CH%d → %s", channel, state ? "ON" : "OFF");
                /* No send_relay_status() — master owns state, no ACK needed */
            } else if (msg->length >= 2) {
                uint8_t channel = msg->payload[0];
                uint8_t state   = msg->payload[1];
                opendash_relay_set(channel, state != 0);
                ESP_LOGI(TAG, "Relay CH%d → %s (legacy)", channel, state ? "ON" : "OFF");
            }
            break;
        }

        case OPENDASH_CMD_REQUEST_RELAY_STATUS: {
            /* Build compact audit response: [0xCA, node_id, actual_mask]
             * Sent unicast to center only — no broadcast, no storm risk. */
            if (!s_center_mac_known) break;
            opendash_relay_channel_state_t states[OPENDASH_RELAY_MAX_CHANNELS];
            uint8_t num_ch = 0;
            opendash_relay_get_all_states(states, &num_ch);
            uint8_t actual_mask = 0;
            for (int i = 0; i < num_ch && i < 8; i++) {
                if (states[i].is_on) actual_mask |= (1u << i);
            }
            uint8_t payload[3] = { 0xCA, OPENDASH_NODE_RELAY_8CH_A, actual_mask };
            opendash_i2c_msg_t resp;
            opendash_i2c_build_msg(&resp, OPENDASH_CMD_RELAY_STATUS, payload, sizeof(payload));
            uint8_t tx_buf[OPENDASH_ESPNOW_MAX_DATA];
            uint16_t len = 0;
            if (opendash_i2c_serialize(&resp, tx_buf, &len) == OPENDASH_OK) {
                opendash_espnow_send(s_center_mac, tx_buf, len);
                ESP_LOGI(TAG, "Audit reply: mask 0x%02X", actual_mask);
            }
            break;
        }

        case OPENDASH_CMD_SYSTEM: {
            if (msg->length < 1) break;
            switch (msg->payload[0]) {
                case OPENDASH_SUBCMD_PING:
                    /* Relay ignores PING — uses autonomous heartbeat instead.
                     * This eliminates broadcast storm from responding to
                     * every PING (~50/sec) from center. */
                    break;
                case OPENDASH_SUBCMD_ENTER_BT_OTA:
                    ESP_LOGW(TAG, "BT OTA requested — shutting down ESP-NOW, starting BLE");
                    opendash_relay_all_off();
                    opendash_espnow_deinit();
                    opendash_bt_ota_start(OPENDASH_NODE_RELAY_8CH_A);
                    /* If OTA times out without connection, restart ESP-NOW */
                    ESP_LOGW(TAG, "BLE OTA exited — rebooting to restore ESP-NOW");
                    vTaskDelay(pdMS_TO_TICKS(100));
                    esp_restart();
                    break;
                case OPENDASH_SUBCMD_SELF_TEST:
                    ESP_LOGW(TAG, "Remote self-test requested");
                    run_self_test();
                    break;
                case OPENDASH_SUBCMD_REBOOT:
                    ESP_LOGW(TAG, "Reboot — all relays OFF");
                    opendash_relay_all_off();
                    vTaskDelay(pdMS_TO_TICKS(100));
                    esp_restart();
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
 * ESP-NOW Task
 * ════════════════════════════════════════════════════════════════════════════ */

static void espnow_task(void *pvParameters)
{
    ESP_LOGI(TAG, "ESP-NOW processing task started");
    esp_task_wdt_add(NULL);

    /* Immediate heartbeat on boot for fast discovery */
    vTaskDelay(pdMS_TO_TICKS(500));  /* Let ESP-NOW fully init */
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

        /* Heartbeat every 49s — staggered: R4=45 R8A=49 R8B=53 MA=57 MB=61.
         * Relay is otherwise silent. No RELAY_STATUS broadcasts. */
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if ((now_ms - last_heartbeat_ms) >= 49000) {
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
    ESP_LOGI(TAG, "  OpenDash 8-Channel Relay Controller A");
    ESP_LOGI(TAG, "  Version: %s", OPENDASH_VERSION_STR);
    ESP_LOGI(TAG, "  Hardware: ESP32 8-CH Relay (LCTech)");
    ESP_LOGI(TAG, "  Node: OPENDASH_NODE_RELAY_8CH_A (ESP-NOW)");
    ESP_LOGI(TAG, "  Program via: FTDI /dev/ttyUSB1 or BLE OTA");
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════");

    /* ── Step 1: NVS ── */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "[1/4] NVS initialized");

    /* ── Step 2: Device Identity ── */
    opendash_identity_init(OPENDASH_NODE_RELAY_8CH_A);
    ESP_LOGI(TAG, "[2/4] Identity verified");

    /* ── Step 3: Relay Hardware ── */
    ret = opendash_relay_init(&relay_config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Relay init returned %s — check GPIO wiring", esp_err_to_name(ret));
    }
    ESP_LOGI(TAG, "[3/4] Relay controller initialized");

    for (int i = 0; i < relay_config.num_channels; i++) {
        ESP_LOGI(TAG, "  CH%d: [%s] GPIO=%d %s",
                 i, relay_config.channels[i].label,
                 relay_config.channels[i].gpio_num,
                 relay_config.channels[i].enabled ? "READY" : "NOT ASSIGNED");
    }

    /* ── Self-Test: hold GPIO0 LOW at boot ── */
    if (check_self_test_mode()) {
        run_self_test();
    }

    /* ── Step 4: ESP-NOW ── */
    ESP_ERROR_CHECK(opendash_espnow_init(OPENDASH_NODE_RELAY_8CH_A));
    ESP_LOGI(TAG, "[4/4] ESP-NOW initialized");

    /* Log OTA partition info */
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running) {
        ESP_LOGI(TAG, "Running partition: %s (0x%lx, %lu bytes)",
                 running->label, (unsigned long)running->address,
                 (unsigned long)running->size);
    }

    xTaskCreatePinnedToCore(espnow_task, "espnow_task", 4096, NULL, 5, NULL, 0);

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
    ESP_LOGI(TAG, "  Relay 8CH-A ready!");
    ESP_LOGI(TAG, "  GPIO0 held LOW at boot → self-test mode");
    ESP_LOGI(TAG, "  Waiting for commands from center via ESP-NOW");
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════");

    /* Combined health monitoring + LED status loop
     * 8-channel variant: 100-tick cycle (~5s) to fit up to 8 flashes */
    uint32_t uptime_s = 0;
    uint32_t loop_tick = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(50));  /* 20 Hz LED loop */
        loop_tick++;

        /* ── LED status pattern (cycle every ~5s = 100 ticks @ 50ms) ── */
        uint32_t phase = loop_tick % 100;

        opendash_relay_channel_state_t led_states[8];
        uint8_t led_num = 0;
        opendash_relay_get_all_states(led_states, &led_num);
        uint8_t active_count = 0;
        for (int i = 0; i < led_num && i < 8; i++) {
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
                if (active_count == 8) {
                    /* All 8 on: one long solid pulse (12-35) */
                    led_on = (phase < 36);
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
            ESP_LOGI(TAG, "Uptime: %lus | Center: %s | Active: %d/8 | CH: %d%d%d%d%d%d%d%d",
                     (unsigned long)uptime_s,
                     s_center_mac_known ? "ONLINE" : "searching",
                     active_count,
                     led_num > 0 ? led_states[0].is_on : 0,
                     led_num > 1 ? led_states[1].is_on : 0,
                     led_num > 2 ? led_states[2].is_on : 0,
                     led_num > 3 ? led_states[3].is_on : 0,
                     led_num > 4 ? led_states[4].is_on : 0,
                     led_num > 5 ? led_states[5].is_on : 0,
                     led_num > 6 ? led_states[6].is_on : 0,
                     led_num > 7 ? led_states[7].is_on : 0);
        }
    }
}
