/**
 * @file ui_manager.c
 * @brief OpenDash Center Display — UI Manager Implementation
 *
 * Creates a professional baseline layout for the center display (800×480):
 * 
 * Layout:
 * ┌──────────────────────────────────────────────────────────┐
 * │  RPM Bar — Full Width Arc (Top Section)                  │
 * ├──────────────┬───────────────────────┬───────────────────┤
 * │  Section A   │     Section B         │    Section C      │
 * │  Coolant °C  │     SPEED (GPS)       │    Boost kPa      │
 * │              │    (Large numeric)    │                   │
 * ├──────────────┼───────────────────────┼───────────────────┤
 * │  Section D   │     Section E         │    Section F      │
 * │  Oil Temp    │    Lap Time/Delta     │    AFR            │
 * │              │                       │                   │
 * ├──────────────┴───────────────────────┴───────────────────┤
 * │  Status Bar — Warnings, Alarms, Checklist Status         │
 * └──────────────────────────────────────────────────────────┘
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
#define LCD_H_RES   800
#define LCD_V_RES   480

/* UI component references */
static lv_obj_t *screen_main = NULL;
static lv_obj_t *rpm_arc = NULL;
static lv_obj_t *sections[6] = {NULL};  /* A through F */
static lv_obj_t *status_bar = NULL;

/* Configuration */
static opendash_display_layout_t current_layout;

/* UI task handle */
static TaskHandle_t ui_task_handle = NULL;

/**
 * @brief Create the RPM arc gauge at the top of the display.
 *
 * This creates a full-width arc that sweeps from left to right,
 * displaying engine RPM.
 */
