/**
 * @file ui_manager.c
 * @brief OpenDash Left/Right Gauges — UI Manager Implementation
 *
 * Creates a professional baseline layout for the round gauge displays (480×480):
 * 
 * Layout:
 *         ┌──────────────────┐
 *        ╱    Section A       ╲
 *       │   (Primary Value)    │
 *       │   Large numeric      │
 *       │      OIL TEMP        │
 *       │                      │
 *       │ ┌──────────────────┐ │
 *       │ │   Section B      │ │
 *       │ │ (Secondary Value)│ │
 *       │ │    BOOST kPa     │ │
 *       │ └──────────────────┘ │
 *        ╲   Arc Gauge Bar    ╱
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
#define LCD_H_RES   480
#define LCD_V_RES   480

/* UI component references */
static lv_obj_t *screen_main = NULL;
static lv_obj_t *arc_gauge = NULL;
static lv_obj_t *primary_label = NULL;
static lv_obj_t *primary_value = NULL;
static lv_obj_t *secondary_label = NULL;
static lv_obj_t *secondary_value = NULL;

/* Configuration */
static opendash_display_layout_t current_layout;

/* UI task handle */
static TaskHandle_t ui_task_handle = NULL;

/**
 * @brief Create the circular arc gauge that surrounds the display.
 */
static void create_arc_gauge(lv_obj_t *parent)
{
    arc_gauge = lv_arc_create(parent);
    lv_obj_set_size(arc_gauge, LCD_H_RES - 20, LCD_V_RES - 20);
    lv_obj_center(arc_gauge);
    
    /* Configure arc */
    lv_arc_set_rotation(arc_gauge, 135);
    lv_arc_set_bg_angles(arc_gauge, 0, 270);
    lv_arc_set_value(arc_gauge, 0);
    lv_arc_set_range(arc_gauge, 0, 100);
    
    /* Remove knob */
    lv_obj_remove_style(arc_gauge, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc_gauge, LV_OBJ_FLAG_CLICKABLE);
    
    /* Style the arc */
    lv_obj_set_style_arc_width(arc_gauge, 30, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc_gauge, 30, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc_gauge, lv_color_hex(0x202020), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc_gauge, lv_color_hex(0x00AAFF), LV_PART_INDICATOR);
    
    ESP_LOGI(TAG, "Arc gauge created");
}

/**
 * @brief Create the primary data display (large numeric value).
 */
static void create_primary_display(lv_obj_t *parent)
{
    /* Container for primary section */
    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_set_size(container, 300, 180);
    lv_obj_align(container, LV_ALIGN_CENTER, 0, -80);
    lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    
    /* Primary label (data point name) */
    primary_label = lv_label_create(container);
    lv_label_set_text(primary_label, "OIL TEMP");
    opendash_set_font(primary_label, OPENDASH_FONT_SIZE_SMALL);
    lv_obj_set_style_text_color(primary_label, lv_color_hex(0x808080), 0);
    lv_obj_align(primary_label, LV_ALIGN_TOP_MID, 0, 0);
    
    /* Primary value */
    primary_value = lv_label_create(container);
    lv_label_set_text(primary_value, "---");
    opendash_set_font(primary_value, OPENDASH_FONT_SIZE_LARGE);
    lv_obj_set_style_text_color(primary_value, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(primary_value, LV_ALIGN_CENTER, 0, 10);
    
    /* Unit label */
    lv_obj_t *unit_label = lv_label_create(container);
    lv_label_set_text(unit_label, "°C");
    opendash_set_font(unit_label, OPENDASH_FONT_SIZE_SMALL);
    lv_obj_set_style_text_color(unit_label, lv_color_hex(0x808080), 0);
    lv_obj_align(unit_label, LV_ALIGN_BOTTOM_MID, 0, 0);
    
    ESP_LOGI(TAG, "Primary display created");
}

/**
 * @brief Create the secondary data display (smaller value).
 */
static void create_secondary_display(lv_obj_t *parent)
{
    /* Container for secondary section */
    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_set_size(container, 250, 120);
    lv_obj_align(container, LV_ALIGN_CENTER, 0, 100);
    lv_obj_set_style_bg_color(container, lv_color_hex(0x181818), 0);
    lv_obj_set_style_border_color(container, lv_color_hex(0x303030), 0);
    lv_obj_set_style_border_width(container, 2, 0);
    lv_obj_set_style_radius(container, 10, 0);
    
    /* Secondary label */
    secondary_label = lv_label_create(container);
    lv_label_set_text(secondary_label, "BOOST");
    opendash_set_font(secondary_label, OPENDASH_FONT_SIZE_SMALL);
    lv_obj_set_style_text_color(secondary_label, lv_color_hex(0x808080), 0);
    lv_obj_align(secondary_label, LV_ALIGN_TOP_MID, 0, 10);
    
    /* Secondary value */
    secondary_value = lv_label_create(container);
    lv_label_set_text(secondary_value, "--- kPa");
    opendash_set_font(secondary_value, OPENDASH_FONT_SIZE_MEDIUM);
    lv_obj_set_style_text_color(secondary_value, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(secondary_value, LV_ALIGN_CENTER, 0, 10);
    
    ESP_LOGI(TAG, "Secondary display created");
}

/**
 * @brief UI rendering task.
 */
static void ui_task(void *pvParameters)
{
    ESP_LOGI(TAG, "UI task started");
    
    while (1) {
        lv_task_handler();
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
    
    ESP_LOGI(TAG, "Creating baseline UI layout for 480×480 round display");
    
    /* Create main screen */
    screen_main = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen_main, lv_color_hex(0x000000), 0);
    
    /* Create UI components */
    create_arc_gauge(screen_main);
    create_primary_display(screen_main);
    create_secondary_display(screen_main);
    
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
