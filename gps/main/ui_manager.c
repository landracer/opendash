/**
 * @file ui_manager.c
 * @brief OpenDash GPS / Telemetry Unit — UI Manager Implementation
 *
 * Creates a professional baseline layout for the GPS display (466×466):
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
 *       │ │                  │ │
 *       │ │   G-Force Circle │ │
 *       │ │     [Viz]        │ │
 *       │ └──────────────────┘ │
 *        ╲ 12 Sats   275°     ╱
 *         └──────────────────┘
 *
 * @see LVGL Documentation: https://docs.lvgl.io/master/
 */

#include "ui_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "opendash_common.h"
#include "opendash_fonts.h"

static const char *TAG = "ui_manager";

/* Display dimensions */
#define LCD_H_RES   466
#define LCD_V_RES   466

/* UI component references */
static lv_obj_t *screen_main = NULL;
static lv_obj_t *speed_label = NULL;
static lv_obj_t *laptime_label = NULL;
static lv_obj_t *delta_label = NULL;
static lv_obj_t *gforce_circle = NULL;
static lv_obj_t *sat_label = NULL;
static lv_obj_t *heading_label = NULL;

/* Configuration */
static opendash_display_layout_t current_layout;

/* UI task handle */
static TaskHandle_t ui_task_handle = NULL;

/**
 * @brief Create the GPS speed display (large numeric at top).
 */
static void create_speed_display(lv_obj_t *parent)
{
    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_set_size(container, 300, 120);
    lv_obj_align(container, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    
    /* Label */
    lv_obj_t *label = lv_label_create(container);
    lv_label_set_text(label, "GPS SPEED");
    opendash_set_font(label, OPENDASH_FONT_SIZE_SMALL);
    lv_obj_set_style_text_color(label, lv_color_hex(0x808080), 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 0);
    
    /* Speed value */
    speed_label = lv_label_create(container);
    lv_label_set_text(speed_label, "--- km/h");
    opendash_set_font(speed_label, OPENDASH_FONT_SIZE_LARGE);
    lv_obj_set_style_text_color(speed_label, lv_color_hex(0x00FF00), 0);
    lv_obj_align(speed_label, LV_ALIGN_CENTER, 0, 15);
    
    ESP_LOGI(TAG, "Speed display created");
}

/**
 * @brief Create the lap timing display.
 */
static void create_lap_display(lv_obj_t *parent)
{
    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_set_size(container, 300, 100);
    lv_obj_align(container, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_style_bg_color(container, lv_color_hex(0x181818), 0);
    lv_obj_set_style_border_color(container, lv_color_hex(0x303030), 0);
    lv_obj_set_style_border_width(container, 2, 0);
    lv_obj_set_style_radius(container, 10, 0);
    
    /* Lap time */
    laptime_label = lv_label_create(container);
    lv_label_set_text(laptime_label, "LAP: --:--.---");
    opendash_set_font(laptime_label, OPENDASH_FONT_SIZE_MEDIUM);
    lv_obj_set_style_text_color(laptime_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(laptime_label, LV_ALIGN_TOP_MID, 0, 10);
    
    /* Delta */
    delta_label = lv_label_create(container);
    lv_label_set_text(delta_label, "Δ: ---");
    opendash_set_font(delta_label, OPENDASH_FONT_SIZE_SMALL);
    lv_obj_set_style_text_color(delta_label, lv_color_hex(0xFFFF00), 0);
    lv_obj_align(delta_label, LV_ALIGN_BOTTOM_MID, 0, -10);
    
    ESP_LOGI(TAG, "Lap display created");
}

/**
 * @brief Create the G-force visualization circle.
 */
static void create_gforce_display(lv_obj_t *parent)
{
    gforce_circle = lv_obj_create(parent);
    lv_obj_set_size(gforce_circle, 150, 150);
    lv_obj_align(gforce_circle, LV_ALIGN_CENTER, 0, 100);
    lv_obj_set_style_bg_color(gforce_circle, lv_color_hex(0x101010), 0);
    lv_obj_set_style_border_color(gforce_circle, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_border_width(gforce_circle, 3, 0);
    lv_obj_set_style_radius(gforce_circle, LV_RADIUS_CIRCLE, 0);
    
    /* G-force label */
    lv_obj_t *g_label = lv_label_create(gforce_circle);
    lv_label_set_text(g_label, "G");
    opendash_set_font(g_label, OPENDASH_FONT_SIZE_SMALL);
    lv_obj_set_style_text_color(g_label, lv_color_hex(0x808080), 0);
    lv_obj_center(g_label);
    
    ESP_LOGI(TAG, "G-force display created");
}

/**
 * @brief Create the status bar (satellites and heading).
 */
static void create_status_bar(lv_obj_t *parent)
{
    /* Satellite count (bottom left) */
    sat_label = lv_label_create(parent);
    lv_label_set_text(sat_label, "0 Sats");
    opendash_set_font(sat_label, OPENDASH_FONT_SIZE_SMALL);
    lv_obj_set_style_text_color(sat_label, lv_color_hex(0x808080), 0);
    lv_obj_align(sat_label, LV_ALIGN_BOTTOM_LEFT, 20, -10);
    
    /* Heading (bottom right) */
    heading_label = lv_label_create(parent);
    lv_label_set_text(heading_label, "---°");
    opendash_set_font(heading_label, OPENDASH_FONT_SIZE_SMALL);
    lv_obj_set_style_text_color(heading_label, lv_color_hex(0x808080), 0);
    lv_obj_align(heading_label, LV_ALIGN_BOTTOM_RIGHT, -20, -10);
    
    ESP_LOGI(TAG, "Status bar created");
}

/**
 * @brief UI rendering task.
 */
static void ui_task(void *pvParameters)
{
    ESP_LOGI(TAG, "UI task started");
    
    while (1) {
        lv_timer_handler();
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
    
    ESP_LOGI(TAG, "Creating baseline UI layout for 466×466 AMOLED display");
    
    /* Create main screen */
    screen_main = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen_main, lv_color_hex(0x000000), 0);
    
    /* Create UI components */
    create_speed_display(screen_main);
    create_lap_display(screen_main);
    create_gforce_display(screen_main);
    create_status_bar(screen_main);
    
    /* Load the screen */
    lv_scr_load(screen_main);
    
    ESP_LOGI(TAG, "Baseline UI layout created");
    
    return ESP_OK;
}

esp_err_t ui_manager_start(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(
        ui_task,
        "ui_task",
        4096,
        NULL,
        5,
        &ui_task_handle,
        1
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create UI task");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "UI task created on core 1");
    return ESP_OK;
}

void ui_manager_update_value(uint16_t data_point_id, float value)
{
    ESP_LOGD(TAG, "Update requested for data point 0x%04X: %.2f", data_point_id, value);
}
