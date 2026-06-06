/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file main.c
 * @brief OpenDash Right Gauge — Entry Point
 *
 * Hardware: Waveshare ESP32-S3-Touch-LCD-2.8C
 * Resolution: 480×480 Round IPS LCD
 * Role: ESP-NOW Receiver — receives display data from Center unit wirelessly
 *
 * Communication Architecture:
 *   Port 0 (I2C Master): SDA=15, SCL=7 — TCA9554 IO expander + GT911 touch
 *                        (initialized by display_init, on-board use only)
 *   ESP-NOW (WiFi):      Wireless peer-to-peer with Center unit
 *                        (no wires, no GPIO conflicts)
 *
 * Startup sequence:
 *   1. NVS init
 *   2. Odometer init (loads persisted mileage from NVS)
 *   3. Load display configuration
 *   4. Initialize display hardware (I2C bus + TCA9554 + ST7701S + RGB + LVGL)
 *   5. Initialize ESP-NOW wireless transport
 *   6. Build UI
 *   7. Start LVGL rendering task + boot button task
 *   8. Main loop: ESP-NOW message processing + odometer accumulation
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"

/* ESP-NOW wireless transport for inter-node communication with Center.
 * No wires needed — uses WiFi peer-to-peer (ESP-NOW).
 * Completely independent from the on-board I2C bus (TCA9554/GT911). */
#define ENABLE_ESPNOW  1

#include "display_init.h"
#include "splash_right.h"
#include "ui_manager.h"
#include "imu_handler.h"
#include "opendash_common.h"
#include "opendash_display_config.h"
#include "opendash_i2c_protocol.h"
#include "opendash_data_model.h"
#include "opendash_odometer.h"
#include "opendash_parachute.h"
#include "opendash_rollover.h"
#include "opendash_rtc.h"
#if ENABLE_ESPNOW
#include "opendash_espnow.h"
#include "opendash_bt_ota.h"
#include "esp_mac.h"
#endif

static const char *TAG = "opendash_right";

/* ────────────────────────────────────────────────────────────────────────────
 * Odometer state — accumulates distance from GPS speed
 * ──────────────────────────────────────────────────────────────────────────── */
static opendash_odometer_t s_odometer;
static float               s_last_gps_speed_kmh  = 0.0f;   /* latest GPS speed */
static int64_t             s_last_odo_time_us     = 0;      /* last sample time */

/* ────────────────────────────────────────────────────────────────────────────
 * ESP-NOW Wireless Receiver
 *
 * Receives messages from the Center master via ESP-NOW (WiFi peer-to-peer).
 * Processes the same OpenDash protocol messages as before, but over the air
 * instead of over I2C wires.
 * ──────────────────────────────────────────────────────────────────────────── */
#if ENABLE_ESPNOW

/** @brief MAC address of the Center master (learned from first received message). */
static uint8_t  s_center_mac[6] = {0};
static bool     s_center_mac_known = false;

/**
 * @brief Send a protocol response back to the Center master.
 */
