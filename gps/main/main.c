/**
 * @file main.c
 * @brief OpenDash GPS / Telemetry Unit — Entry Point
 *
 * Hardware: Waveshare ESP32-S3-Touch-AMOLED-1.75
 * Resolution: 466×466 Round AMOLED (CO5300 QSPI)
 * GPS: LC76G GNSS module (I2C)
 * IMU: QMI8658 6-axis accelerometer + gyroscope (I2C)
 * Touch: CST9217 (I2C, managed by BSP)
 * Role: I2C Slave (0x12), GPS/IMU data provider
 *
 * Initialization Order (critical):
 *   1. NVS
 *   2. Config load
 *   3. Display init (calls bsp_i2c_init — sets up shared I2C bus)
 *   4. GPS init (needs BSP I2C bus handle)
 *   5. IMU init (needs BSP I2C bus handle)
 *   6. UI init (needs LVGL from display)
 *   7. I2C node init (separate bus for inter-display comms)
 *   8. Start all tasks
 *
 * @see Waveshare BSP: waveshare/esp32_s3_touch_amoled_1_75
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "display_init.h"
#include "ui_manager.h"
#include "gps_handler.h"
#include "imu_handler.h"
#include "i2c_node.h"
#include "opendash_common.h"
#include "opendash_display_config.h"

static const char *TAG = "opendash_gps";

/**
 * @brief Main application entry point.
 */
void app_main(void)
{
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════");
    ESP_LOGI(TAG, "  OpenDash GPS / Telemetry Unit");
    ESP_LOGI(TAG, "  Version: %s", OPENDASH_VERSION_STR);
    ESP_LOGI(TAG, "  Hardware: Waveshare ESP32-S3-Touch-AMOLED-1.75");
    ESP_LOGI(TAG, "  Display:  466x466 CO5300 QSPI Round AMOLED");
    ESP_LOGI(TAG, "  I2C Addr: 0x12 (OpenDash GPS node)");
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════");

    /* ────────────────────────────────────────────────────────────────────────
     * Step 1: Initialize NVS
     * ──────────────────────────────────────────────────────────────────────── */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "[1/8] NVS initialized");

    /* ────────────────────────────────────────────────────────────────────────
     * Step 2: Load Display Configuration
     * ──────────────────────────────────────────────────────────────────────── */
    opendash_display_layout_t layout;
    opendash_err_t od_ret = opendash_config_load(OPENDASH_NODE_GPS, &layout);
    if (od_ret != OPENDASH_OK) {
        ESP_LOGW(TAG, "Failed to load config from NVS, using defaults");
        opendash_config_reset_defaults(OPENDASH_NODE_GPS, &layout);
    }
    ESP_LOGI(TAG, "[2/8] Configuration loaded");

    /* ────────────────────────────────────────────────────────────────────────
     * Step 3: Initialize Display Hardware (MUST be before GPS/IMU!)
     *
     * display_init() calls bsp_i2c_init() which sets up the shared I2C
     * bus (SDA=15, SCL=14). GPS and IMU modules obtain their I2C handles
     * from this bus via bsp_i2c_get_handle().
     * ──────────────────────────────────────────────────────────────────────── */
    ESP_LOGI(TAG, "Initializing display (BSP + I2C bus)...");
    ESP_ERROR_CHECK(display_init());
    ESP_LOGI(TAG, "[3/8] Display initialized (CO5300 QSPI + CST9217 touch)");

    /* ────────────────────────────────────────────────────────────────────────
     * Step 4: Initialize GPS Module (LC76G via I2C)
     * ──────────────────────────────────────────────────────────────────────── */
    ESP_LOGI(TAG, "Initializing GPS module (LC76G I2C)...");
    ESP_ERROR_CHECK(gps_handler_init());
    ESP_LOGI(TAG, "[4/8] GPS module initialized");

    /* ────────────────────────────────────────────────────────────────────────
     * Step 5: Initialize IMU Sensor (QMI8658 via I2C)
     * ──────────────────────────────────────────────────────────────────────── */
    ESP_LOGI(TAG, "Initializing IMU sensor (QMI8658 I2C)...");
    ESP_ERROR_CHECK(imu_handler_init());
    ESP_LOGI(TAG, "[5/8] IMU sensor initialized");

    /* ────────────────────────────────────────────────────────────────────────
     * Step 6: Initialize LVGL UI Manager
     * ──────────────────────────────────────────────────────────────────────── */
    ESP_LOGI(TAG, "Initializing UI manager (3-mode display)...");
    ESP_ERROR_CHECK(ui_manager_init(&layout));
    ESP_LOGI(TAG, "[6/8] UI manager initialized (GPS / LAP / GFORCE modes)");

    /* ────────────────────────────────────────────────────────────────────────
     * Step 7: Initialize I2C Slave Node (OpenDash inter-display bus)
     * ──────────────────────────────────────────────────────────────────────── */
    ESP_LOGI(TAG, "Initializing I2C slave node (addr 0x12)...");
    bool i2c_node_ok = (i2c_node_init() == ESP_OK);
    if (i2c_node_ok) {
        ESP_LOGI(TAG, "[7/8] I2C slave node initialized");
    } else {
        ESP_LOGW(TAG, "[7/8] I2C slave node FAILED — inter-node comms disabled");
    }

    /* ────────────────────────────────────────────────────────────────────────
     * Step 8: Start All Tasks
     * ──────────────────────────────────────────────────────────────────────── */
    ESP_LOGI(TAG, "Starting all tasks...");
    gps_handler_start();
    imu_handler_start();
    ui_manager_start();
    if (i2c_node_ok) {
        i2c_node_start();
    }
    ESP_LOGI(TAG, "[8/8] All tasks running");

    ESP_LOGI(TAG, "═══════════════════════════════════════════════════");
    ESP_LOGI(TAG, "  System ready!");
    ESP_LOGI(TAG, "  GPS: acquiring satellites (LC76G I2C)");
    ESP_LOGI(TAG, "  IMU: active (QMI8658 @ 100 Hz)");
    ESP_LOGI(TAG, "  Display: GPS mode (boot button to cycle)");
    ESP_LOGI(TAG, "  I2C node: listening on 0x12");
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════");

    /* ────────────────────────────────────────────────────────────────────────
     * Main Loop — Watchdog / health monitoring
     * ──────────────────────────────────────────────────────────────────────── */
    uint32_t uptime_s = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));   /* 10-second heartbeat */
        uptime_s += 10;

        /* Periodic health log */
        gps_data_t gps = {0};
        gps_handler_get_data(&gps);
        ESP_LOGI(TAG, "Uptime: %lus | GPS fix: %s | Sats: %d | Mode: %d",
                 uptime_s,
                 gps.fix_valid ? "YES" : "NO",
                 gps.satellites,
                 ui_manager_get_current_screen());
    }
}
