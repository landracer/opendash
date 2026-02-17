/**
 * @file ui_manager.c
 * @brief OpenDash Center Display — UI Manager Implementation
 *
 * Creates a professional baseline layout for the center display (800×480):
 * 
 * Layout (3-column with centered ARC/RPM):
 * ┌────────────┬─────────────────────────┬────────────┐
 * │ GPS SPEED  │                         │ LAP TIME   │
 * ├────────────┤                         ├────────────┤
 * │ COOLANT °C │   ARC + RPM (center)    │ BOOST kPa  │
 * ├────────────┤                         ├────────────┤
 * │ OIL TEMP   │                         │ AFR        │
 * ├────────────┴─────────────────────────┴────────────┤
 * │  Status Bar — Warnings, Alarms, Checklist Status  │
 * └───────────────────────────────────────────────────┘
 *
 * - Narrow side panels (160px each) with 3 stacked sections
 * - Large centered ARC + RPM gauge filling center area
 * - 90% opacity on sections (10% bleed through)
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
#include "opendash_ui_styles.h"
#include <string.h>

/* Include background image if available */
#if __has_include("background_center.h")
#include "background_center.h"
#define HAS_BACKGROUND_IMAGE 1
#else
#define HAS_BACKGROUND_IMAGE 0
#endif

static const char *TAG = "ui_manager";

/* Display dimensions */
#define LCD_H_RES   800
#define LCD_V_RES   480

/* UI component references */
static lv_obj_t *screen_main = NULL;
static lv_obj_t *background_img = NULL;
static lv_obj_t *rpm_arc = NULL;
static lv_obj_t *rpm_value_container = NULL;  /* For outlined RPM text */
static lv_obj_t *sections[6] = {NULL};  /* A through F */
static lv_obj_t *status_bar = NULL;

/* Configuration */
static opendash_display_layout_t current_layout;

/* UI task handle */
static TaskHandle_t ui_task_handle = NULL;

/**
 * @brief Create outlined text label with shadow effect
 * 
 * Creates multiple labels at offset positions to create outline effect.
 * 
 * @param parent Parent object
 * @param text Text to display
 * @param font_size Font size
 * @param text_color Main text color
 * @param outline_color Outline color
 * @param outline_px Outline thickness in pixels
 * @return Container object holding the outlined text
 */
