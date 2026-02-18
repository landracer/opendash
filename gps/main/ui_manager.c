/**
 * @file ui_manager.c
 * @brief OpenDash GPS / Telemetry Unit — UI Manager Implementation
 *
 * Creates a professional, fully functional layout for the GPS display (466×466):
 * 
 * Layout:
 *         ┌──────────────────┐
 *        ╱    GPS Speed       ╲
 *       │    (Large numeric)   │
 *       │       125 km/h       │
 *       │                      │
 *       │ ┌──────────────────┐ │
 *       │ │   Lap Time       │ │
 *       │ │   1:32.456       │ │
 *       │ │   Lap Delta      │ │
 *       │ │   +0.234s        │ │
 *       │ │   G-Force: 1.2G  │ │
 *       │ └──────────────────┘ │
 *        ╲ 12 Sats   275°     ╱
 *         └──────────────────┘
 *
 * Real-time data updates from GPS and IMU sensors.
 * 
 * @see LVGL Documentation: https://docs.lvgl.io/master/
 */

#include "ui_manager.h"
#include "gps_handler.h"
#include "imu_handler.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "opendash_common.h"
#include "opendash_fonts.h"
#include "opendash_ui_styles.h"
#include <stdio.h>
#include <math.h>

static const char *TAG = "ui_manager";

/* Display dimensions */
#define LCD_H_RES   466
#define LCD_V_RES   466

/* Update intervals (ms) */
#define UI_UPDATE_INTERVAL_MS       500  /* Update display every 500ms */
#define DATA_FETCH_TIMEOUT_MS       100  /* Timeout when fetching sensor data */

/* UI component references */
static lv_obj_t *screen_main = NULL;
static lv_obj_t *speed_label = NULL;
static lv_obj_t *speed_unit_label = NULL;
static lv_obj_t *laptime_label = NULL;
static lv_obj_t *delta_label = NULL;
static lv_obj_t *gforce_label = NULL;
static lv_obj_t *sat_label = NULL;
static lv_obj_t *heading_label = NULL;
static lv_obj_t *fix_status_label = NULL;
static lv_obj_t *coords_label = NULL;
static lv_obj_t *info_panel = NULL;

/* Configuration */
static opendash_display_layout_t current_layout;

/* UI task handle */
static TaskHandle_t ui_task_handle = NULL;

/* Mutex for thread-safe LVGL access */
static SemaphoreHandle_t lvgl_mutex = NULL;

/* Lap timing simulation */
static uint32_t lap_start_ticks = 0;
static uint32_t lap_best_time_ms = 0;
static bool lap_in_progress = false;

/**
 * @brief Lock LVGL mutex
 */
static inline void lvgl_lock(void)
{
    if (lvgl_mutex) {
        xSemaphoreTake(lvgl_mutex, portMAX_DELAY);
    }
}

/**
 * @brief Unlock LVGL mutex
 */
static inline void lvgl_unlock(void)
{
    if (lvgl_mutex) {
        xSemaphoreGive(lvgl_mutex);
    }
}

/**
 * @brief Create the primary speed display area.
 */
