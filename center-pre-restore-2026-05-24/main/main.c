/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file main.c
 * @brief OpenDash Center Display — Entry Point
 *
 * Hardware: Waveshare ESP32-S3-Touch-LCD-4.3
 * Resolution: 800×480 IPS LCD
 * Touch: Capacitive touch controller
 * Role: ESP-NOW Master, main display coordinator
 *
 * This is the main entry point for the Center Display unit. It initializes:
 * - ESP-IDF system services
 * - Display hardware (LCD + touch)
 * - LVGL graphics library
 * - ESP-NOW wireless master for communication with other nodes
 * - OBD2/CAN interface (if available)
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
#include "espnow_master.h"
#include "system_config.h"
#include "boost_client.h"
#include "opendash_common.h"
#include "opendash_display_config.h"
#include "splash_center.h"
#include "background_center.h"

static const char *TAG = "opendash_center";

/**
 * @brief Display splash screen for startup
 *
 * Shows the splash image for a brief period before transitioning
 * to the main UI. This provides visual feedback during initialization.
 */
static void show_splash_screen(void)
{
    ESP_LOGI(TAG, "Displaying splash screen...");
    
    /* Create splash screen */
    lv_obj_t *splash_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(splash_screen, lv_color_hex(0x000000), 0);
    
    /* Add splash image */
    lv_obj_t *splash_img = lv_image_create(splash_screen);
    lv_image_set_src(splash_img, &splash_center_dsc);
    lv_obj_center(splash_img);
    
    /* Load splash screen */
    lv_scr_load(splash_screen);
    
    /* Process LVGL for a period to ensure screen renders */
    for (int i = 0; i < 200; i++) {  /* ~2 seconds at 10ms per iteration */
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    ESP_LOGI(TAG, "Splash screen complete");
}

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
     * Step 3b: Display Splash Screen
     * ──────────────────────────────────────────────────────────────────────── */
    show_splash_screen();

    /* ────────────────────────────────────────────────────────────────────────
     * Step 3c: Initialize ESP-NOW Wireless Master
     *
     * Uses ESP-NOW (WiFi peer-to-peer) for zero-wire communication with
     * all peripheral nodes (Left, Right, GPS).  No wires, no GPIO conflicts.
     * ──────────────────────────────────────────────────────────────────────── */
    ESP_LOGI(TAG, "Initializing ESP-NOW wireless master...");
    ESP_ERROR_CHECK(espnow_master_init());

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

    /* ────────────────────────────────────────────────────────────────────────
     * Step 6: Start ESP-NOW Master Polling Task
     *
     * Broadcasts discovery PINGs, pushes demo data to online gauge pods,
     * and requests telemetry from GPS — all wirelessly via ESP-NOW.
     * ──────────────────────────────────────────────────────────────────────── */
    ESP_LOGI(TAG, "Starting ESP-NOW master polling task...");
    ESP_ERROR_CHECK(espnow_master_start());
    ESP_LOGI(TAG, "ESP-NOW master polling active");

    /* ────────────────────────────────────────────────────────────────────────
     * Step 7: Boost controller client
     *
     * Loads target node from NVS, spawns the 10 Hz live-data push task, and
     * registers the auxiliary RX callback that ingests boost telemetry +
     * map echoes coming back from the MOS-4CH-A slave.
     * ──────────────────────────────────────────────────────────────────────── */
    ESP_LOGI(TAG, "Initializing system config + boost client...");
    ESP_ERROR_CHECK(system_config_init());
    ESP_ERROR_CHECK(boost_client_init());
    ESP_LOGI(TAG, "Boost client active — target node = %u", g_boost_target_node);

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

        /* Optionally query node status for center UI in the future */
        /* i2c_master_node_status_t status;
           i2c_master_get_status(&status); */
    }
}
