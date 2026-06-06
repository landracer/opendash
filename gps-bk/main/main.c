/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file main.c
 * @brief OpenDash GPS / Telemetry Unit — Entry Point
 *
 * Hardware: Waveshare ESP32-S3-Touch-AMOLED-1.75
 * Resolution: 466×466 Round AMOLED (CO5300 QSPI)
 * GPS: LC76G GNSS module (I2C)
 * IMU: QMI8658 6-axis accelerometer + gyroscope (I2C)
 * Touch: CST9217 (I2C, managed by display_init)
 * Role: ESP-NOW node (OPENDASH_NODE_GPS), GPS/IMU data provider
 *
 * Architecture:
 *   - GPS reads sensor data locally (LC76G + QMI8658 on shared I2C bus)
 *   - Proactively broadcasts all GPS/IMU data to center via ESP-NOW
 *   - Responds to center PINGs with STATUS_REPORT
 *   - Receives SET_DATA_POINT from center (engine data for display)
 *   - Broadcasts TIME_SYNC to all nodes when GPS has valid fix
 *
 * Initialization Order (critical):
 *   1. NVS
 *   2. Config load
 *   3. Display init (I2C bus, CO5300, CST9217, LVGL)
 *   4. Splash screen
 *   5. GPS init (LC76G via shared I2C)
 *   6. IMU init (QMI8658 via shared I2C)
 *   7. UI init
 *   8. ESP-NOW init
 *   9. Start all tasks
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
#include "gps_handler.h"
#include "imu_handler.h"
#include "opendash_common.h"
#include "opendash_display_config.h"
#include "opendash_espnow.h"
#include "opendash_i2c_protocol.h"
#include "opendash_data_model.h"
#include "opendash_rtc.h"
#include "sd_logger.h"
#include "esp_task_wdt.h"

/* Splash screen image */
#if __has_include("splash_gps.h")
#include "splash_gps.h"
#define HAS_SPLASH 1
#else
#define HAS_SPLASH 0
#endif

static const char *TAG = "opendash_gps";

/* ════════════════════════════════════════════════════════════════════════════
 * ESP-NOW Communication
 * ════════════════════════════════════════════════════════════════════════════ */

static uint8_t s_center_mac[6];
static bool    s_center_mac_known = false;
static uint8_t s_tx_buf[OPENDASH_ESPNOW_MAX_DATA];

/**
 * @brief Send a protocol response to center (unicast if MAC known).
 */
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

/**
 * @brief Send a single data point value to center.
 */
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

/**
 * @brief Process a single received ESP-NOW message.
 */
static void dispatch_message(const opendash_espnow_event_t *evt,
                              const opendash_i2c_msg_t *msg)
{
    /* Learn center's MAC from first valid message */
    if (!s_center_mac_known) {
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
                case OPENDASH_SUBCMD_PING: {
                    uint8_t status_payload[3] = {
                        OPENDASH_NODE_GPS,
                        0x01, 0x00
                    };
                    opendash_i2c_msg_t resp;
                    opendash_i2c_build_msg(&resp, OPENDASH_CMD_STATUS_REPORT,
                                           status_payload, sizeof(status_payload));
                    send_to_center(&resp);
                    ESP_LOGD(TAG, "PING → STATUS_REPORT sent");
                    break;
                }
                case OPENDASH_SUBCMD_REBOOT:
                    ESP_LOGW(TAG, "Reboot requested via ESP-NOW");
                    esp_restart();
                    break;
                default:
                    ESP_LOGD(TAG, "Unknown system subcmd: 0x%02X", msg->payload[0]);
                    break;
            }
            break;
        }

        /* ── Data Request from Center ──────────────────────── */
        case OPENDASH_CMD_REQUEST_DATA: {
            if (msg->length >= 2) {
                uint16_t dp_id = (msg->payload[0] << 8) | msg->payload[1];
                gps_data_t gps = {0};
                imu_data_t imu = {0};

                float value = 0.0f;
                bool found = true;

                switch (dp_id) {
                    case OPENDASH_DP_GPS_SPEED:    gps_handler_get_data(&gps); value = gps.speed;            break;
                    case OPENDASH_DP_GPS_HEADING:  gps_handler_get_data(&gps); value = gps.heading;          break;
                    case OPENDASH_DP_LATITUDE:     gps_handler_get_data(&gps); value = (float)gps.latitude;  break;
                    case OPENDASH_DP_LONGITUDE:    gps_handler_get_data(&gps); value = (float)gps.longitude; break;
                    case OPENDASH_DP_ALTITUDE:     gps_handler_get_data(&gps); value = gps.altitude;         break;
                    case OPENDASH_DP_SAT_COUNT:    gps_handler_get_data(&gps); value = (float)gps.satellites; break;
                    case OPENDASH_DP_HDOP:         gps_handler_get_data(&gps); value = gps.hdop;             break;
                    case OPENDASH_DP_GFORCE_LAT:   imu_handler_get_data(&imu); value = imu.g_lateral;        break;
                    case OPENDASH_DP_GFORCE_LONG:  imu_handler_get_data(&imu); value = imu.g_longitudinal;   break;
                    case OPENDASH_DP_GFORCE_VERT:  imu_handler_get_data(&imu); value = imu.g_vertical;       break;
                    default: found = false; break;
                }
                if (found) {
                    send_data_point(dp_id, value);
                }
            }
            break;
        }

        default:
            ESP_LOGD(TAG, "Unhandled cmd: 0x%02X", msg->cmd);
            break;
    }
}