static void create_speed_display(lv_obj_t *parent)
{
    /* Speed label ("---") */
    speed_label = lv_label_create(parent);
    lv_label_set_text(speed_label, "---");
    lv_obj_set_style_text_font(speed_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(speed_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(speed_label, LV_ALIGN_TOP_MID, 0, 40);
    
    /* Unit label */
    speed_unit_label = lv_label_create(parent);
    lv_label_set_text(speed_unit_label, "km/h");
    lv_obj_set_style_text_font(speed_unit_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(speed_unit_label, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(speed_unit_label, LV_ALIGN_TOP_MID, 0, 110);
    
    ESP_LOGI(TAG, "Speed display created");
}

/**
 * @brief Create the info panel (lap time, delta, g-force).
 */
static void create_info_panel(lv_obj_t *parent)
{
    info_panel = lv_obj_create(parent);
    lv_obj_set_size(info_panel, 380, 140);
    lv_obj_align(info_panel, LV_ALIGN_CENTER, 0, 30);
    lv_obj_set_style_bg_color(info_panel, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_border_color(info_panel, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(info_panel, 1, 0);
    lv_obj_set_style_radius(info_panel, 8, 0);
    lv_obj_set_style_pad_all(info_panel, 12, 0);
    
    /* Lap time line: "LAP: 01:23.456" */
    laptime_label = lv_label_create(info_panel);
    lv_label_set_text(laptime_label, "LAP: --:--.---;");
    lv_obj_set_style_text_font(laptime_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(laptime_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(laptime_label, LV_ALIGN_TOP_LEFT, 0, 0);
    
    /* Delta line: "Δ: +0.123s" */
    delta_label = lv_label_create(info_panel);
    lv_label_set_text(delta_label, "Δ: --.---;");
    lv_obj_set_style_text_font(delta_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(delta_label, lv_color_hex(0xFFFF00), 0);
    lv_obj_align(delta_label, LV_ALIGN_TOP_RIGHT, 0, 0);
    
    /* G-force line: "G: -.-- (x: -.--, y: -.--, z: --.--)" */
    gforce_label = lv_label_create(info_panel);
    lv_label_set_text(gforce_label, "G: -.-");
    lv_obj_set_style_text_font(gforce_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(gforce_label, lv_color_hex(0x00FF00), 0);
    lv_obj_align(gforce_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    
    /* Coordinates line: "Lat: dd.ddddd Lon: dd.ddddd" */
    coords_label = lv_label_create(info_panel);
    lv_label_set_text(coords_label, "Lat: --.- Lon: --.-");
    lv_obj_set_style_text_font(coords_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(coords_label, lv_color_hex(0x888888), 0);
    lv_obj_align(coords_label, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    
    ESP_LOGI(TAG, "Info panel created");
}

/**
 * @brief Create the status bar (satellites, heading, GPS fix status).
 */
static void create_status_bar(lv_obj_t *parent)
{
    /* GPS Fix status indicator (top right corner) */
    fix_status_label = lv_label_create(parent);
    lv_label_set_text(fix_status_label, "NO FIX");
    lv_obj_set_style_text_font(fix_status_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(fix_status_label, lv_color_hex(0xFF3333), 0);
    lv_obj_align(fix_status_label, LV_ALIGN_TOP_RIGHT, -15, 10);
    
    /* Satellite count (bottom left) */
    sat_label = lv_label_create(parent);
    lv_label_set_text(sat_label, "-- Sats");
    lv_obj_set_style_text_font(sat_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(sat_label, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(sat_label, LV_ALIGN_BOTTOM_LEFT, 15, -15);
    
    /* Heading (bottom right) */
    heading_label = lv_label_create(parent);
    lv_label_set_text(heading_label, "---°");
    lv_obj_set_style_text_font(heading_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(heading_label, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(heading_label, LV_ALIGN_BOTTOM_RIGHT, -15, -15);
    
    ESP_LOGI(TAG, "Status bar created");
}

/**
 * @brief Update display with current GPS and IMU data.
 */
static void update_display_data(void)
{
    /* Fetch GPS data */
    gps_data_t gps_data = {0};
    if (gps_handler_get_data(&gps_data) != ESP_OK) {
        ESP_LOGD(TAG, "Failed to fetch GPS data");
        return;
    }
    
    /* Fetch IMU data */
    imu_data_t imu_data = {0};
    if (imu_handler_get_data(&imu_data) != ESP_OK) {
        ESP_LOGD(TAG, "Failed to fetch IMU data");
    }
    
    lvgl_lock();
    
    /* Update speed */
    char speed_str[16];
    if (gps_data.fix_valid) {
        snprintf(speed_str, sizeof(speed_str), "%.0f", gps_data.speed);
    } else {
        snprintf(speed_str, sizeof(speed_str), "---");
    }
    lv_label_set_text(speed_label, speed_str);
    
    /* Update GPS fix status */
    if (gps_data.fix_valid) {
        lv_label_set_text(fix_status_label, "3D FIX");
        lv_obj_set_style_text_color(fix_status_label, lv_color_hex(0x33FF33), 0);
    } else {
        lv_label_set_text(fix_status_label, "NO FIX");
        lv_obj_set_style_text_color(fix_status_label, lv_color_hex(0xFF3333), 0);
    }
    
    /* Update satellite count */
    char sat_str[16];
    snprintf(sat_str, sizeof(sat_str), "%d Sats", gps_data.satellites);
    lv_label_set_text(sat_label, sat_str);
    
    /* Update heading */
    char heading_str[16];
    if (gps_data.fix_valid && gps_data.speed > 1.0f) {
        snprintf(heading_str, sizeof(heading_str), "%.0f°", gps_data.heading);
    } else {
        snprintf(heading_str, sizeof(heading_str), "---°");
    }
    lv_label_set_text(heading_label, heading_str);
    
    /* Update coordinates */
    char coords_str[64];
    snprintf(coords_str, sizeof(coords_str), "Lat: %.4f Lon: %.4f",
             gps_data.latitude, gps_data.longitude);
    lv_label_set_text(coords_label, coords_str);
    
    /* Update lap timing (simulated) */
    if (gps_data.fix_valid && gps_data.speed > 5.0f && !lap_in_progress) {
        /* Start new lap */
        lap_in_progress = true;
        lap_start_ticks = xTaskGetTickCount();
    }
    
    char laptime_str[32];
    char delta_str[32];
    
    if (lap_in_progress) {
        uint32_t elapsed_ms = (xTaskGetTickCount() - lap_start_ticks) * portTICK_PERIOD_MS;
        uint32_t minutes = elapsed_ms / 60000;
        uint32_t seconds = (elapsed_ms % 60000) / 1000;
        uint32_t millis = elapsed_ms % 1000;
        
        snprintf(laptime_str, sizeof(laptime_str), "LAP: %02lu:%02lu.%03lu",
                 minutes, seconds, millis);
        
        if (lap_best_time_ms > 0) {
            int32_t delta_ms = (int32_t)elapsed_ms - (int32_t)lap_best_time_ms;
            snprintf(delta_str, sizeof(delta_str), "Δ: %+.2f",
                     delta_ms / 1000.0f);
        } else {
            snprintf(delta_str, sizeof(delta_str), "Δ: --.-");
        }
    } else {
        snprintf(laptime_str, sizeof(laptime_str), "LAP: --:--.---");
        snprintf(delta_str, sizeof(delta_str), "Δ: --.-");
    }
    
    lv_label_set_text(laptime_label, laptime_str);
    lv_label_set_text(delta_label, delta_str);
    
    /* Update G-force */
    char gforce_str[64];
    float total_g = sqrtf(imu_data.accel_x * imu_data.accel_x +
                          imu_data.accel_y * imu_data.accel_y +
                          imu_data.accel_z * imu_data.accel_z);
    snprintf(gforce_str, sizeof(gforce_str), "G: %.2f (x:%.2f y:%.2f z:%.2f)",
             total_g, imu_data.accel_x, imu_data.accel_y, imu_data.accel_z);
    lv_label_set_text(gforce_label, gforce_str);
    
    lvgl_unlock();
}

/**
 * @brief UI update task - periodically refreshes sensor data on display.
 */
static void ui_update_task(void *pvParameters)
{
    ESP_LOGI(TAG, "UI update task started");
    TickType_t last_wake = xTaskGetTickCount();
    
    while (1) {
        update_display_data();
        
        /* Wait for next update interval */
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(UI_UPDATE_INTERVAL_MS));
    }
}

/**
 * @brief LVGL rendering task - must run at high frequency.
 */
static void ui_render_task(void *pvParameters)
{
    ESP_LOGI(TAG, "UI render task started");
    
    while (1) {
        lvgl_lock();
        lv_timer_handler();
        lvgl_unlock();
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t ui_manager_init(const opendash_display_layout_t *layout)
{
    if (layout == NULL) {
        ESP_LOGE(TAG, "Layout pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    memcpy(&current_layout, layout, sizeof(opendash_display_layout_t));
    
    /* Create LVGL mutex */
    lvgl_mutex = xSemaphoreCreateMutex();
    if (lvgl_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create LVGL mutex");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Creating UI layout for 466×466 AMOLED display");
    
    /* Create main screen */
    lvgl_lock();
    
    screen_main = lv_obj_create(NULL);
    lv_obj_set_size(screen_main, LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_bg_color(screen_main, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(screen_main, 0, 0);
    lv_obj_set_style_pad_all(screen_main, 0, 0);
    
    /* Create UI components */
    create_speed_display(screen_main);
    create_info_panel(screen_main);
    create_status_bar(screen_main);
    
    /* Load and display the screen */
    lv_scr_load(screen_main);
    
    lvgl_unlock();
    
    ESP_LOGI(TAG, "UI layout created successfully");
    
    return ESP_OK;
}

esp_err_t ui_manager_start(void)
{
    /* Create update task (lower priority, periodic) */
    BaseType_t ret = xTaskCreatePinnedToCore(
        ui_update_task,
        "ui_update",
        4096,
        NULL,
        4,
        NULL,
        0  /* Core 0 for sensor data fetching */
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create UI update task");
        return ESP_FAIL;
    }
    
    /* Create render task (higher priority, frequent) */
    ret = xTaskCreatePinnedToCore(
        ui_render_task,
        "ui_render",
        4096,
        NULL,
        5,
        &ui_task_handle,
        1  /* Core 1 for LVGL rendering */
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create UI render task");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "UI tasks created and running");
    return ESP_OK;
}

void ui_manager_update_value(uint16_t data_point_id, float value)
{
    /* Called by central system when values change */
    ESP_LOGD(TAG, "Update requested for data point 0x%04X: %.2f", data_point_id, value);
}
