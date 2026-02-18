/**
 * @file main.c
 * @brief OpenDash Left/Right Gauges — Entry Point
 *
 * Hardware: Waveshare ESP32-S3-LCD-2.8C
 * Resolution: 480×480 Round IPS LCD
 * Role: I2C Slave, displays data received from Center unit
 *
 * This is the entry point for the Left or Right gauge pod. The node ID
 * is determined at runtime (can be configured via GPIO or NVS).
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

static const char *TAG = "opendash_leftright";

/**
 * @brief Main application entry point.
 */
void app_main(void)
{
    ESP_LOGI(TAG, "OpenDash Left/Right Gauge Starting...");
    ESP_LOGI(TAG, "Version: %s", OPENDASH_VERSION_STR);
    ESP_LOGI(TAG, "Node Type: OPENDASH_NODE_LEFT/RIGHT");

    /* ────────────────────────────────────────────────────────────────────────
     * Step 1: Initialize NVS (Non-Volatile Storage)
     * ──────────────────────────────────────────────────────────────────────── */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized");

    /* ────────────────────────────────────────────────────────────────────────
     * Step 2: Load Display Configuration
     * ──────────────────────────────────────────────────────────────────────── */
    opendash_display_layout_t layout;
    opendash_err_t od_ret = opendash_config_load(OPENDASH_NODE_LEFT, &layout);
    if (od_ret != OPENDASH_OK) {
        ESP_LOGW(TAG, "Failed to load config from NVS, using defaults");
        opendash_config_reset_defaults(OPENDASH_NODE_LEFT, &layout);
    } else {
        ESP_LOGI(TAG, "Display configuration loaded from NVS");
    }

    /* ────────────────────────────────────────────────────────────────────────
     * Step 3: Initialize Display Hardware
     * ──────────────────────────────────────────────────────────────────────── */
    ESP_LOGI(TAG, "Initializing display hardware...");
    ESP_ERROR_CHECK(display_init());
    ESP_LOGI(TAG, "Display hardware initialized (480×480 Round LCD)");

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

    ESP_LOGI(TAG, "OpenDash Left/Right Gauge initialization complete");
    ESP_LOGI(TAG, "System ready - displaying baseline UI");

    /* ────────────────────────────────────────────────────────────────────────
     * Main Loop
     * ──────────────────────────────────────────────────────────────────────── */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        /* Future: Add I2C slave response handling */
    }
}
