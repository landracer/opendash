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
 * Licensed under Sovereign Individual License v1.0 — see LICENSE file
 *
 * @see ESP-IDF API Reference:
 *      https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32s3/api-reference/index.html
 */

#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "display_init.h"
#include "ui_manager.h"
#include "espnow_master.h"
#include "opendash_common.h"
#include "opendash_display_config.h"
#include "opendash_identity.h"
#include "opendash_uart.h"
#include "opendash_obd_config.h"
#include "opendash_i2c_protocol.h"
#include "splash_center.h"
#include "background_center.h"
#include "system_config.h"
#include "boost_client.h"

static const char *TAG = "opendash_center";

static bool node_supports_ota(opendash_node_t node)
{
    switch (node) {
        case OPENDASH_NODE_LEFT:
        case OPENDASH_NODE_RIGHT:
        case OPENDASH_NODE_GPS:
        case OPENDASH_NODE_POD1:
        case OPENDASH_NODE_POD2:
        case OPENDASH_NODE_RELAY_4CH:
        case OPENDASH_NODE_RELAY_8CH_A:
        case OPENDASH_NODE_RELAY_8CH_B:
        case OPENDASH_NODE_MOS_4CH_A:
        case OPENDASH_NODE_MOS_4CH_B:
            return true;
        default:
            return false;
    }
}

static bool parse_node_name(const char *name, opendash_node_t *out)
{
    if (!name || !out) {
        return false;
    }

    if (strcmp(name, "left") == 0) {
        *out = OPENDASH_NODE_LEFT;
    } else if (strcmp(name, "right") == 0) {
        *out = OPENDASH_NODE_RIGHT;
    } else if (strcmp(name, "gps") == 0) {
        *out = OPENDASH_NODE_GPS;
    } else if (strcmp(name, "pod1") == 0) {
        *out = OPENDASH_NODE_POD1;
    } else if (strcmp(name, "pod2") == 0) {
        *out = OPENDASH_NODE_POD2;
    } else if (strcmp(name, "relay4") == 0 || strcmp(name, "relay_4ch") == 0) {
        *out = OPENDASH_NODE_RELAY_4CH;
    } else if (strcmp(name, "relay8a") == 0 || strcmp(name, "relay_8ch_a") == 0) {
        *out = OPENDASH_NODE_RELAY_8CH_A;
    } else if (strcmp(name, "relay8b") == 0 || strcmp(name, "relay_8ch_b") == 0) {
        *out = OPENDASH_NODE_RELAY_8CH_B;
    } else if (strcmp(name, "mosa") == 0 || strcmp(name, "mos_4ch_a") == 0) {
        *out = OPENDASH_NODE_MOS_4CH_A;
    } else if (strcmp(name, "mosb") == 0 || strcmp(name, "mos_4ch_b") == 0) {
        *out = OPENDASH_NODE_MOS_4CH_B;
    } else {
        return false;
    }

    return true;
}

static void trim_ascii(char *s)
{
    if (!s || s[0] == '\0') {
        return;
    }

    size_t len = strlen(s);
    size_t start = 0;
    while (start < len && isspace((unsigned char)s[start])) {
        start++;
    }

    size_t end = len;
    while (end > start && isspace((unsigned char)s[end - 1])) {
        end--;
    }

    if (start > 0 || end < len) {
        memmove(s, s + start, end - start);
        s[end - start] = '\0';
    }
}