static void create_rpm_arc(lv_obj_t *parent)
{
    /* Create arc indicator */
    rpm_arc = lv_arc_create(parent);
    lv_obj_set_size(rpm_arc, LCD_H_RES - 40, 120);
    lv_obj_align(rpm_arc, LV_ALIGN_TOP_MID, 0, 10);
    
    /* Configure arc appearance */
    lv_arc_set_rotation(rpm_arc, 135);
    lv_arc_set_bg_angles(rpm_arc, 0, 270);
    lv_arc_set_value(rpm_arc, 0);
    lv_arc_set_range(rpm_arc, 0, 8000);
    
    /* Remove knob (we just want the arc indicator) */
    lv_obj_remove_style(rpm_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(rpm_arc, LV_OBJ_FLAG_CLICKABLE);
    
    /* Style the arc */
    lv_obj_set_style_arc_width(rpm_arc, 20, LV_PART_MAIN);
    lv_obj_set_style_arc_width(rpm_arc, 20, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(rpm_arc, lv_color_hex(0x303030), LV_PART_MAIN);
    lv_obj_set_style_arc_color(rpm_arc, lv_color_hex(0x00FF00), LV_PART_INDICATOR);
    
    /* Add RPM label */
    lv_obj_t *rpm_label = lv_label_create(rpm_arc);
    lv_label_set_text(rpm_label, "0 RPM");
    opendash_set_font(rpm_label, OPENDASH_FONT_SIZE_SMALL);
    lv_obj_set_style_text_color(rpm_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(rpm_label);
    
    ESP_LOGI(TAG, "RPM arc created");
}

/**
 * @brief Create a data section with label and value display.
 *
 * Each section displays a configurable data point with a label and value.
 */
static lv_obj_t* create_data_section(lv_obj_t *parent, const char *label_text, 
                                      int x, int y, int width, int height)
{
    /* Create container for this section */
    lv_obj_t *section = lv_obj_create(parent);
    lv_obj_set_size(section, width, height);
    lv_obj_set_pos(section, x, y);
    lv_obj_set_style_bg_color(section, lv_color_hex(0x202020), 0);
    lv_obj_set_style_border_color(section, lv_color_hex(0x404040), 0);
    lv_obj_set_style_border_width(section, 2, 0);
    lv_obj_set_style_radius(section, 8, 0);
    
    /* Create label */
    lv_obj_t *label = lv_label_create(section);
    lv_label_set_text(label, label_text);
    opendash_set_font(label, OPENDASH_FONT_SIZE_SMALL);
    lv_obj_set_style_text_color(label, lv_color_hex(0x808080), 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 8, 8);
    
    /* Create value label */
    lv_obj_t *value_label = lv_label_create(section);
    lv_label_set_text(value_label, "---");
    opendash_set_font(value_label, OPENDASH_FONT_SIZE_MEDIUM);
    lv_obj_set_style_text_color(value_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(value_label, LV_ALIGN_CENTER, 0, 8);
    
    return section;
}

/**
 * @brief Create the 6-section grid for data points.
 */
static void create_data_grid(lv_obj_t *parent)
{
    const int section_width = 250;
    const int section_height = 110;
    const int start_y = 140;
    const int spacing = 10;
    
    /* Calculate centered starting position */
    const int total_width = 3 * section_width + 2 * spacing;
    const int start_x = (LCD_H_RES - total_width) / 2;
    
    /* Row 1: Sections A, B, C */
    sections[0] = create_data_section(parent, "COOLANT °C", 
                                      start_x, start_y, section_width, section_height);
    sections[1] = create_data_section(parent, "GPS SPEED", 
                                      start_x + section_width + spacing, start_y, 
                                      section_width, section_height);
    sections[2] = create_data_section(parent, "BOOST kPa", 
                                      start_x + 2*(section_width + spacing), start_y, 
                                      section_width, section_height);
    
    /* Row 2: Sections D, E, F */
    sections[3] = create_data_section(parent, "OIL TEMP °C", 
                                      start_x, start_y + section_height + spacing, 
                                      section_width, section_height);
    sections[4] = create_data_section(parent, "LAP TIME", 
                                      start_x + section_width + spacing, 
                                      start_y + section_height + spacing, 
                                      section_width, section_height);
    sections[5] = create_data_section(parent, "AFR", 
                                      start_x + 2*(section_width + spacing), 
                                      start_y + section_height + spacing, 
                                      section_width, section_height);
    
    ESP_LOGI(TAG, "Data grid created (6 sections)");
}

/**
 * @brief Create the status bar at the bottom.
 */
static void create_status_bar(lv_obj_t *parent)
{
    status_bar = lv_obj_create(parent);
    lv_obj_set_size(status_bar, LCD_H_RES - 40, 50);
    lv_obj_align(status_bar, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(status_bar, lv_color_hex(0x181818), 0);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    
    /* Status text */
    lv_obj_t *status_label = lv_label_create(status_bar);
    lv_label_set_text(status_label, "OpenDash v0.1.0 | System Ready | No Warnings");
    opendash_set_font(status_label, OPENDASH_FONT_SIZE_SMALL);
    lv_obj_set_style_text_color(status_label, lv_color_hex(0x00FF00), 0);
    lv_obj_center(status_label);
    
    ESP_LOGI(TAG, "Status bar created");
}

/**
 * @brief UI rendering task.
 *
 * Continuously calls lv_timer_handler() to process LVGL events and rendering.
 */
static void ui_task(void *pvParameters)
{
    ESP_LOGI(TAG, "UI task started");
    
    while (1) {
        /* Process LVGL timers */
        lv_timer_handler();
        
        /* Delay to allow other tasks to run */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t ui_manager_init(const opendash_display_layout_t *layout)
{
    if (layout == NULL) {
        ESP_LOGE(TAG, "Layout pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    /* Save configuration */
    memcpy(&current_layout, layout, sizeof(opendash_display_layout_t));
    
    ESP_LOGI(TAG, "Creating baseline UI layout for 800×480 display");
    
    /* Create main screen */
    screen_main = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen_main, lv_color_hex(0x000000), 0);
    
    /* Create UI components */
    create_rpm_arc(screen_main);
    create_data_grid(screen_main);
    create_status_bar(screen_main);
    
    /* Load the screen */
    lv_scr_load(screen_main);
    
    ESP_LOGI(TAG, "Baseline UI layout created");
    
    return ESP_OK;
}

esp_err_t ui_manager_start(void)
{
    /* Create UI task on core 1 for smooth rendering */
    BaseType_t ret = xTaskCreatePinnedToCore(
        ui_task,
        "ui_task",
        4096,
        NULL,
        5,  /* Priority */
        &ui_task_handle,
        1   /* Core 1 */
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
    /* Future implementation: Update the appropriate widget based on data_point_id */
    ESP_LOGD(TAG, "Update requested for data point 0x%04X: %.2f", data_point_id, value);
}
