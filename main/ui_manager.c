/**
 * @file ui_manager.c
 * @brief OpenDash Center Display — UI Manager Implementation
 *
 * Enhanced version with multi-screen support and warning boxes.
 * 
 * SCREEN 1 — ENGINE METRICS:
 * ┌────────────┬─────────────────────────┬────────────┐
 * │ GPS SPEED  │                         │ LAP TIME   │
 * ├────────────┤   ARC + RPM (center)    ├────────────┤
 * │ COOLANT °C │                         │ BOOST kPa  │
 * ├────────────┤                         ├────────────┤
 * │ OIL TEMP   │                         │ AFR        │
 * └────────────┴─────────────────────────┴────────────┘
 *
 * SCREEN 2 — GPS/TELEMETRY:
 * ┌────────────┬─────────────────────────┬────────────┐
 * │ SAT COUNT  │                         │ HDOP       │
 * ├────────────┤   SPEED ARC (center)    ├────────────┤
 * │ ALTITUDE   │                         │ HEADING    │
 * ├────────────┤                         ├────────────┤
 * │ LAT/LONG   │                         │ ACCURACY   │
 * └────────────┴─────────────────────────┴────────────┘
 *
 * Features:
 * - Warning boxes with hard red/orange flashing (no transparency)
 * - Multi-screen with touch swipe or boot button navigation
 * - Smooth screen transitions
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

/* Warning box dimensions */
#define WARNING_BOX_WIDTH       60
#define WARNING_BOX_HEIGHT      180
#define WARNING_BOX_FLASH_MS    100  /* 100ms on/off for flashing */

/* ────────────────────────────────────────────────────────────────────────────
 * Display Modes & Layout Management
 * 
 * Single screen with cycling display modes:
 * - ENGINE mode: Shows RPM arc + engine parameters (coolant, oil temp, boost, afr, etc)
 * - GPS mode: Shows speed arc + GPS parameters (satellites, altitude, heading, etc)
 * 
 * All LVGL objects are created once during init. Mode changes only update labels
 * and data routing—no object creation/destruction required.
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Display mode configuration
 * 
 * Maps each mode to its label text for the 6 data sections.
 * The sections are always in this order:
 * [0] = left column, row 1   [1] = left column, row 2   [2] = left column, row 3
 * [3] = right column, row 1  [4] = right column, row 2  [5] = right column, row 3
 */
typedef struct {
    const char *section_labels[6];
    const char *status_text;
} display_mode_config_t;

/* Display mode configurations */
static const display_mode_config_t mode_configs[DISPLAY_MODE_COUNT] = {
    [DISPLAY_MODE_ENGINE] = {
        .section_labels = {"COOLANT °C", "GPS SPEED", "BOOST kPa", "OIL TEMP °C", "LAP TIME", "AFR"},
        .status_text = "MODE: ENGINE | Press boot button to switch"
    },
    [DISPLAY_MODE_GPS] = {
        .section_labels = {"ALTITUDE m", "SAT COUNT", "HEADING °", "LATITUDE", "HDOP", "ACCURACY"},
        .status_text = "MODE: GPS | Press boot button to switch"
    }
};

typedef struct {
    lv_obj_t *box;
    lv_timer_t *flash_timer;
    opendash_warning_level_t level;
    uint32_t flash_duration;
    uint32_t flash_start_time;
    bool is_flashing;
} warning_box_t;

/**
 * @brief Data section widget references
 * 
 * Stores references to all widgets within a data section so we can
 * update label text, values, and max values dynamically.
 */
typedef struct {
    lv_obj_t *section;         /* Outer container */
    lv_obj_t *label;           /* Label text (e.g., "COOLANT °C") */
    lv_obj_t *value;           /* Value display (e.g., "95") */
    lv_obj_t *max_val;         /* Max value display (e.g., "Max: 105") */
} data_section_widgets_t;