static void ota_serial_cmd_task(void *arg)
{
    (void)arg;

    char line[96];
    ESP_LOGI(TAG,
             "Serial command task ready. Use: ota <left|right|gps|pod1|pod2|relay4|relay8a|relay8b|mosa|mosb>");

    while (1) {
        if (fgets(line, sizeof(line), stdin) == NULL) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        trim_ascii(line);
        if (line[0] == '\0') {
            continue;
        }

        for (size_t i = 0; line[i] != '\0'; ++i) {
            line[i] = (char)tolower((unsigned char)line[i]);
        }

        if (strcmp(line, "help") == 0 || strcmp(line, "?") == 0) {
            ESP_LOGI(TAG,
                     "Commands: ota <node>, nodes, help");
            ESP_LOGI(TAG,
                     "Nodes: left right gps pod1 pod2 relay4 relay8a relay8b mosa mosb");
            continue;
        }

        if (strcmp(line, "nodes") == 0) {
            ESP_LOGI(TAG,
                     "Nodes: left right gps pod1 pod2 relay4 relay8a relay8b mosa mosb");
            continue;
        }

        if (strncmp(line, "ota", 3) == 0) {
            char *node_name = line + 3;
            trim_ascii(node_name);
            if (node_name[0] == '\0') {
                ESP_LOGW(TAG, "Usage: ota <node>");
                continue;
            }

            opendash_node_t node;
            if (!parse_node_name(node_name, &node)) {
                ESP_LOGW(TAG, "Unknown node '%s'. Type 'nodes' for list.", node_name);
                continue;
            }

            if (!node_supports_ota(node)) {
                ESP_LOGW(TAG, "Node '%s' does not support ENTER_BT_OTA", node_name);
                continue;
            }

            int sent_ok = 0;
            int attempts = 0;
            esp_err_t first_err = ESP_FAIL;
            /* Under heavy CH1 backpressure, short bursts often land in quarantine windows.
             * Stretch retries so ENTER_BT_OTA has multiple chances to pass through. */
            for (int i = 0; i < 15; ++i) {
                attempts++;
                esp_err_t err = espnow_master_send_system_subcmd(node, OPENDASH_SUBCMD_ENTER_BT_OTA);
                if (i == 0) {
                    first_err = err;
                }
                if (err == ESP_OK) {
                    sent_ok++;
                    if (sent_ok >= 3) {
                        break;
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(150));
            }

            if (sent_ok > 0) {
                ESP_LOGI(TAG, "Sent ENTER_BT_OTA x%d to %s (attempts=%d)", sent_ok, node_name, attempts);
            } else {
                ESP_LOGW(TAG,
                         "Failed to send ENTER_BT_OTA to %s after %d attempts (first err=0x%x)",
                         node_name,
                         attempts,
                         first_err);
            }
            continue;
        }

        ESP_LOGW(TAG, "Unknown command '%s'. Type 'help'.", line);
    }
}

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

    /* ── Load OBD2 Config from NVS ── */
    obd_config_load();
    ESP_LOGI(TAG, "OBD config loaded (enabled=%d, mil=%d)",
             obd_config_get()->obd_enabled,
             obd_config_get()->mil_indicator_enabled);

    /* ── Device Identity Check ── */
    opendash_identity_init(OPENDASH_NODE_CENTER);

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
     * Step 3d: Initialize Multidisplay UART Receiver
     *
     * Listens on GPIO20 (USB D+ reclaimed) for SERIALOUT_BINARY frames from
     * the multidisplay HC-06 Bluetooth adapter at 9600 baud.
     * Parsed data (EGT1-4, O2/Lambda, MAF, RPM) feeds DISPLAY_MODE_MD.
     * ──────────────────────────────────────────────────────────────────────── */
    ESP_LOGI(TAG, "Initializing multidisplay UART receiver...");
    if (!opendash_uart_init()) {
        ESP_LOGW(TAG, "Multidisplay UART init failed — MD screen will show no data");
    } else {
        ESP_LOGI(TAG, "Multidisplay UART receiver active (GPIO%d, 9600 baud)",
                 OPENDASH_UART_RX_PIN);
    }

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
     * Step 7: System Config + Boost Controller client
     *
     * system_config_init() loads the persisted boost target node (default
     * mos_4ch_a) from NVS.  boost_client_init() registers the aux RX hook
     * with the ESP-NOW dispatcher, starts the 10 Hz live-data pusher, and
     * issues a one-shot PULL_ALL on slave-online to sync settings.
     * ──────────────────────────────────────────────────────────────────────── */
    ESP_LOGI(TAG, "Initialising system_config + boost_client...");
    system_config_init();
    boost_client_init();
    ESP_LOGI(TAG, "Boost client running");

    xTaskCreate(ota_serial_cmd_task,
                "ota_serial_cmd",
                4096,
                NULL,
                4,
                NULL);

    ESP_LOGI(TAG, "OpenDash Center Display initialization complete");
    ESP_LOGI(TAG, "System ready - displaying baseline UI");

    /* ────────────────────────────────────────────────────────────────────────
     * Main Loop
     * 
     * MIL/CEL indicator is handled data-driven in espnow_master.c via
     * OPENDASH_DP_OBD2_FLAGS / OPENDASH_DP_MIL_ON data points from left pod.
     * This loop handles warning threshold checks for OBD sensor values.
     * ──────────────────────────────────────────────────────────────────────── */
    opendash_md_data_t md_snap;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        /* ── Warning threshold checks (via UART data if available) ── */
        if (opendash_uart_get_data(&md_snap) && md_snap.obd2_present) {
            const obd_config_t *cfg = obd_config_get();
            static bool warn_active[OBD_WARN_THRESHOLD_COUNT] = {0};

            struct { float val; obd_warn_sensor_t idx; } checks[] = {
                { md_snap.obd2_coolant_temp, OBD_WARN_COOLANT_TEMP },
                { md_snap.obd2_oil_temp,     OBD_WARN_OIL_TEMP },
                /* Oil pressure not in standard PIDs — skip if 0 */
                { md_snap.obd2_ctrl_voltage, OBD_WARN_BATTERY_VOLT },
            };
            int check_count = 3;

            for (int i = 0; i < check_count; i++) {
                obd_warn_sensor_t si = checks[i].idx;
                const obd_warning_threshold_t *w = &cfg->warnings[si];
                if (w->critical == 0.0f && w->caution == 0.0f) continue;

                float val = checks[i].val;
                bool tripped = false;
                opendash_warning_level_t level = OPENDASH_WARNING_NONE;

                if (w->above) {
                    if (w->critical > 0 && val >= w->critical)
                        { tripped = true; level = OPENDASH_WARNING_CRITICAL; }
                    else if (w->caution > 0 && val >= w->caution)
                        { tripped = true; level = OPENDASH_WARNING_CAUTION; }
                } else {
                    if (w->critical > 0 && val <= w->critical)
                        { tripped = true; level = OPENDASH_WARNING_CRITICAL; }
                    else if (w->caution > 0 && val <= w->caution)
                        { tripped = true; level = OPENDASH_WARNING_CAUTION; }
                }

                if (tripped && !warn_active[si]) {
                    if (display_lvgl_lock(50)) {
                        ui_manager_warning_box_trigger(0, level, NULL, 0);
                        display_lvgl_unlock();
                    }
                    warn_active[si] = true;
                    ESP_LOGW(TAG, "OBD warning tripped: sensor %d val=%.1f", si, val);
                } else if (!tripped && warn_active[si]) {
                    if (display_lvgl_lock(50)) {
                        ui_manager_warning_box_clear(0);
                        display_lvgl_unlock();
                    }
                    warn_active[si] = false;
                }
            }
        }
    }
}