static lv_obj_t* create_outlined_label(lv_obj_t *parent, const char *text,
                                        opendash_font_size_t font_size,
                                        uint32_t text_color, uint32_t outline_color,
                                        int8_t outline_px)
{
    /* Create container for all the labels */
    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_remove_style_all(container);
    lv_obj_set_size(container, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    
    /* Offset directions for outline: 8 directions */
    const int8_t offsets[8][2] = {
        {-outline_px, 0}, {outline_px, 0},  /* Left, Right */
        {0, -outline_px}, {0, outline_px},  /* Up, Down */
        {-outline_px, -outline_px}, {outline_px, -outline_px},  /* Diagonals */
        {-outline_px, outline_px}, {outline_px, outline_px}
    };
    
    /* Create outline/shadow labels first (behind) */
    for (int i = 0; i < 8; i++) {
        lv_obj_t *shadow = lv_label_create(container);
        lv_label_set_text(shadow, text);
        opendash_set_font(shadow, font_size);
        lv_obj_set_style_text_color(shadow, lv_color_hex(outline_color), 0);
        lv_obj_set_pos(shadow, offsets[i][0], offsets[i][1]);
    }
    
    /* Create main text on top */
    lv_obj_t *main_label = lv_label_create(container);
    lv_label_set_text(main_label, text);
    opendash_set_font(main_label, font_size);
    lv_obj_set_style_text_color(main_label, lv_color_hex(text_color), 0);
    lv_obj_set_pos(main_label, 0, 0);
    
    return container;
}

/* Side panel dimensions for 3-column layout */
#define SIDE_PANEL_WIDTH    160   /* Narrower side panels */
#define CENTER_WIDTH        (LCD_H_RES - 2 * SIDE_PANEL_WIDTH - 20)  /* ~460px center */
#define STATUS_BAR_HEIGHT   50
#define SECTION_HEIGHT      130   /* Height for each stacked section */
#define SECTION_SPACING     4     /* Reduced spacing between sections */

/**
 * @brief Create the RPM arc gauge centered on the display.
 *
 * This creates a full-height circular arc centered between the side panels.
 * RPM value displayed with large font, black text with white outline.
 */
static void create_rpm_arc(lv_obj_t *parent)
{
    /* Calculate center area dimensions */
    const int available_height = LCD_V_RES - STATUS_BAR_HEIGHT - 20;  /* ~410px */
    const int arc_size = (available_height < CENTER_WIDTH) ? available_height : CENTER_WIDTH;  /* Use smaller dimension */
    const int arc_width = 40;       /* Main arc thickness */
    const int outline_width = 4;    /* Black outline thickness on each side */
    const int arc_y_offset = -STATUS_BAR_HEIGHT / 2 + 30;  /* Lowered position */
    
    /* Create OUTER BLACK OUTLINE arc (larger, behind main arc) */
    lv_obj_t *outer_outline = lv_arc_create(parent);
    lv_obj_set_size(outer_outline, arc_size + (outline_width * 2), arc_size + (outline_width * 2));
    lv_obj_align(outer_outline, LV_ALIGN_CENTER, 0, arc_y_offset);
    lv_arc_set_rotation(outer_outline, 135);
    lv_arc_set_bg_angles(outer_outline, 0, 270);
    lv_arc_set_value(outer_outline, 0);
    lv_arc_set_range(outer_outline, 0, 100);
    lv_obj_remove_style(outer_outline, NULL, LV_PART_KNOB);
    lv_obj_remove_style(outer_outline, NULL, LV_PART_INDICATOR);
    lv_obj_clear_flag(outer_outline, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(outer_outline, arc_width + (outline_width * 2), LV_PART_MAIN);
    lv_obj_set_style_arc_color(outer_outline, lv_color_hex(OPENDASH_COLOR_RPM_BORDER), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(outer_outline, true, LV_PART_MAIN);
    
    /* Create MAIN RPM arc on top */
    rpm_arc = lv_arc_create(parent);
    lv_obj_set_size(rpm_arc, arc_size, arc_size);
    lv_obj_align(rpm_arc, LV_ALIGN_CENTER, 0, arc_y_offset);
    
    /* Configure arc as a 270-degree sweep (typical tachometer style) */
    lv_arc_set_rotation(rpm_arc, 135);      /* Start from lower-left */
    lv_arc_set_bg_angles(rpm_arc, 0, 270);  /* 270 degree arc */
    lv_arc_set_value(rpm_arc, 0);
    lv_arc_set_range(rpm_arc, 0, 8000);
    lv_arc_set_mode(rpm_arc, LV_ARC_MODE_NORMAL);
    
    /* Remove knob (we just want the arc indicator) */
    lv_obj_remove_style(rpm_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(rpm_arc, LV_OBJ_FLAG_CLICKABLE);
    
    /* Style the main arc - dark red bg, white indicator */
    lv_obj_set_style_arc_width(rpm_arc, arc_width, LV_PART_MAIN);
    lv_obj_set_style_arc_width(rpm_arc, arc_width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(rpm_arc, lv_color_hex(OPENDASH_COLOR_RPM_BG), LV_PART_MAIN);
    lv_obj_set_style_arc_color(rpm_arc, lv_color_hex(OPENDASH_COLOR_RPM_INDICATOR), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(rpm_arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(rpm_arc, true, LV_PART_INDICATOR);
    
    /* Create RPM value with outlined text - XXLARGE font for big display */
    rpm_value_container = create_outlined_label(parent, "0",
                                                 OPENDASH_FONT_SIZE_XXLARGE,
                                                 OPENDASH_COLOR_RPM_TEXT,
                                                 OPENDASH_COLOR_RPM_OUTLINE,
                                                 3);  /* Thicker outline for large font */
    lv_obj_align(rpm_value_container, LV_ALIGN_CENTER, 0, -STATUS_BAR_HEIGHT / 2 + 10);  /* Lowered with arc */
    
    /* Add "RPM" unit label below the number - white text, black outline */
    lv_obj_t *rpm_unit = create_outlined_label(parent, "RPM",
                                                OPENDASH_FONT_SIZE_MEDIUM,
                                                OPENDASH_COLOR_TEXT_PRIMARY,
                                                OPENDASH_COLOR_TEXT_OUTLINE,
                                                1);
    lv_obj_align(rpm_unit, LV_ALIGN_CENTER, 0, -STATUS_BAR_HEIGHT / 2 + 80);  /* Lowered with arc */
    
    ESP_LOGI(TAG, "RPM arc created (centered, full-height, outlined text)");
}

/**
 * @brief Create a data section with label and value display.
 *
 * Each section displays a configurable data point with:
 * - Label centered horizontally, hugging the top edge
 * - Value centered horizontally, hugging the bottom edge
 */
static lv_obj_t* create_data_section(lv_obj_t *parent, const char *label_text, 
                                      int x, int y, int width, int height)
{
    /* Create container for this section */
    lv_obj_t *section = lv_obj_create(parent);
    lv_obj_set_size(section, width, height);
    lv_obj_set_pos(section, x, y);
    lv_obj_set_style_bg_color(section, lv_color_hex(OPENDASH_COLOR_BG_SECTION), 0);
    lv_obj_set_style_bg_opa(section, 242, 0);  /* ~95% opaque - very low bleed through */
    lv_obj_set_style_border_color(section, lv_color_hex(OPENDASH_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(section, 3, 0);  /* Thicker black outline */
    lv_obj_set_style_radius(section, 8, 0);
    lv_obj_set_style_pad_all(section, 0, 0);  /* Remove default padding - let text hug edges */
    lv_obj_clear_flag(section, LV_OBJ_FLAG_SCROLLABLE);
    
    /* Create label - CENTERED, hugging TOP edge */
    lv_obj_t *label_outlined = create_outlined_label(section, label_text,
                                                      OPENDASH_FONT_SIZE_SMALL,
                                                      OPENDASH_COLOR_TEXT_PRIMARY,
                                                      OPENDASH_COLOR_TEXT_OUTLINE,
                                                      1);
    lv_obj_align(label_outlined, LV_ALIGN_TOP_MID, OPENDASH_LABEL_OFFSET_X, OPENDASH_LABEL_OFFSET_Y);
    
    /* Create value label - CENTERED, hugging BOTTOM edge */
    lv_obj_t *value_outlined = create_outlined_label(section, "---",
                                                      OPENDASH_FONT_SIZE_MEDIUM,
                                                      OPENDASH_COLOR_TEXT_PRIMARY,
                                                      OPENDASH_COLOR_TEXT_OUTLINE,
                                                      1);
    lv_obj_align(value_outlined, LV_ALIGN_BOTTOM_MID, OPENDASH_VALUE_OFFSET_X, OPENDASH_VALUE_OFFSET_Y);
    
    return section;
}

/**
 * @brief Create the 6-section grid for data points in left/right columns.
 * 
 * New layout:
 * ┌───────────┬─────────────────────┬───────────┐
 * │ GPS SPEED │                     │ LAP TIME  │
 * │───────────│   ARC + RPM         │───────────│
 * │ COOLANT   │    (center)         │ BOOST     │
 * │───────────│                     │───────────│
 * │ OIL TEMP  │                     │ AFR       │
 * └───────────┴─────────────────────┴───────────┘
 */
static void create_data_grid(lv_obj_t *parent)
{
    const int section_width = SIDE_PANEL_WIDTH;
    const int section_height = SECTION_HEIGHT;
    const int spacing = SECTION_SPACING;
    const int start_y = 20;  /* Top margin - lowered to prevent bleed-over */
    const int left_x = 5;    /* Left margin */
    const int right_x = LCD_H_RES - section_width - 5;  /* Right side position */
    
    /* Left Column (top to bottom): GPS SPEED, COOLANT, OIL TEMP */
    sections[1] = create_data_section(parent, "GPS SPEED", 
                                      left_x, start_y, 
                                      section_width, section_height);
    sections[0] = create_data_section(parent, "COOLANT °C", 
                                      left_x, start_y + section_height + spacing, 
                                      section_width, section_height);
    sections[3] = create_data_section(parent, "OIL TEMP °C", 
                                      left_x, start_y + 2 * (section_height + spacing), 
                                      section_width, section_height);
    
    /* Right Column (top to bottom): LAP TIME, BOOST, AFR */
    sections[4] = create_data_section(parent, "LAP TIME", 
                                      right_x, start_y, 
                                      section_width, section_height);
    sections[2] = create_data_section(parent, "BOOST kPa", 
                                      right_x, start_y + section_height + spacing, 
                                      section_width, section_height);
    sections[5] = create_data_section(parent, "AFR", 
                                      right_x, start_y + 2 * (section_height + spacing), 
                                      section_width, section_height);
    
    ESP_LOGI(TAG, "Data grid created (left/right columns, center open for ARC)");
}

/**
 * @brief Create the status bar at the bottom.
 * 
 * Status bar displays system status and warnings.
 * Normal: white text with black outline
 * Warnings: RED text with WHITE outline, LARGE font
 */
static void create_status_bar(lv_obj_t *parent)
{
    status_bar = lv_obj_create(parent);
    lv_obj_set_size(status_bar, LCD_H_RES - 40, 50);
    lv_obj_align(status_bar, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(status_bar, lv_color_hex(OPENDASH_COLOR_BG_STATUSBAR), 0);
    lv_obj_set_style_bg_opa(status_bar, 242, 0);  /* ~95% opaque - match section opacity */
    lv_obj_set_style_border_color(status_bar, lv_color_hex(OPENDASH_COLOR_BORDER_STATUS), 0);
    lv_obj_set_style_border_width(status_bar, 3, 0);  /* Thicker white outline */
    lv_obj_set_style_radius(status_bar, 8, 0);
    lv_obj_clear_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);
    
    /* Status text - white text with black outline */
    lv_obj_t *status_outlined = create_outlined_label(status_bar,
                                                       "OpenDash v0.1.0 | System Ready | No Warnings",
                                                       OPENDASH_FONT_SIZE_SMALL,
                                                       OPENDASH_COLOR_TEXT_PRIMARY,
                                                       OPENDASH_COLOR_TEXT_OUTLINE,
                                                       1);
    lv_obj_center(status_outlined);
    
    ESP_LOGI(TAG, "Status bar created");
}

/**
 * @brief Update status bar with warning state.
 * 
 * When in warning state, recreates with large RED text with WHITE outline.
 * 
 * @param warning_text Warning message (NULL for normal state)
 */
void ui_manager_set_warning(const char *warning_text)
{
    if (status_bar == NULL) return;
    
    /* Remove existing content */
    lv_obj_clean(status_bar);
    
    if (warning_text != NULL && strlen(warning_text) > 0) {
        /* Warning state: large RED text with WHITE outline */
        lv_obj_t *warning_outlined = create_outlined_label(status_bar,
                                                            warning_text,
                                                            OPENDASH_FONT_SIZE_LARGE,
                                                            OPENDASH_COLOR_WARNING_TEXT,
                                                            OPENDASH_COLOR_WARNING_OUTLINE,
                                                            2);
        lv_obj_center(warning_outlined);
    } else {
        /* Normal state: regular size, white text with black outline */
        lv_obj_t *status_outlined = create_outlined_label(status_bar,
                                                           "OpenDash v0.1.0 | System Ready | No Warnings",
                                                           OPENDASH_FONT_SIZE_SMALL,
                                                           OPENDASH_COLOR_TEXT_PRIMARY,
                                                           OPENDASH_COLOR_TEXT_OUTLINE,
                                                           1);
        lv_obj_center(status_outlined);
    }
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
    
#if HAS_BACKGROUND_IMAGE
    /* Add background image */
    background_img = lv_img_create(screen_main);
    lv_img_set_src(background_img, &background_center_dsc);
    lv_obj_set_size(background_img, LCD_H_RES, LCD_V_RES);
    lv_obj_align(background_img, LV_ALIGN_CENTER, 0, 0);
    lv_obj_move_to_index(background_img, 0);  /* Send to back */
    ESP_LOGI(TAG, "Background image loaded");
#else
    ESP_LOGW(TAG, "No background image available");
#endif
    
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
        8192,
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
