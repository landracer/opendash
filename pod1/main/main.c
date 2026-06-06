/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file main.c
 * @brief OpenDash Pod 1 — Entry Point
 *
 * Hardware: Waveshare ESP32-S3-Touch-AMOLED-1.75
 * Resolution: 466×466 Round AMOLED (CO5300 QSPI)
 * IMU: QMI8658 6-axis accelerometer + gyroscope (I2C)
 * Touch: CST9217 (I2C, managed by display_init)
 * NO GPS — display-only pod, receives engine data from center via ESP-NOW
 *
 * Role: ESP-NOW node (OPENDASH_NODE_POD1)
 *   - Receives SET_DATA_POINT from center (engine data for display)
 *   - Broadcasts IMU data to center for parachute deployment voting
 *   - Responds to center PINGs with STATUS_REPORT
 *
 * Display Screens (cycle with touch swipe or boot button):
 *   OIL_TEMP  — Oil temperature (large) + info panel
 *   WATER     — Coolant temperature (large) + info panel
 *   AFR       — Air-fuel ratio (large) + info panel
 *   BOOST     — Boost pressure (large) + info panel
 *   GFORCE    — G-force display with lateral/longitudinal/total
 *   DEBUG     — System diagnostics + ESP-NOW stats
 *
 * Initialization Order:
 *   1. NVS
 *   2. Config load
 *   3. Display init (I2C bus, CO5300, CST9217, LVGL)
 *   4. Splash screen
 *   5. IMU init (QMI8658 via shared I2C)
 *   6. UI init
 *   7. ESP-NOW init
 *   8. Start all tasks
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "nvs_flash.h"

#include "display_init.h"
#include "ui_manager.h"
#include "imu_handler.h"
#include "opendash_common.h"
#include "opendash_display_config.h"
#include "opendash_espnow.h"
#include "opendash_bt_ota.h"
#include "opendash_i2c_protocol.h"
#include "opendash_data_model.h"
#include "opendash_parachute.h"
#include "opendash_rollover.h"
#include "opendash_rtc.h"
#include "opendash_audio.h"
#include "esp_task_wdt.h"

/* Splash screen image */
#if __has_include("splash_pod1.h")
#include "splash_pod1.h"
#define HAS_SPLASH 1
#else
#define HAS_SPLASH 0
#endif

static const char *TAG = "opendash_pod1";

/* ════════════════════════════════════════════════════════════════════════════
 * ESP-NOW Communication
 * ════════════════════════════════════════════════════════════════════════════ */

static uint8_t s_center_mac[6];
static bool    s_center_mac_known = false;
static uint8_t s_tx_buf[OPENDASH_ESPNOW_MAX_DATA];

static esp_err_t send_to_center(const opendash_i2c_msg_t *msg)
{
    uint16_t len;
    if (opendash_i2c_serialize(msg, s_tx_buf, &len) != OPENDASH_OK) {
        return ESP_FAIL;
    }
    if (s_center_mac_known) {
        return opendash_espnow_send(s_center_mac, s_tx_buf, len);
    }
    return opendash_espnow_broadcast(s_tx_buf, len);
}

static esp_err_t send_data_point(uint16_t dp_id, float value)
{
    uint8_t payload[6];
    payload[0] = (dp_id >> 8) & 0xFF;
    payload[1] = dp_id & 0xFF;
    memcpy(&payload[2], &value, sizeof(float));

    opendash_i2c_msg_t msg;
    opendash_i2c_build_msg(&msg, OPENDASH_CMD_DATA_RESPONSE, payload, sizeof(payload));
    return send_to_center(&msg);
}

/* Rollover detector IMU read shim: live roll angle + roll rate for the shared
 * detector module (see opendash_rollover.h / opendash_rollover_detector.c). */
static bool rollover_read(float *roll_deg, float *roll_rate)
{
    imu_data_t d;
    if (imu_handler_get_data(&d) != ESP_OK) return false;
    *roll_deg  = OPENDASH_ROLLOVER_ROLL_ANGLE(d);
    *roll_rate = OPENDASH_ROLLOVER_ROLL_RATE(d);
    return true;
}

/**
 * @brief Process a single received ESP-NOW message.
 */