/**
 * @brief Drain ESP-NOW receive queue.
 */
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
 * GPS/IMU Data Broadcasting Task
 *
 * Proactively pushes sensor data to center at 5 Hz.
 * Center then forwards it to all gauge pods + displays it locally.
 * ════════════════════════════════════════════════════════════════════════════ */

static void data_broadcast_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Data broadcast task started (5 Hz)");
    esp_task_wdt_add(NULL);
    TickType_t last_wake = xTaskGetTickCount();
    uint32_t cycle = 0;

    while (1) {
        esp_task_wdt_reset();

        /* ── Process incoming ESP-NOW messages ──── */
        process_espnow_messages();

        /* ── Broadcast GPS data ──────────────────── */
        gps_data_t gps = {0};
        gps_handler_get_data(&gps);

        send_data_point(OPENDASH_DP_GPS_SPEED,   gps.speed);
        send_data_point(OPENDASH_DP_GPS_HEADING,  gps.heading);
        send_data_point(OPENDASH_DP_LATITUDE,     (float)gps.latitude);
        send_data_point(OPENDASH_DP_LONGITUDE,    (float)gps.longitude);
        vTaskDelay(1);  /* yield between bursts */
        esp_task_wdt_reset();
        send_data_point(OPENDASH_DP_ALTITUDE,     gps.altitude);
        send_data_point(OPENDASH_DP_SAT_COUNT,    (float)gps.satellites);
        send_data_point(OPENDASH_DP_HDOP,         gps.hdop);
        send_data_point(OPENDASH_DP_GPS_FIX,      gps.fix_valid ? 1.0f : 0.0f);

        vTaskDelay(pdMS_TO_TICKS(5));  /* Brief yield between GPS and IMU */

        /* ── Broadcast IMU data ──────────────────── */
        imu_data_t imu = {0};
        imu_handler_get_data(&imu);

        send_data_point(OPENDASH_DP_GFORCE_LAT,   imu.g_lateral);
        send_data_point(OPENDASH_DP_GFORCE_LONG,  imu.g_longitudinal);
        send_data_point(OPENDASH_DP_GFORCE_VERT,  imu.g_vertical);
        esp_task_wdt_reset();

        /* ── SD card logging (5 Hz snapshot) ─────── */
        sd_logger_log_snapshot(gps.speed, gps.heading,
                               gps.latitude, gps.longitude,
                               gps.altitude, gps.satellites, gps.hdop,
                               gps.fix_valid,
                               imu.g_lateral, imu.g_longitudinal,
                               imu.g_vertical);

        /* ── Time Sync broadcast (every ~2 seconds, when fix valid) ── */
        if ((cycle % 10) == 0 && gps.fix_valid) {
            uint8_t time_payload[9];
            time_payload[0] = OPENDASH_SUBCMD_TIME_SYNC;
            time_payload[1] = gps.hour;
            time_payload[2] = gps.minute;
            time_payload[3] = gps.second;
            time_payload[4] = 0;  /* day - not parsed from NMEA yet */
            time_payload[5] = 0;  /* month */
            time_payload[6] = 0;  /* year_lo */
            time_payload[7] = 0;  /* year_hi */
            time_payload[8] = gps.fix_valid ? 1 : 0;

            opendash_i2c_msg_t time_msg;
            opendash_i2c_build_msg(&time_msg, OPENDASH_CMD_SYSTEM,
                                   time_payload, sizeof(time_payload));

            uint16_t tslen;
            if (opendash_i2c_serialize(&time_msg, s_tx_buf, &tslen) == OPENDASH_OK) {
                opendash_espnow_broadcast(s_tx_buf, tslen);
            }
            ESP_LOGD(TAG, "TIME_SYNC broadcast: %02d:%02d:%02d UTC",
                     gps.hour, gps.minute, gps.second);
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
    lv_image_set_src(img, &splash_gps_dsc);
    lv_obj_center(img);

    lv_scr_load(splash_scr);
    display_lvgl_unlock();

    /* Show splash for 2 seconds */
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
    ESP_LOGI(TAG, "  OpenDash GPS / Telemetry Unit");
    ESP_LOGI(TAG, "  Version: %s", OPENDASH_VERSION_STR);
    ESP_LOGI(TAG, "  Hardware: Waveshare ESP32-S3-Touch-AMOLED-1.75");
    ESP_LOGI(TAG, "  Display:  466x466 CO5300 QSPI Round AMOLED");
    ESP_LOGI(TAG, "  Node: OPENDASH_NODE_GPS (ESP-NOW)");
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════");

    /* ── Step 1: NVS ────────────────────────────────────────── */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "[1/9] NVS initialized");

    /* ── Step 2: Config ─────────────────────────────────────── */
    opendash_display_layout_t layout;
    opendash_err_t od_ret = opendash_config_load(OPENDASH_NODE_GPS, &layout);
    if (od_ret != OPENDASH_OK) {
        ESP_LOGW(TAG, "Config load failed, using defaults");
        opendash_config_reset_defaults(OPENDASH_NODE_GPS, &layout);
    }
    ESP_LOGI(TAG, "[2/9] Configuration loaded");

    /* ── Step 3: Display Hardware ───────────────────────────── */
    ESP_LOGI(TAG, "Initializing display (CO5300 QSPI + CST9217 touch)...");
    ESP_ERROR_CHECK(display_init());
    ESP_LOGI(TAG, "[3/9] Display initialized (466x466 AMOLED)");

    /* ── Step 3b: RTC (PCF85063 on shared I2C bus) ──────────── */
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
    ESP_LOGI(TAG, "[4/9] Splash screen complete");

    /* ── Step 5: GPS Module ─────────────────────────────────── */
    ESP_LOGI(TAG, "Initializing GPS module (LC76G I2C)...");
    ESP_ERROR_CHECK(gps_handler_init());
    ESP_LOGI(TAG, "[5/9] GPS module initialized");

    /* ── Step 6: IMU Sensor ─────────────────────────────────── */
    ESP_LOGI(TAG, "Initializing IMU sensor (QMI8658 I2C)...");
    ESP_ERROR_CHECK(imu_handler_init());
    ESP_LOGI(TAG, "[6/9] IMU sensor initialized");

    /* ── Step 6b: SD Card Logger ────────────────────────────── */
    if (sd_logger_init() == ESP_OK) {
        sd_logger_start();
        ESP_LOGI(TAG, "[6b/9] SD card logger active");
    } else {
        ESP_LOGW(TAG, "[6b/9] SD card not available — logging disabled");
    }

    /* ── Step 7: UI Manager ─────────────────────────────────── */
    ESP_LOGI(TAG, "Initializing UI manager...");
    ESP_ERROR_CHECK(ui_manager_init(&layout));
    ESP_LOGI(TAG, "[7/9] UI manager initialized (GPS / LAP / GFORCE modes)");

    /* ── Step 8: ESP-NOW Wireless ───────────────────────────── */
    ESP_LOGI(TAG, "Initializing ESP-NOW wireless...");
    ESP_ERROR_CHECK(opendash_espnow_init(OPENDASH_NODE_GPS));
    ESP_LOGI(TAG, "[8/9] ESP-NOW initialized");

    /* ── Step 9: Start All Tasks ────────────────────────────── */
    ESP_LOGI(TAG, "Starting all tasks...");
    gps_handler_start();
    imu_handler_start();
    ui_manager_start();

    /* Data broadcast task: reads sensors + handles ESP-NOW messages */
    xTaskCreatePinnedToCore(
        data_broadcast_task, "gps_broadcast", 6144, NULL,
        4,    /* Medium priority */
        NULL,
        0     /* Core 0 */
    );

    ESP_LOGI(TAG, "[9/9] All tasks running");

    ESP_LOGI(TAG, "═══════════════════════════════════════════════════");
    ESP_LOGI(TAG, "  System ready!");
    ESP_LOGI(TAG, "  GPS: acquiring satellites (LC76G I2C)");
    ESP_LOGI(TAG, "  IMU: active (QMI8658 @ 100 Hz)");
    ESP_LOGI(TAG, "  ESP-NOW: broadcasting GPS/IMU data at 5 Hz");
    ESP_LOGI(TAG, "  Display: GPS mode (boot button to cycle)");
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════");

    /* ── Main Loop: Health monitoring ───────────────────────── */
    uint32_t uptime_s = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        uptime_s += 10;

        gps_data_t gps = {0};
        gps_handler_get_data(&gps);
        ESP_LOGI(TAG, "Uptime: %lus | GPS: %s | Sats: %d | Center: %s | Mode: %d",
                 uptime_s,
                 gps.fix_valid ? "3D FIX" : "NO FIX",
                 gps.satellites,
                 s_center_mac_known ? "ONLINE" : "searching",
                 ui_manager_get_current_screen());
    }
}