/**
 * @brief Screen layout with all widgets
 * 
 * Single screen object with all widgets pre-created during init.
 * Mode changes only update label text and don't require object recreation.
 */
typedef struct {
    lv_obj_t *screen;              /* Main screen object */
    lv_obj_t *background_img;      /* Background image (optional) */
    lv_obj_t *main_gauge;          /* Center arc (RPM or Speed) */
    lv_obj_t *gauge_value;         /* Center arc value text */
    data_section_widgets_t sections[6];  /* 6 data display sections */
    lv_obj_t *status_bar;          /* Status bar at bottom */
    lv_obj_t *status_text;         /* Status text (mode indicator) */
} screen_layout_t;

/* UI component references */
static screen_layout_t screen_layout = {0};  /* Single screen with all modes */
static display_mode_t current_mode = DISPLAY_MODE_ENGINE;
static warning_box_t warning_boxes[2] = {0};  /* 0=left, 1=right */

/* Configuration & state */
static opendash_display_layout_t current_layout;
static TaskHandle_t ui_task_handle = NULL;
static uint32_t last_swipe_time = 0;
static int16_t swipe_start_x = 0;

/**
 * @brief Create outlined text label with shadow effect
 * 
 * Creates multiple labels at offset positions to create outline effect.
 */
static lv_obj_t* create_outlined_label(lv_obj_t *parent, const char *text,
                                        opendash_font_size_t font_size,
                                        uint32_t text_color, uint32_t outline_color,
                                        int8_t outline_px)
{
    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_remove_style_all(container);
    lv_obj_set_size(container, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(container, outline_px, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    
    const int8_t offsets[4][2] = {
        {0, outline_px}, {outline_px * 2, outline_px},
        {outline_px, 0}, {outline_px, outline_px * 2}
    };
    
    for (int i = 0; i < 4; i++) {
        lv_obj_t *shadow = lv_label_create(container);
        lv_label_set_text(shadow, text);
        opendash_set_font(shadow, font_size);
        lv_obj_set_style_text_color(shadow, lv_color_hex(outline_color), 0);
        lv_obj_set_pos(shadow, offsets[i][0], offsets[i][1]);
    }
    
    lv_obj_t *main_label = lv_label_create(container);
    lv_label_set_text(main_label, text);
    opendash_set_font(main_label, font_size);
    lv_obj_set_style_text_color(main_label, lv_color_hex(text_color), 0);
    lv_obj_set_pos(main_label, outline_px, outline_px);
    
    return container;
}

/**
 * @brief Update outlined label text
 * 
 * Updates all shadow labels and main label with new text.
 * Useful for changing labels when switching display modes.
 */
static void update_outlined_label_text(lv_obj_t *label_container, const char *new_text)
{
    if (label_container == NULL || new_text == NULL) return;
    
    /* Update all child labels in the container */
    uint32_t child_count = lv_obj_get_child_count(label_container);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t *child = lv_obj_get_child(label_container, i);
        if (child != NULL && lv_obj_check_type(child, &lv_label_class)) {
            lv_label_set_text(child, new_text);
        }
    }
}

/* Layout parameters */
#define SIDE_PANEL_WIDTH    160
#define CENTER_WIDTH        (LCD_H_RES - 2 * SIDE_PANEL_WIDTH - 20)
#define STATUS_BAR_HEIGHT   50
#define SECTION_HEIGHT      130
#define SECTION_SPACING     4

/**
 * @brief Create a data section with label and value display
 * 
 * Returns a structure with references to all widgets in the section.
 * This allows us to update label text later when display mode changes.
 */
static data_section_widgets_t create_data_section(lv_obj_t *parent, const char *label_text, 
                                                   int x, int y, int width, int height)
{
    data_section_widgets_t widgets = {0};
    
    widgets.section = lv_obj_create(parent);
    lv_obj_set_size(widgets.section, width, height);
    lv_obj_set_pos(widgets.section, x, y);
    lv_obj_set_style_bg_color(widgets.section, lv_color_hex(OPENDASH_COLOR_BG_SECTION), 0);
    lv_obj_set_style_bg_opa(widgets.section, 242, 0);  /* ~95% opaque */
    lv_obj_set_style_border_color(widgets.section, lv_color_hex(OPENDASH_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(widgets.section, 3, 0);
    lv_obj_set_style_radius(widgets.section, 8, 0);
    lv_obj_set_style_pad_all(widgets.section, 0, 0);
    lv_obj_clear_flag(widgets.section, LV_OBJ_FLAG_SCROLLABLE);
    
    /* Label (e.g., "COOLANT °C") - updatable when mode changes */
    widgets.label = create_outlined_label(widgets.section, label_text,
                                          OPENDASH_FONT_SIZE_MEDIUM,
                                          OPENDASH_COLOR_TEXT_PRIMARY,
                                          OPENDASH_COLOR_TEXT_OUTLINE,
                                          1);
    lv_obj_align(widgets.label, LV_ALIGN_TOP_MID, OPENDASH_LABEL_OFFSET_X, OPENDASH_LABEL_OFFSET_Y);
    
    /* Value display (e.g., "95") */
    widgets.value = create_outlined_label(widgets.section, "---",
                                          OPENDASH_FONT_SIZE_XLARGE,
                                          OPENDASH_COLOR_TEXT_PRIMARY,
                                          OPENDASH_COLOR_TEXT_OUTLINE,
                                          1);
    lv_obj_align(widgets.value, LV_ALIGN_CENTER, 0, 25);
    
    /* Max value display (e.g., "Max: 105") */
    widgets.max_val = create_outlined_label(widgets.section, "Max: ---",
                                            OPENDASH_FONT_SIZE_MEDIUM,
                                            OPENDASH_COLOR_TEXT_PRIMARY,
                                            OPENDASH_COLOR_TEXT_OUTLINE,
                                            1);
    lv_obj_align(widgets.max_val, LV_ALIGN_BOTTOM_MID, -35, OPENDASH_VALUE_OFFSET_Y);
    
    return widgets;
}

/**
 * @brief Create the RPM arc gauge (Screen 1 - Engine)
 */
static void create_rpm_arc(lv_obj_t *parent, lv_obj_t **arc_out, lv_obj_t **value_out)
{
    const int available_height = LCD_V_RES - STATUS_BAR_HEIGHT - 20;
    const int arc_size = (available_height < CENTER_WIDTH) ? available_height : CENTER_WIDTH;
    const int arc_width = 40;
    const int outline_width = 8;
    const int arc_y_offset = -STATUS_BAR_HEIGHT / 2 + 30;
    
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
    
    lv_obj_t *rpm_arc = lv_arc_create(parent);
    lv_obj_set_size(rpm_arc, arc_size, arc_size);
    lv_obj_align(rpm_arc, LV_ALIGN_CENTER, 0, arc_y_offset);
    lv_arc_set_rotation(rpm_arc, 135);
    lv_arc_set_bg_angles(rpm_arc, 0, 270);
    lv_arc_set_value(rpm_arc, 0);
    lv_arc_set_range(rpm_arc, 0, 8000);
    lv_arc_set_mode(rpm_arc, LV_ARC_MODE_NORMAL);
    lv_obj_remove_style(rpm_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(rpm_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(rpm_arc, arc_width, LV_PART_MAIN);
    lv_obj_set_style_arc_width(rpm_arc, arc_width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(rpm_arc, lv_color_hex(OPENDASH_COLOR_RPM_BG), LV_PART_MAIN);
    lv_obj_set_style_arc_color(rpm_arc, lv_color_hex(OPENDASH_COLOR_RPM_INDICATOR), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(rpm_arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(rpm_arc, true, LV_PART_INDICATOR);
    
    lv_obj_t *rpm_value = create_outlined_label(parent, "0",
                                                 OPENDASH_FONT_SIZE_XXXLARGE,
                                                 OPENDASH_COLOR_RPM_TEXT,
                                                 OPENDASH_COLOR_RPM_OUTLINE,
                                                 3);
    lv_obj_align(rpm_value, LV_ALIGN_CENTER, 0, -STATUS_BAR_HEIGHT / 2 + 10);
    
    lv_obj_t *rpm_unit = create_outlined_label(parent, "RPM",
                                                OPENDASH_FONT_SIZE_XLARGE,
                                                OPENDASH_COLOR_TEXT_PRIMARY,
                                                OPENDASH_COLOR_TEXT_OUTLINE,
                                                4);
    lv_obj_align(rpm_unit, LV_ALIGN_CENTER, 0, arc_size / 2 - 80);
    
    *arc_out = rpm_arc;
    *value_out = rpm_value;
    ESP_LOGI(TAG, "RPM arc created");
}

/**
 * @brief Create the single screen with all data sections
 * 
 * Creates all LVGL objects once during initialization.
 * Display mode changes only update the label text in the sections.
 */
static esp_err_t create_screen_layout(void)
{
    screen_layout = (screen_layout_t){0};
    
    /* Create main screen */
    screen_layout.screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen_layout.screen, lv_color_hex(0x000000), 0);
    
    /* Add optional background image */
#if HAS_BACKGROUND_IMAGE
    screen_layout.background_img = lv_image_create(screen_layout.screen);
    lv_image_set_src(screen_layout.background_img, &background_center_dsc);
    lv_obj_set_size(screen_layout.background_img, LCD_H_RES, LCD_V_RES);
    lv_obj_align(screen_layout.background_img, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_img_opa(screen_layout.background_img, 76, 0);
    lv_obj_move_to_index(screen_layout.background_img, 0);
#endif
    
    /* Create center arc (RPM/Speed arc) */
    create_rpm_arc(screen_layout.screen, &screen_layout.main_gauge, &screen_layout.gauge_value);
    
    /* Layout parameters */
    const int section_width = SIDE_PANEL_WIDTH;
    const int section_height = SECTION_HEIGHT;
    const int spacing = SECTION_SPACING;
    const int start_y = 20;
    const int left_x = 5;
    const int right_x = LCD_H_RES - section_width - 5;
    
    /* Create all 6 data sections (will use placeholder labels for now) */
    /* Row 0: Section 0 (left), Section 3 (right) */
    screen_layout.sections[0] = create_data_section(screen_layout.screen, "PLACEHOLDER",
                                                     left_x, start_y, section_width, section_height);
    screen_layout.sections[3] = create_data_section(screen_layout.screen, "PLACEHOLDER",
                                                     right_x, start_y, section_width, section_height);
    
    /* Row 1: Section 1 (left), Section 4 (right) */
    screen_layout.sections[1] = create_data_section(screen_layout.screen, "PLACEHOLDER",
                                                     left_x, start_y + section_height + spacing,
                                                     section_width, section_height);
    screen_layout.sections[4] = create_data_section(screen_layout.screen, "PLACEHOLDER",
                                                     right_x, start_y + section_height + spacing,
                                                     section_width, section_height);
    
    /* Row 2: Section 2 (left), Section 5 (right) */
    screen_layout.sections[2] = create_data_section(screen_layout.screen, "PLACEHOLDER",
                                                     left_x, start_y + 2 * (section_height + spacing),
                                                     section_width, section_height);
    screen_layout.sections[5] = create_data_section(screen_layout.screen, "PLACEHOLDER",
                                                     right_x, start_y + 2 * (section_height + spacing),
                                                     section_width, section_height);
    
    /* Create status bar */
    screen_layout.status_bar = lv_obj_create(screen_layout.screen);
    lv_obj_set_size(screen_layout.status_bar, LCD_H_RES - 40, 50);
    lv_obj_align(screen_layout.status_bar, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(screen_layout.status_bar, lv_color_hex(OPENDASH_COLOR_BG_STATUSBAR), 0);
    lv_obj_set_style_bg_opa(screen_layout.status_bar, 242, 0);
    lv_obj_set_style_border_color(screen_layout.status_bar, lv_color_hex(OPENDASH_COLOR_BORDER_STATUS), 0);
    lv_obj_set_style_border_width(screen_layout.status_bar, 3, 0);
    lv_obj_set_style_radius(screen_layout.status_bar, 8, 0);
    lv_obj_clear_flag(screen_layout.status_bar, LV_OBJ_FLAG_SCROLLABLE);
    
    screen_layout.status_text = create_outlined_label(screen_layout.status_bar,
                                                       "Initializing...",
                                                       OPENDASH_FONT_SIZE_MEDIUM,
                                                       OPENDASH_COLOR_TEXT_PRIMARY,
                                                       OPENDASH_COLOR_TEXT_OUTLINE,
                                                       1);
    lv_obj_center(screen_layout.status_text);
    
    ESP_LOGI(TAG, "Screen layout created with 6 data sections + center arc");
    return ESP_OK;
}

/**
 * @brief Update display mode and refresh all labels
 * 
 * Changes the current display mode and updates all section labels
 * to match the new mode's configuration.
 */
static void update_display_mode(display_mode_t new_mode)
{
    current_mode = new_mode;
    const display_mode_config_t *config = &mode_configs[current_mode];
    
    /* Update label text in all 6 sections */
    for (int i = 0; i < 6; i++) {
        update_outlined_label_text(screen_layout.sections[i].label, config->section_labels[i]);
    }
    
    /* Update status text */
    update_outlined_label_text(screen_layout.status_text, config->status_text);
    
    ESP_LOGI(TAG, "Display mode changed to %d", current_mode);
}

/**
 * @brief Flash animation timer callback for warning boxes
 */
static void warning_flash_timer_cb(lv_timer_t *timer)
{
    warning_box_t *warning = (warning_box_t *)lv_timer_get_user_data(timer);
    
    if (warning->box == NULL) {
        lv_timer_delete(warning->flash_timer);
        warning->flash_timer = NULL;
        return;
    }
    
    uint32_t elapsed = esp_log_timestamp() - warning->flash_start_time;
    
    /* Check if flash duration expired (0 = continuous) */
    if (warning->flash_duration > 0 && elapsed > warning->flash_duration) {
        ui_manager_warning_box_clear(warning == &warning_boxes[0] ? 0 : 1);
        return;
    }
    
    /* Toggle visibility every flash period */
    bool visible = ((elapsed / WARNING_BOX_FLASH_MS) % 2) == 0;
    if (visible) {
        lv_obj_remove_flag(warning->box, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(warning->box, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief Create warning box on specified side
 */
static esp_err_t create_warning_box(uint8_t position)
{
    if (position > 1) return ESP_ERR_INVALID_ARG;
    
    warning_box_t *warning = &warning_boxes[position];
    
    if (warning->box != NULL) {
        lv_obj_del(warning->box);
        warning->box = NULL;
    }
    
    /* Create box container */
    warning->box = lv_obj_create(screen_layout.screen);
    lv_obj_set_size(warning->box, WARNING_BOX_WIDTH, WARNING_BOX_HEIGHT);
    
    /* Position: left or right side */
    if (position == 0) {  /* Left */
        lv_obj_align(warning->box, LV_ALIGN_LEFT_MID, 5, 0);
    } else {  /* Right */
        lv_obj_align(warning->box, LV_ALIGN_RIGHT_MID, -5, 0);
    }
    
    /* Hard solid color with 100% opacity (no bleed through) */
    uint32_t box_color = (warning->level == OPENDASH_WARNING_CRITICAL) 
        ? OPENDASH_COLOR_WARNING_BOX_RED 
        : OPENDASH_COLOR_WARNING_BOX_ORANGE;
    
    lv_obj_set_style_bg_color(warning->box, lv_color_hex(box_color), 0);
    lv_obj_set_style_bg_opa(warning->box, 255, 0);  /* 100% opaque - NO bleed through */
    lv_obj_set_style_border_width(warning->box, 0, 0);  /* No border */
    lv_obj_set_style_radius(warning->box, 4, 0);
    lv_obj_clear_flag(warning->box, LV_OBJ_FLAG_SCROLLABLE);
    
    return ESP_OK;
}

/**
 * @brief Touch event handler for screen transition (simplified)
 * 
 * For now, this is a placeholder. Touch input will be enhanced
 * when GT911 driver is fully implemented.
 */
static void touch_event_handler(lv_event_t *e)
{
    (void)e;  /* Unused for now */
}

/**
 * @brief UI rendering task
 */
static void ui_task(void *pvParameters)
{
    ESP_LOGI(TAG, "UI task started");
    
    while (1) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * Public API Implementation
 * ──────────────────────────────────────────────────────────────────────────── */

esp_err_t ui_manager_init(const opendash_display_layout_t *layout)
{
    if (layout == NULL) {
        ESP_LOGE(TAG, "Layout pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    memcpy(&current_layout, layout, sizeof(opendash_display_layout_t));
    
    ESP_LOGI(TAG, "Creating single-screen UI with cycling display modes");
    
    /* Create the single screen with all LVGL objects */
    ESP_ERROR_CHECK(create_screen_layout());
    
    /* Set initial display mode and load screen */
    update_display_mode(DISPLAY_MODE_ENGINE);
    lv_scr_load(screen_layout.screen);
    
    ESP_LOGI(TAG, "Display initialized - press boot button to cycle between display modes");
    ESP_LOGI(TAG, "Modes: %s", "ENGINE, GPS (enable more with DISPLAY_MODE_COUNT)");
    
    return ESP_OK;
}

esp_err_t ui_manager_start(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(
        ui_task,
        "ui_task",
        8192,
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

esp_err_t ui_manager_warning_box_trigger(uint8_t position, 
                                          opendash_warning_level_t level,
                                          const char *message,
                                          uint32_t flash_ms)
{
    if (position > 1) return ESP_ERR_INVALID_ARG;
    
    warning_box_t *warning = &warning_boxes[position];
    warning->level = level;
    warning->flash_duration = flash_ms;
    warning->flash_start_time = esp_log_timestamp();
    warning->is_flashing = true;
    
    create_warning_box(position);
    
    /* Start flash timer if not already running */
    if (warning->flash_timer == NULL) {
        warning->flash_timer = lv_timer_create(warning_flash_timer_cb, WARNING_BOX_FLASH_MS, warning);
    }
    
    ESP_LOGI(TAG, "Warning box triggered on position %d, level %d", position, level);
    return ESP_OK;
}

esp_err_t ui_manager_warning_box_clear(uint8_t position)
{
    if (position > 1) return ESP_ERR_INVALID_ARG;
    
    warning_box_t *warning = &warning_boxes[position];
    
    if (warning->flash_timer != NULL) {
        lv_timer_delete(warning->flash_timer);
        warning->flash_timer = NULL;
    }
    
    if (warning->box != NULL) {
        lv_obj_del(warning->box);
        warning->box = NULL;
    }
    
    warning->is_flashing = false;
    
    ESP_LOGI(TAG, "Warning box cleared on position %d", position);
    return ESP_OK;
}

esp_err_t ui_manager_next_screen(void)
{
    display_mode_t next_mode = (current_mode + 1) % DISPLAY_MODE_COUNT;
    update_display_mode(next_mode);
    return ESP_OK;
}

uint8_t ui_manager_get_current_screen(void)
{
    /* Return current display mode as screen index */
    return (uint8_t)current_mode;
}

esp_err_t ui_manager_set_display_mode(display_mode_t mode)
{
    if (mode >= DISPLAY_MODE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    update_display_mode(mode);
    return ESP_OK;
}