static void dispatch_message(const opendash_espnow_event_t *evt,
                              const opendash_i2c_msg_t *msg)
{
    /* Learn center's MAC from first valid message */
    /* Only latch on center-originated cmds (bit 7 clear). Slave broadcasts
     * (STATUS_REPORT 0x82, DATA_RESPONSE 0x81, etc.) use bit 7 set and must
     * NOT be mistaken for center. */
    if (!s_center_mac_known && (msg->cmd & 0x80) == 0) {
        memcpy(s_center_mac, evt->src_mac, 6);
        s_center_mac_known = true;
        opendash_espnow_add_peer(s_center_mac);
        ESP_LOGI(TAG, "Center discovered @ " MACSTR " (RSSI=%d dBm)",
                 MAC2STR(s_center_mac), evt->rssi);
    }

    switch (msg->cmd) {
        /* ── Data Point Update from Center ─────────────────── */
        case OPENDASH_CMD_SET_DATA_POINT: {
            if (msg->length >= 6) {
                uint16_t dp_id = (msg->payload[0] << 8) | msg->payload[1];
                float value;
                memcpy(&value, &msg->payload[2], sizeof(float));

                if (display_lvgl_lock(10)) {
                    ui_manager_update_value(dp_id, value);
                    display_lvgl_unlock();
                }
                ESP_LOGD(TAG, "SET_DATA dp=0x%04X val=%.2f", dp_id, value);
            }
            break;
        }

        /* ── Brightness ────────────────────────────────────── */
        case OPENDASH_CMD_SET_BRIGHTNESS: {
            if (msg->length >= 1) {
                uint8_t pct = (msg->payload[0] * 100) / 255;
                display_set_brightness(pct);
            }
            break;
        }

        /* ── System Commands ───────────────────────────────── */
        case OPENDASH_CMD_SYSTEM: {
            if (msg->length < 1) break;

            switch (msg->payload[0]) {
                /* PING removed — channel-based architecture uses boot ANNOUNCE
                 * + staggered heartbeats. Center discovers nodes from their
                 * STATUS_REPORT broadcasts, not from polling. */
                case OPENDASH_SUBCMD_REBOOT:
                    ESP_LOGW(TAG, "Reboot requested via ESP-NOW");
                    esp_restart();
                    break;
                case OPENDASH_SUBCMD_ENTER_BT_OTA: {
                    ESP_LOGW(TAG, "Entering BLE OTA mode");
                    /* Blank the display so it's obvious the unit is mid-flash
                     * rather than running normally. */
                    if (display_lvgl_lock(200)) {
                        lv_obj_t *ota_scr = lv_obj_create(NULL);
                        lv_obj_set_style_bg_color(ota_scr, lv_color_hex(0x000000), 0);
                        lv_obj_set_style_bg_opa(ota_scr, LV_OPA_COVER, 0);
                        lv_obj_t *t1 = lv_label_create(ota_scr);
                        lv_label_set_text(t1, "WAITING FOR OTA");
                        lv_obj_set_style_text_color(t1, lv_color_hex(0x00CCFF), 0);
                        lv_obj_align(t1, LV_ALIGN_CENTER, 0, -30);
                        lv_obj_t *t2 = lv_label_create(ota_scr);
                        lv_label_set_text(t2, "POD1");
                        lv_obj_set_style_text_color(t2, lv_color_hex(0xFFFFFF), 0);
                        lv_obj_align(t2, LV_ALIGN_CENTER, 0, 5);
                        lv_obj_t *t3 = lv_label_create(ota_scr);
                        lv_label_set_text(t3, "Reset via BOOT button\nor center reset");
                        lv_obj_set_style_text_color(t3, lv_color_hex(0xAAAAAA), 0);
                        lv_obj_set_style_text_align(t3, LV_TEXT_ALIGN_CENTER, 0);
                        lv_obj_align(t3, LV_ALIGN_CENTER, 0, 45);
                        lv_screen_load(ota_scr);
                        display_lvgl_unlock();
                    }
                    opendash_bt_ota_enter(OPENDASH_NODE_POD1,
                                          s_center_mac_known ? s_center_mac : NULL);
                    break;
                }
                case OPENDASH_SUBCMD_TIME_SYNC: {
                    if (msg->length >= 9) {
                        ESP_LOGD(TAG, "TIME_SYNC: %02d:%02d:%02d UTC",
                                 msg->payload[1], msg->payload[2], msg->payload[3]);
                    }
                    break;
                }
                default:
                    ESP_LOGD(TAG, "Unknown system subcmd: 0x%02X", msg->payload[0]);
                    break;
            }
            break;
        }

        /* ── Data Request from Center (IMU data) ──────────── */
        case OPENDASH_CMD_REQUEST_DATA: {
            if (msg->length >= 2) {
                uint16_t dp_id = (msg->payload[0] << 8) | msg->payload[1];
                imu_data_t imu = {0};
                float value = 0.0f;
                bool found = true;

                switch (dp_id) {
                    case OPENDASH_DP_GFORCE_LAT:   imu_handler_get_data(&imu); value = imu.g_lateral;        break;
                    case OPENDASH_DP_GFORCE_LONG:   imu_handler_get_data(&imu); value = imu.g_longitudinal;   break;
                    case OPENDASH_DP_GFORCE_VERT:   imu_handler_get_data(&imu); value = imu.g_vertical;       break;
                    default: found = false; break;
                }
                if (found) {
                    send_data_point(dp_id, value);
                }
            }
            break;
        }

        /* ── Parachute config push (Center → detector) ──── */
        case OPENDASH_CMD_PARACHUTE_SET_CONFIG: {
            if (msg->length >= sizeof(opendash_parachute_config_t)) {
                opendash_parachute_config_t cfg;
                memcpy(&cfg, msg->payload, sizeof(cfg));
                opendash_parachute_config_set(&cfg);
                opendash_rollover_detector_send_status();  /* echo for center confirm */
                ESP_LOGI(TAG, "Deploy cfg set: %s roll=%.0f deg/%ums rate=%.0f",
                         cfg.enabled ? "ENABLED" : "disabled",
                         cfg.roll_deploy_deg, cfg.sustain_ms, cfg.roll_rate_deg_s);
            }
            break;
        }

        /* ── Parachute status pull (Center → detector) ──── */
        case OPENDASH_CMD_PARACHUTE_PULL_ALL: {
            opendash_rollover_detector_send_status();
            break;
        }

        /* ── Parachute zero/cal (Center → detector) ──────── */
        case OPENDASH_CMD_PARACHUTE_CALIBRATE: {
            opendash_rollover_detector_calibrate();
            break;
        }

        /* ── Audio Alert from Center ───────────────────── */
        case OPENDASH_CMD_AUDIO_ALERT: {
            if (msg->length >= 2) {
                opendash_audio_alert_t alert = {
                    .sound_id    = msg->payload[0],
                    .priority    = msg->payload[1],
                    .duration_ms = (msg->length >= 4)
                        ? (uint16_t)(msg->payload[2] | (msg->payload[3] << 8))
                        : 0,
                };
                opendash_audio_play(&alert);
                ESP_LOGI(TAG, "Audio alert: snd=%d pri=%d dur=%dms",
                         alert.sound_id, alert.priority, alert.duration_ms);
            }
            break;
        }

        default:
            ESP_LOGD(TAG, "Unhandled cmd: 0x%02X", msg->cmd);
            break;
    }
}

