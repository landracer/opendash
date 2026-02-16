/**
 * @file main.c
 * @brief OpenDash Center Display — Entry Point
 *
 * Hardware: Waveshare ESP32-S3-Touch-LCD-4.3
 * Resolution: 800×480 IPS LCD
 * Touch: Capacitive touch controller
 * Role: I2C Master, main display coordinator
 *
 * This is the main entry point for the Center Display unit. It initializes:
 * - ESP-IDF system services
 * - Display hardware (LCD + touch)
 * - LVGL graphics library
 * - I2C master for communication with other nodes
 * - OBD2/CAN interface (if available)
 * - WiFi/BLE (optional, for OTA updates)
 *
 * @see ESP-IDF API Reference:
 *      https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32s3/api-reference/index.html
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
#include "opendash_common.h"
#include "opendash_display_config.h"

static const char *TAG = "opendash_center";

/**
 * @brief Main application entry point.
 *
 * Performs system initialization in the following order:
 * 1. Initialize NVS (Non-Volatile Storage) for configuration
 * 2. Load display configuration from NVS
 * 3. Initialize display hardware (LCD + touch)
 * 4. Initialize LVGL and UI manager
 * 5. Start FreeRTOS tasks for UI rendering and data handling
 *
 * @note This function is called by the ESP-IDF startup code after the
 *       bootloader and system initialization are complete.
 */
void app_main(void)
{
    ESP_LOGI(TAG, "OpenDash Center Display Starting...");
    ESP_LOGI(TAG, "Version: %s", OPENDASH_VERSION_STR);
    ESP_LOGI(TAG, "Node Type: OPENDASH_NODE_CENTER");

    /* ────────────────────────────────────────────────────────────────────────
     * Step 1: Initialize NVS (Non-Volatile Storage)
     * ──────────────────────────────────────────────────────────────────────── */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* NVS partition was truncated and needs to be erased */
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized");

    /* ────────────────────────────────────────────────────────────────────────
     * Step 2: Load Display Configuration
     * ──────────────────────────────────────────────────────────────────────── */
    opendash_display_layout_t layout;
    opendash_err_t od_ret = opendash_config_load(OPENDASH_NODE_CENTER, &layout);
    if (od_ret != OPENDASH_OK) {
        ESP_LOGW(TAG, "Failed to load config from NVS, using defaults");
        opendash_config_reset_defaults(OPENDASH_NODE_CENTER, &layout);
    } else {
        ESP_LOGI(TAG, "Display configuration loaded from NVS");
    }

    /* ────────────────────────────────────────────────────────────────────────
     * Step 3: Initialize Display Hardware
     * ──────────────────────────────────────────────────────────────────────── */
    ESP_LOGI(TAG, "Initializing display hardware...");
    ESP_ERROR_CHECK(display_init());
    ESP_LOGI(TAG, "Display hardware initialized (800×480 IPS LCD)");

    /* ────────────────────────────────────────────────────────────────────────
     * Step 4: Initialize LVGL UI Manager
     * ──────────────────────────────────────────────────────────────────────── */
    ESP_LOGI(TAG, "Initializing UI manager...");
    ESP_ERROR_CHECK(ui_manager_init(&layout));
    ESP_LOGI(TAG, "UI manager initialized");

    /* ────────────────────────────────────────────────────────────────────────
     * Step 5: Start UI Rendering Task
     * ──────────────────────────────────────────────────────────────────────── */
    ESP_LOGI(TAG, "Starting UI task...");
    ui_manager_start();
    ESP_LOGI(TAG, "UI task running");

    ESP_LOGI(TAG, "OpenDash Center Display initialization complete");
    ESP_LOGI(TAG, "System ready - displaying baseline UI");

    /* ────────────────────────────────────────────────────────────────────────
     * Main Loop
     * 
     * The UI task runs independently. This loop can be used for additional
     * background tasks (I2C polling, OBD2 reading, etc.) in the future.
     * ──────────────────────────────────────────────────────────────────────── */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        /* Future: Add I2C master polling, OBD2 reading, etc. */
    }
}