static esp_err_t send_response_to_center(const opendash_i2c_msg_t *resp)
{
    if (!s_center_mac_known) return ESP_ERR_NOT_FOUND;

    uint8_t buf[OPENDASH_ESPNOW_MAX_DATA];
    uint16_t len;
    if (opendash_i2c_serialize(resp, buf, &len) != OPENDASH_OK) {
        return ESP_FAIL;
    }

    return opendash_espnow_send(s_center_mac, buf, len);
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
 * @brief Process a single received protocol message.
 *
 * Supported commands:
 *   - SET_DATA_POINT: Update a gauge value (also captures GPS speed for odo)
 *   - SET_BRIGHTNESS: Adjust backlight brightness
 *   - SET_ALARM:      Trigger or clear warning overlay
 *   - SYSTEM:         PING response, REBOOT, FACTORY_RESET
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
        /* ── Data Point Update ─────────────────────────────────── */
        case OPENDASH_CMD_SET_DATA_POINT: {
            if (msg->length >= 6) {
                uint16_t dp_id = (msg->payload[0] << 8) | msg->payload[1];
                float value;
                memcpy(&value, &msg->payload[2], sizeof(float));

                /* Capture GPS speed for odometer distance calculation */
                if (dp_id == OPENDASH_DP_GPS_SPEED) {
                    s_last_gps_speed_kmh = value;
                }

                if (display_lvgl_lock(10)) {
                    ui_manager_update_value(dp_id, value);
                    display_lvgl_unlock();
                }
                ESP_LOGD(TAG, "SET_DATA dp=0x%04X val=%.2f", dp_id, value);
            }
            break;
        }

        /* ── Brightness ────────────────────────────────────────── */
        case OPENDASH_CMD_SET_BRIGHTNESS: {
            if (msg->length >= 1) {
                display_set_brightness(msg->payload[0]);
            }
            break;
        }

        /* ── Alarm / Warning ───────────────────────────────────── */
        case OPENDASH_CMD_SET_ALARM: {
            if (msg->length >= 11) {
                uint8_t  flags = msg->payload[10];
                bool     active   = (flags & 0x01) != 0;
                bool     critical = (flags & 0x02) != 0;

                if (display_lvgl_lock(10)) {
                    if (active) {
                        opendash_warning_level_t level = critical
                            ? OPENDASH_WARNING_CRITICAL
                            : OPENDASH_WARNING_CAUTION;
                        ui_manager_warning_trigger(level, "WARNING", 3000);
                    } else {
                        ui_manager_warning_clear();
                    }
                    display_lvgl_unlock();
                }
            }
            break;
        }

        /* ── System Commands ───────────────────────────────────── */
        case OPENDASH_CMD_SYSTEM: {
            if (msg->length < 1) break;

            switch (msg->payload[0]) {
                case OPENDASH_SUBCMD_PING: {
                    /* Respond with STATUS_REPORT so center discovers us */
                    uint8_t status_payload[3] = {
                        OPENDASH_NODE_RIGHT,  /* Node ID */
                        0x01, 0x00            /* Flags: running, no errors */
                    };
                    opendash_i2c_msg_t resp;
                    opendash_i2c_build_msg(&resp, OPENDASH_CMD_STATUS_REPORT,
                                           status_payload, sizeof(status_payload));
                    send_response_to_center(&resp);
                    ESP_LOGD(TAG, "PING → STATUS_REPORT sent");
                    break;
                }
                case OPENDASH_SUBCMD_REBOOT:
                    ESP_LOGW(TAG, "Reboot requested via ESP-NOW");
                    opendash_odometer_save_now(&s_odometer);
                    esp_restart();
                    break;

                case OPENDASH_SUBCMD_FACTORY_RESET: {
                    ESP_LOGW(TAG, "Factory reset requested via ESP-NOW");
                    opendash_display_layout_t layout;
                    opendash_config_reset_defaults(OPENDASH_NODE_RIGHT, &layout);
                    opendash_config_save(&layout);
                    opendash_odometer_save_now(&s_odometer);
                    esp_restart();
                    break;
                }
                case OPENDASH_SUBCMD_ENTER_BT_OTA: {
                    ESP_LOGW(TAG, "Entering BLE OTA mode");
                    opendash_odometer_save_now(&s_odometer);
                    if (display_lvgl_lock(200)) {
                        lv_obj_t *ota_scr = lv_obj_create(NULL);
                        lv_obj_set_style_bg_color(ota_scr, lv_color_hex(0x000000), 0);
                        lv_obj_set_style_bg_opa(ota_scr, LV_OPA_COVER, 0);
                        lv_obj_t *t1 = lv_label_create(ota_scr);
                        lv_label_set_text(t1, "WAITING FOR OTA");
                        lv_obj_set_style_text_color(t1, lv_color_hex(0x00CCFF), 0);
                        lv_obj_align(t1, LV_ALIGN_CENTER, 0, -30);
                        lv_obj_t *t2 = lv_label_create(ota_scr);
                        lv_label_set_text(t2, "RIGHT");
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
                    /* Quiesce display + UI so RGB DMA / LVGL stop competing
                     * with NimBLE for CPU and memory bandwidth during OTA. */
                    ui_manager_suspend();
                    display_pause_for_ota();
                    opendash_bt_ota_enter(OPENDASH_NODE_RIGHT,
                                          s_center_mac_known ? s_center_mac : NULL);
                    break;
                }
                default:
                    ESP_LOGD(TAG, "Unknown system subcmd: 0x%02X",
                             msg->payload[0]);
                    break;
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

        default:
            ESP_LOGD(TAG, "Unhandled cmd: 0x%02X", msg->cmd);
            break;
    }
}

/**
 * @brief Drain the ESP-NOW receive queue and process all pending messages.
 *
 * Called from the main loop (~50 ms cadence).
 */
static void process_espnow_messages(void)
{
    opendash_espnow_event_t evt;

    int processed = 0;
    while (opendash_espnow_recv(&evt, 0 /* non-blocking */)) {
        /* Deserialize the protocol message */
        opendash_i2c_msg_t msg;
        opendash_err_t ret = opendash_i2c_deserialize(evt.data, evt.len, &msg);
        if (ret != OPENDASH_OK) {
            ESP_LOGD(TAG, "Invalid ESP-NOW msg from " MACSTR " (len=%d)",
                     MAC2STR(evt.src_mac), evt.len);
            continue;
        }

        dispatch_message(&evt, &msg);

        /* Yield every 4 messages to let IDLE task reset watchdog on CPU 0 */
        if (++processed >= 4) {
            vTaskDelay(1);
            processed = 0;
        }
    }
}
#endif /* ENABLE_ESPNOW */

/**
 * @brief Odometer reset callback — triggered by 5-second boot button hold.
 *
 * Clears total + trip A + trip B and saves to NVS immediately.
 */
static void odometer_reset_callback(void)
{
    ESP_LOGW(TAG, "=== ODOMETER RESET (boot button long-press 5s) ===");
    s_odometer.total_meters = 0;
    s_odometer.trip_a_meters = 0;
    s_odometer.trip_b_meters = 0;
    s_odometer.last_nvs_save_meters = 0;
    opendash_odometer_save_now(&s_odometer);
    ESP_LOGW(TAG, "Odometer cleared and saved to NVS");

    /* Update UI immediately */
    if (display_lvgl_lock(100)) {
        ui_manager_update_odometer(&s_odometer);
        display_lvgl_unlock();
    }
}

/**
 * @brief Update the odometer based on the latest GPS speed.
 *
 * Called every main-loop tick (~50 ms).  Calculates distance traveled since
 * the last sample using:  distance_m = speed_kmh * (dt_s / 3.6)
 *
 * The odometer module handles NVS persistence automatically (every 100 m).
 */
static void odometer_accumulate(void)
{
    int64_t now_us = esp_timer_get_time();

    if (s_last_odo_time_us == 0) {
        /* First call — just record the timestamp */
        s_last_odo_time_us = now_us;
        return;
    }

    float dt_s = (float)(now_us - s_last_odo_time_us) / 1e6f;
    s_last_odo_time_us = now_us;

    /* Only accumulate if speed is sane (> 1 km/h, < 400 km/h) */
    if (s_last_gps_speed_kmh > 1.0f && s_last_gps_speed_kmh < 400.0f) {
        float distance_m = s_last_gps_speed_kmh * dt_s / 3.6f;
        opendash_odometer_add_distance(&s_odometer, (uint32_t)distance_m);
    }
}

/* (ESP-NOW message processing is above — no I2C slave code needed) */

/* ────────────────────────────────────────────────────────────────────────────
 * Splash Screen
 * ──────────────────────────────────────────────────────────────────────────── */
static void show_splash_screen(void)
{
    ESP_LOGI(TAG, "Displaying splash screen...");
    lv_obj_t *splash = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(splash, lv_color_hex(0x000000), 0);

    lv_obj_t *img = lv_image_create(splash);
    lv_image_set_src(img, &splash_right_dsc);
    lv_obj_center(img);
    lv_scr_load(splash);

    /* Pump LVGL for ~2 seconds so splash is visible */
    for (int i = 0; i < 200; i++) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGI(TAG, "Splash screen done");
}

/* ────────────────────────────────────────────────────────────────────────────
 * Application Entry Point
 * ──────────────────────────────────────────────────────────────────────────── */

void app_main(void)
{
    ESP_LOGI(TAG, "══════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "OpenDash Right Gauge v%s", OPENDASH_VERSION_STR);
    ESP_LOGI(TAG, "Hardware: ESP32-S3-Touch-LCD-2.8C (480×480 Round)");
    ESP_LOGI(TAG, "══════════════════════════════════════════════════════");

    /* Step 1: NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "[1/7] NVS initialized");

    /* Parachute/rollover config (thresholds pushed live from center) */
    opendash_parachute_config_init();

    /* Step 2: Odometer — must be initialized after NVS */
    opendash_odometer_init(&s_odometer);
    ESP_LOGI(TAG, "[2/7] Odometer loaded (%.1f km, trip=%.1f km)",
             opendash_odometer_get_km(&s_odometer),
             opendash_odometer_get_trip_a_km(&s_odometer));

    /* Step 3: Load display configuration */
    opendash_display_layout_t layout;
    opendash_err_t od_ret = opendash_config_load(OPENDASH_NODE_RIGHT, &layout);
    if (od_ret != OPENDASH_OK) {
        ESP_LOGW(TAG, "Config not in NVS — using defaults");
        opendash_config_reset_defaults(OPENDASH_NODE_RIGHT, &layout);
    }
    ESP_LOGI(TAG, "[3/7] Display config loaded (%d sections, brightness=%d)",
             layout.num_sections, layout.brightness);

    /* Step 4: Display hardware
     * (I2C master bus + TCA9554 + LCD reset + SPI init + ST7701S regs +
     *  RGB panel + backlight + GT911 probe + LVGL) */
    ESP_LOGI(TAG, "[4/7] Initializing display...");
    ESP_ERROR_CHECK(display_init());
    display_set_brightness(layout.brightness * 100 / 255);

    /* Step 4a: RTC — sync system clock from PCF85063 before GPS fix */
    if (opendash_rtc_init_master(display_get_i2c_handle()) == ESP_OK) {
        opendash_rtc_sync_system_clock();
    }

    /* Step 4b: Splash screen — show for ~2 seconds before UI builds */
    show_splash_screen();

    /* Step 5: ESP-NOW wireless transport for Center unit communication.
     * No wires needed — uses WiFi peer-to-peer (ESP-NOW).
     * Completely independent from on-board I2C bus (TCA9554/GT911). */
#if ENABLE_ESPNOW
    ESP_LOGI(TAG, "[5/7] Initializing ESP-NOW wireless transport...");
    ESP_ERROR_CHECK(opendash_espnow_init(OPENDASH_NODE_RIGHT));
#else
    ESP_LOGI(TAG, "[5/7] ESP-NOW DISABLED");
#endif

    /* Step 6: Build UI */
    ESP_LOGI(TAG, "[6/7] Building UI...");
    if (display_lvgl_lock(1000)) {
        ESP_ERROR_CHECK(ui_manager_init(&layout));
        display_lvgl_unlock();
    }

    /* Step 7: Start UI rendering task + boot button */
    ESP_LOGI(TAG, "[7/7] Starting UI and button tasks...");
    ESP_ERROR_CHECK(ui_manager_start());
    start_button_task();
    display_register_long_press_cb(odometer_reset_callback);

    /* Step 7a: IMU + distributed rollover detector. The 2.8C board carries a
     * QMI8658 on the shared display I2C bus. If it is absent the driver soft-
     * fails and the gauge keeps running without contributing rollover votes. */
    if (imu_handler_init() == ESP_OK && imu_handler_start() == ESP_OK) {
        opendash_rollover_detector_start(OPENDASH_NODE_RIGHT, rollover_read);
        ESP_LOGI(TAG, "Rollover detector active (QMI8658 present)");
    } else {
        ESP_LOGW(TAG, "IMU not present — rollover detection disabled on this node");
    }

    ESP_LOGI(TAG, "══════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "Init complete — system running");
    ESP_LOGI(TAG, "══════════════════════════════════════════════════════");

    /* Boot beep: 2 short beeps = clean boot */
    display_buzzer_boot_ok();

    /* ── Main Loop: Process ESP-NOW messages + accumulate odometer ─── */
    uint32_t odo_update_counter = 0;
    uint32_t io_refresh_counter = 0;

    while (1) {
#if ENABLE_ESPNOW
        process_espnow_messages();
#endif

        /* Accumulate odometer distance from GPS speed (every tick) */
        odometer_accumulate();

        /* Push odometer values to UI once per second (20 × 50ms = 1000ms).
         * More frequent updates waste CPU on LVGL invalidation and can
         * starve the IDLE task on CPU 0 → watchdog timeout.              */
        if (++odo_update_counter >= 20) {
            odo_update_counter = 0;
            if (display_lvgl_lock(10)) {
                ui_manager_update_odometer(&s_odometer);
                display_lvgl_unlock();
            }
        }

        /* Re-confirm TCA9554 IO expander outputs every 5 seconds
         * (100 × 50ms) to guard against I2C bus glitch corrupting
         * the output register (could reset LCD via EXIO1).           */
        if (++io_refresh_counter >= 100) {
            io_refresh_counter = 0;
            display_refresh_io_state();
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