static void process_espnow_messages(void)
{
    opendash_espnow_event_t evt;
    int processed = 0;

    while (opendash_espnow_recv(&evt, 0)) {
        opendash_i2c_msg_t msg;
        opendash_err_t ret = opendash_i2c_deserialize(evt.data, evt.len, &msg);
        if (ret != OPENDASH_OK) {
            ESP_LOGD(TAG, "Invalid ESP-NOW msg from " MACSTR " (len=%d)",
                     MAC2STR(evt.src_mac), evt.len);
            continue;
        }
        dispatch_message(&evt, &msg);

        if (++processed >= 4) {
            vTaskDelay(1);
            processed = 0;
        }
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 * IMU Data Broadcasting Task
 *
 * Proactively pushes IMU data to center at 5 Hz for:
 *   - Local g-force display
 *   - Parachute deployment voting (multi-node IMU consensus)
 * ════════════════════════════════════════════════════════════════════════════ */

static void data_broadcast_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Data broadcast task started (5 Hz)");
    esp_task_wdt_add(NULL);
    TickType_t last_wake = xTaskGetTickCount();
    uint32_t cycle = 1;  /* start at 1 so boot-broadcast doesn't double-fire */

    while (1) {
        esp_task_wdt_reset();

        /* ── Process incoming ESP-NOW messages ──── */
        process_espnow_messages();

        /* ── Broadcast IMU data ──────────────────── */
        imu_data_t imu = {0};
        imu_handler_get_data(&imu);

        send_data_point(OPENDASH_DP_GFORCE_LAT,   imu.g_lateral);
        send_data_point(OPENDASH_DP_GFORCE_LONG,  imu.g_longitudinal);
        send_data_point(OPENDASH_DP_GFORCE_VERT,  imu.g_vertical);
        esp_task_wdt_reset();

        /* ── Heartbeat STATUS_REPORT every 39s (195 cycles @ 5Hz) — staggered: P1=39 P2=42 ── */
        if ((cycle % 195) == 0) {
            uint8_t hb_payload[3] = { OPENDASH_NODE_POD1, 0x01, 0x00 };
            opendash_i2c_msg_t hb_msg;
            opendash_i2c_build_msg(&hb_msg, OPENDASH_CMD_STATUS_REPORT,
                                   hb_payload, sizeof(hb_payload));
            uint8_t hb_buf[OPENDASH_ESPNOW_MAX_DATA];
            uint16_t hb_len = 0;
            if (opendash_i2c_serialize(&hb_msg, hb_buf, &hb_len) == OPENDASH_OK) {
                opendash_espnow_broadcast(hb_buf, hb_len);
            }
        }

        cycle++;
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(200));  /* 5 Hz */
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 * Splash Screen
 * ════════════════════════════════════════════════════════════════════════════ */

#if HAS_SPLASH
static void show_splash_screen(void)
{
    ESP_LOGI(TAG, "Displaying splash screen...");

    if (!display_lvgl_lock(1000)) {
        ESP_LOGW(TAG, "Failed to acquire LVGL lock for splash");
        return;
    }

    lv_obj_t *splash_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(splash_scr, lv_color_hex(0x000000), 0);

    lv_obj_t *img = lv_image_create(splash_scr);
    lv_image_set_src(img, &splash_pod1_dsc);
    lv_obj_center(img);

    lv_scr_load(splash_scr);
    display_lvgl_unlock();

    for (int i = 0; i < 20; i++) {
        if (display_lvgl_lock(50)) {
            lv_timer_handler();
            display_lvgl_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "Splash screen complete");
}
#endif

/* ════════════════════════════════════════════════════════════════════════════
 * Main Entry Point
 * ════════════════════════════════════════════════════════════════════════════ */

void app_main(void)
{
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════");
    ESP_LOGI(TAG, "  OpenDash Pod 1 — Display + IMU Unit");
    ESP_LOGI(TAG, "  Version: %s", OPENDASH_VERSION_STR);
    ESP_LOGI(TAG, "  Hardware: Waveshare ESP32-S3-Touch-AMOLED-1.75");
    ESP_LOGI(TAG, "  Display:  466x466 CO5300 QSPI Round AMOLED");
    ESP_LOGI(TAG, "  IMU: QMI8658 (parachute deployment voting)");
    ESP_LOGI(TAG, "  Node: OPENDASH_NODE_POD1 (ESP-NOW)");
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════");

    /* ── Step 1: NVS ────────────────────────────────────────── */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "[1/8] NVS initialized");

    /* Parachute/rollover config (thresholds pushed live from center) */
    opendash_parachute_config_init();

    /* ── Step 2: Config ─────────────────────────────────────── */
    opendash_display_layout_t layout;
    opendash_err_t od_ret = opendash_config_load(OPENDASH_NODE_POD1, &layout);
    if (od_ret != OPENDASH_OK) {
        ESP_LOGW(TAG, "Config load failed, using defaults");
        opendash_config_reset_defaults(OPENDASH_NODE_POD1, &layout);
    }
    ESP_LOGI(TAG, "[2/8] Configuration loaded");

    /* ── Step 3: Display Hardware ───────────────────────────── */
    ESP_LOGI(TAG, "Initializing display (CO5300 QSPI + CST9217 touch)...");
    ESP_ERROR_CHECK(display_init());
    ESP_LOGI(TAG, "[3/8] Display initialized (466x466 AMOLED)");

    /* ── Step 3b: RTC (if present on shared I2C bus) ─────────── */
    i2c_master_bus_handle_t i2c_bus = display_get_i2c_handle();
    if (i2c_bus) {
        esp_err_t rtc_ret = opendash_rtc_init_master(i2c_bus);
        if (rtc_ret == ESP_OK) {
            opendash_rtc_sync_system_clock();
        }
    }

    /* ── Step 4: Splash Screen ──────────────────────────────── */
#if HAS_SPLASH
    show_splash_screen();
#endif
    ESP_LOGI(TAG, "[4/8] Splash screen complete");

    /* ── Step 5: IMU Sensor ─────────────────────────────────── */
    ESP_LOGI(TAG, "Initializing IMU sensor (QMI8658 I2C)...");
    ESP_ERROR_CHECK(imu_handler_init());
    ESP_LOGI(TAG, "[5/8] IMU sensor initialized");

    /* ── Step 6: UI Manager ─────────────────────────────────── */
    ESP_LOGI(TAG, "Initializing UI manager...");
    ESP_ERROR_CHECK(ui_manager_init(&layout));
    ESP_LOGI(TAG, "[6/8] UI manager initialized");

    /* ── Step 7: ESP-NOW Wireless ───────────────────────────── */
    ESP_LOGI(TAG, "Initializing ESP-NOW wireless...");
    ESP_ERROR_CHECK(opendash_espnow_init(OPENDASH_NODE_POD1));
    /* Announce presence immediately so center discovers without waiting for PING */
    {
        uint8_t hb_payload[3] = { OPENDASH_NODE_POD1, 0x01, 0x00 };
        opendash_i2c_msg_t hb_msg;
        opendash_i2c_build_msg(&hb_msg, OPENDASH_CMD_STATUS_REPORT,
                                hb_payload, sizeof(hb_payload));
        uint8_t hb_buf[OPENDASH_ESPNOW_MAX_DATA];
        uint16_t hb_len = 0;
        if (opendash_i2c_serialize(&hb_msg, hb_buf, &hb_len) == OPENDASH_OK) {
            opendash_espnow_broadcast(hb_buf, hb_len);
            ESP_LOGI(TAG, "Initial STATUS_REPORT broadcast sent");
        }
    }
    ESP_LOGI(TAG, "[7/8] ESP-NOW initialized");

    /* ── Step 7b: Audio Alert System ────────────────────────── */
    esp_err_t audio_ret = opendash_audio_init();
    if (audio_ret == ESP_OK) {
        ESP_LOGI(TAG, "  Audio alert system ready");
    } else {
        ESP_LOGW(TAG, "  Audio init skipped (%s) — no speaker?", esp_err_to_name(audio_ret));
    }

    /* ── Step 8: Start All Tasks ────────────────────────────── */
    ESP_LOGI(TAG, "Starting all tasks...");
    imu_handler_start();
    ui_manager_start();

    /* Distributed rollover detector: local roll detection + VOTE broadcast */
    opendash_rollover_detector_start(OPENDASH_NODE_POD1, rollover_read);

    xTaskCreatePinnedToCore(
        data_broadcast_task, "pod1_bcast", 6144, NULL,
        4,    /* Medium priority */
        NULL,
        0     /* Core 0 */
    );

    ESP_LOGI(TAG, "[8/8] All tasks running");

    ESP_LOGI(TAG, "═══════════════════════════════════════════════════");
    ESP_LOGI(TAG, "  System ready!");
    ESP_LOGI(TAG, "  IMU: active (QMI8658 @ 100 Hz)");
    ESP_LOGI(TAG, "  ESP-NOW: broadcasting IMU data at 5 Hz");
    ESP_LOGI(TAG, "  Display: OIL TEMP mode (swipe to cycle)");
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════");

    /* ── Main Loop: Health monitoring ───────────────────────── */
    uint32_t uptime_s = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        uptime_s += 10;

        ESP_LOGI(TAG, "Uptime: %lus | Center: %s | Mode: %d",
                 uptime_s,
                 s_center_mac_known ? "ONLINE" : "searching",
                 ui_manager_get_current_screen());
    }
}
