/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
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
 * │ BATTERY V  │                         │ GPS FIX    │
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
#include "display_init.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "opendash_common.h"
#include "opendash_data_model.h"
#include "opendash_fonts.h"
#include "opendash_ui_styles.h"
#include "opendash_uart.h"
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

/* Display mode configurations — American units (PSI, °F, MPH, ft) */
static const display_mode_config_t mode_configs[DISPLAY_MODE_COUNT] = {
    [DISPLAY_MODE_ENGINE] = {
        .section_labels = {"COOLANT \xc2\xb0""F", "GPS MPH", "BOOST PSI", "OIL TEMP \xc2\xb0""F", "LAP TIME", "AFR"},
        .status_text = "MODE: ENGINE | Swipe or boot button to switch"
    },
    [DISPLAY_MODE_GPS] = {
        .section_labels = {"ALTITUDE ft", "SAT COUNT", "HEADING \xc2\xb0", "BATTERY V", "HDOP", "GPS FIX"},
        .status_text = "MODE: GPS | Swipe or boot button to switch"
    },
    [DISPLAY_MODE_MD] = {
        .section_labels = {"EGT 1 \xc2\xb0""F", "EGT 2 \xc2\xb0""F", "O2 / Lambda", "EGT 3 \xc2\xb0""F", "EGT 4 \xc2\xb0""F", "MAS (LMM)"},
        .status_text = "MODE: MULTIDISPLAY | " OPENDASH_MD_BT_NAME
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
    lv_obj_t *gauge_minmax;        /* Min/Max label below arc value */
    lv_obj_t *shift_light_bar;     /* Shift light indicator bar (blinks at redline) */
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

/* Center arc min/max tracking (per display mode) */
static float s_arc_min_val[DISPLAY_MODE_COUNT];
static float s_arc_max_val[DISPLAY_MODE_COUNT];
static bool  s_arc_has_data[DISPLAY_MODE_COUNT];

/* ── Shift-light (RPM Red) ──────────────────────────────────
 * When the RPM arc exceeds 90% (7200 RPM on 0-8000 range),
 * the arc indicator + RPM number blink red/blue at 150ms.
 * ──────────────────────────────────────────────────────────── */
#define SHIFT_BLINK_THRESHOLD   90      /* Arc % above which shift-light blinks */
#define SHIFT_BLINK_INTERVAL_MS 150     /* On/off toggle interval (ms) */

static bool     s_shift_active = false;
static uint32_t s_shift_last_toggle_ms = 0;
static bool     s_shift_blink_on = false;

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
    
    /* 8 shadow copies at all cardinal + diagonal offsets for
     * a clean, gap-free outline around every letter.             */
    const int8_t offsets[8][2] = {
        {0, outline_px},              /* top              */
        {outline_px * 2, outline_px}, /* bottom            */
        {outline_px, 0},              /* left              */
        {outline_px, outline_px * 2}, /* right             */
        {0, 0},                       /* top-left          */
        {outline_px * 2, 0},          /* top-right         */
        {0, outline_px * 2},          /* bottom-left       */
        {outline_px * 2, outline_px * 2}, /* bottom-right  */
    };
    
    for (int i = 0; i < 8; i++) {
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
 * Used for STATIC labels (section headers, status) that change rarely.
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

/**
 * @brief Update outlined label colors (text + outline)
 * 
 * Children 0-7 are shadow/outline labels, child 8 (last) is the main text.
 */
static void update_outlined_label_color(lv_obj_t *label_container,
                                         uint32_t text_color, uint32_t outline_color)
{
    if (label_container == NULL) return;
    uint32_t child_count = lv_obj_get_child_count(label_container);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t *child = lv_obj_get_child(label_container, i);
        if (child == NULL || !lv_obj_check_type(child, &lv_label_class)) continue;
        if (i < child_count - 1) {
            /* Shadow/outline label */
            lv_obj_set_style_text_color(child, lv_color_hex(outline_color), 0);
        } else {
            /* Main text label (last child) */
            lv_obj_set_style_text_color(child, lv_color_hex(text_color), 0);
        }
    }
}

/**
 * @brief Create a simple label (no outline) for dynamic values.
 *
 * Used for values/max that update frequently (RPM, speed, etc.)
 * to avoid the 5× overhead of outlined labels.
 */
static lv_obj_t* create_simple_label(lv_obj_t *parent, const char *text,
                                      opendash_font_size_t font_size,
                                      uint32_t text_color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    opendash_set_font(label, font_size);
    lv_obj_set_style_text_color(label, lv_color_hex(text_color), 0);
    return label;
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
    lv_obj_set_style_border_color(widgets.section, lv_color_hex(0xFFFFFF), 0);
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
    
    /* Value display (e.g., "95") — simple label for fast updates */
    widgets.value = create_simple_label(widgets.section, "---",
                                        OPENDASH_FONT_SIZE_XLARGE,
                                        OPENDASH_COLOR_TEXT_PRIMARY);
    lv_obj_align(widgets.value, LV_ALIGN_CENTER, 0, 5);
    
    /* Max value display (e.g., "Max: 105") — simple label for fast updates */
    widgets.max_val = create_simple_label(widgets.section, "Max: ---",
                                          OPENDASH_FONT_SIZE_MEDIUM,
                                          OPENDASH_COLOR_TEXT_PRIMARY);
    lv_obj_align(widgets.max_val, LV_ALIGN_BOTTOM_MID, 0, OPENDASH_VALUE_OFFSET_Y);
    
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
    
    /* RPM/Speed value — outlined label: white text, black outline */
    lv_obj_t *rpm_value = create_outlined_label(parent, "0",
                                               OPENDASH_FONT_SIZE_XXXLARGE,
                                               0xFFFFFF,   /* White fill */
                                               0x000000,   /* Black outline */
                                               2);
    lv_obj_align(rpm_value, LV_ALIGN_CENTER, 0, -STATUS_BAR_HEIGHT / 2 + 10);
    
    lv_obj_t *rpm_unit = create_outlined_label(parent, "RPM",
                                                OPENDASH_FONT_SIZE_XLARGE,
                                                OPENDASH_COLOR_TEXT_PRIMARY,
                                                OPENDASH_COLOR_TEXT_OUTLINE,
                                                4);
    lv_obj_align(rpm_unit, LV_ALIGN_CENTER, 0, arc_size / 2 - 80);

    /* Min/Max label below the RPM value */
    screen_layout.gauge_minmax = create_outlined_label(parent, "",
                                                        OPENDASH_FONT_SIZE_SMALL,
                                                        0xCCCCCC,   /* Light gray */
                                                        0x000000,   /* Black outline */
                                                        1);
    lv_obj_align(screen_layout.gauge_minmax, LV_ALIGN_CENTER, 0, -STATUS_BAR_HEIGHT / 2 + 75);
    
    *arc_out = rpm_arc;
    *value_out = rpm_value;

    /* Shift light bar — thin horizontal bar across the top of the gauge area.
     * Blinks red at high RPM instead of changing the arc indicator color,
     * which avoids the expensive anti-aliased arc redraw. */
    lv_obj_t *shift_bar = lv_obj_create(parent);
    lv_obj_set_size(shift_bar, arc_size + 40, 12);
    lv_obj_align(shift_bar, LV_ALIGN_CENTER, 0, arc_y_offset - arc_size / 2 - 20);
    lv_obj_set_style_bg_color(shift_bar, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_bg_opa(shift_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(shift_bar, 6, 0);
    lv_obj_set_style_border_width(shift_bar, 0, 0);
    lv_obj_set_style_pad_all(shift_bar, 0, 0);
    lv_obj_add_flag(shift_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(shift_bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    screen_layout.shift_light_bar = shift_bar;

    ESP_LOGI(TAG, "RPM arc created (with min/max + shift light)");
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
    lv_obj_clear_flag(screen_layout.screen, LV_OBJ_FLAG_SCROLLABLE);  /* Prevent layout shifting */
    lv_obj_add_flag(screen_layout.screen, LV_OBJ_FLAG_CLICKABLE);     /* Required for touch events in LVGL 9 */
    
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
    lv_obj_set_style_border_color(screen_layout.status_bar, lv_color_hex(0xFFFFFF), 0);
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
    
    /* Update label text in all 6 sections and reset values */
    for (int i = 0; i < 6; i++) {
        update_outlined_label_text(screen_layout.sections[i].label, config->section_labels[i]);
        if (screen_layout.sections[i].value) {
            lv_label_set_text(screen_layout.sections[i].value, "---");
        }
        if (screen_layout.sections[i].max_val) {
            lv_label_set_text(screen_layout.sections[i].max_val, "");
        }
    }
    
    /* Reset center arc */
    if (screen_layout.main_gauge) {
        lv_arc_set_value(screen_layout.main_gauge, 0);
    }
    if (screen_layout.gauge_value) {
        update_outlined_label_text(screen_layout.gauge_value, "0");
    }
    if (screen_layout.gauge_minmax) {
        update_outlined_label_text(screen_layout.gauge_minmax, "");
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
 * @brief Touch event handler for screen transition via horizontal swipe.
 */
static void touch_event_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        lv_point_t point;
        lv_indev_get_point(lv_indev_active(), &point);
        swipe_start_x = point.x;
    }
    else if (code == LV_EVENT_RELEASED) {
        uint32_t now = esp_log_timestamp();
        if (now - last_swipe_time < 500) return;  /* Debounce */

        lv_point_t point;
        lv_indev_get_point(lv_indev_active(), &point);
        int16_t dx = point.x - swipe_start_x;

        if (dx > 80 || dx < -80) {
            last_swipe_time = now;
            display_mode_t next_mode = (current_mode + 1) % DISPLAY_MODE_COUNT;
            update_display_mode(next_mode);
            ESP_LOGI(TAG, "Swipe detected (dx=%d) → mode %d", dx, next_mode);
        }
    }
}

/**
 * @brief UI rendering task
 */
static void ui_task(void *pvParameters)
{
    ESP_LOGI(TAG, "UI task started");
    
    while (1) {
        /* LVGL is NOT thread-safe.  All API calls (including
         * lv_timer_handler which renders & processes events) must
         * be serialised with the same mutex used by espnow_master
         * and any other task that touches LVGL objects.            */
        if (display_lvgl_lock(100)) {
            lv_timer_handler();
            display_lvgl_unlock();
        }
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

    /* Register swipe touch event on the screen */
    lv_obj_add_event_cb(screen_layout.screen, touch_event_handler, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(screen_layout.screen, touch_event_handler, LV_EVENT_RELEASED, NULL);

    ESP_LOGI(TAG, "Display initialized - swipe or boot button to cycle modes");
    ESP_LOGI(TAG, "Modes: ENGINE, GPS, MULTIDISPLAY (%d total)", DISPLAY_MODE_COUNT);
    
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

/* ────────────────────────────────────────────────────────────────────────────
 * Data Point → Section Mapping
 *
 * Each display mode maps specific data point IDs to the 6 section slots
 * and the center arc.  When a value arrives, we check if it belongs to
 * the current mode and update the matching widget.
 * ──────────────────────────────────────────────────────────────────────────── */

typedef struct {
    uint16_t section_dp[6];   /**< Data point ID for each of the 6 sections */
    uint16_t arc_dp;          /**< Data point ID for the center arc */
    float    arc_min;         /**< Arc minimum value (raw units) */
    float    arc_max;         /**< Arc maximum value (raw units) */
} mode_dp_map_t;

static const mode_dp_map_t mode_dp_maps[DISPLAY_MODE_COUNT] = {
    [DISPLAY_MODE_ENGINE] = {
        .section_dp = {
            OPENDASH_DP_COOLANT_TEMP,    /* [0] COOLANT °C  */
            OPENDASH_DP_GPS_SPEED,       /* [1] GPS SPEED   */
            OPENDASH_DP_BOOST_PRESSURE,  /* [2] BOOST kPa   */
            OPENDASH_DP_OIL_TEMP,        /* [3] OIL TEMP °C */
            OPENDASH_DP_LAP_TIME,        /* [4] LAP TIME    */
            OPENDASH_DP_AFR,             /* [5] AFR         */
        },
        .arc_dp  = OPENDASH_DP_RPM,
        .arc_min = 0.0f,
        .arc_max = 8000.0f,
    },
    [DISPLAY_MODE_GPS] = {
        .section_dp = {
            OPENDASH_DP_ALTITUDE,        /* [0] ALTITUDE m  */
            OPENDASH_DP_SAT_COUNT,       /* [1] SAT COUNT   */
            OPENDASH_DP_GPS_HEADING,     /* [2] HEADING °   */
            OPENDASH_DP_BATTERY_VOLTAGE, /* [3] BATTERY V   */
            OPENDASH_DP_HDOP,            /* [4] HDOP        */
            OPENDASH_DP_GPS_FIX,         /* [5] GPS FIX     */
        },
        .arc_dp  = OPENDASH_DP_GPS_SPEED,
        .arc_min = 0.0f,
        .arc_max = 300.0f,
    },
    [DISPLAY_MODE_MD] = {
        .section_dp = {
            OPENDASH_DP_EGT1,            /* [0] EGT 1       */
            OPENDASH_DP_EGT2,            /* [1] EGT 2       */
            OPENDASH_DP_O2_LAMBDA,       /* [2] O2 / Lambda */
            OPENDASH_DP_EGT3,            /* [3] EGT 3       */
            OPENDASH_DP_EGT4,            /* [4] EGT 4       */
            OPENDASH_DP_MAF_RATE,        /* [5] MAS (LMM)   */
        },
        .arc_dp  = OPENDASH_DP_MD_RPM,
        .arc_min = 0.0f,
        .arc_max = 8000.0f,
    },
};

/* Track max values per section for the "Max:" label */
static float s_section_max[DISPLAY_MODE_COUNT][6];
static bool  s_section_has_data[DISPLAY_MODE_COUNT][6];

void ui_manager_update_value(uint16_t data_point_id, float value)
{
    const mode_dp_map_t *map = &mode_dp_maps[current_mode];
    char buf[32];

    /* ── Check center arc ──────────────────────────────────── */
    if (data_point_id == map->arc_dp) {
        /* Arc value (integer for RPM, 1 decimal for speed) */
        if (map->arc_dp == OPENDASH_DP_RPM) {
            snprintf(buf, sizeof(buf), "%d", (int)value);
        } else {
            snprintf(buf, sizeof(buf), "%.0f", value);
        }
        update_outlined_label_text(screen_layout.gauge_value, buf);

        /* Track min/max for center arc */
        if (!s_arc_has_data[current_mode]) {
            s_arc_min_val[current_mode] = value;
            s_arc_max_val[current_mode] = value;
            s_arc_has_data[current_mode] = true;
        } else {
            if (value < s_arc_min_val[current_mode]) s_arc_min_val[current_mode] = value;
            if (value > s_arc_max_val[current_mode]) s_arc_max_val[current_mode] = value;
        }
        /* Update min/max label */
        if (screen_layout.gauge_minmax) {
            char mm_buf[48];
            if (map->arc_dp == OPENDASH_DP_RPM) {
                snprintf(mm_buf, sizeof(mm_buf), "MIN: %d   MAX: %d",
                         (int)s_arc_min_val[current_mode],
                         (int)s_arc_max_val[current_mode]);
            } else {
                snprintf(mm_buf, sizeof(mm_buf), "MIN: %.0f   MAX: %.0f",
                         s_arc_min_val[current_mode],
                         s_arc_max_val[current_mode]);
            }
            update_outlined_label_text(screen_layout.gauge_minmax, mm_buf);
        }

        /* Update arc indicator */
        float range = map->arc_max - map->arc_min;
        int pct = 0;
        if (range > 0.0f) {
            pct = (int)(((value - map->arc_min) / range) * 100.0f);
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
        }
        lv_arc_set_value(screen_layout.main_gauge,
                         (int32_t)(map->arc_min + (value - map->arc_min)));

        /* ── Shift-light: blink indicator + text at high RPM ── */
        if (map->arc_dp == OPENDASH_DP_RPM) {
            if (pct >= SHIFT_BLINK_THRESHOLD) {
                uint32_t now = esp_log_timestamp();
                if (!s_shift_active) {
                    s_shift_active = true;
                    s_shift_last_toggle_ms = now;
                    s_shift_blink_on = true;
                }
                if (now - s_shift_last_toggle_ms >= SHIFT_BLINK_INTERVAL_MS) {
                    s_shift_last_toggle_ms = now;
                    s_shift_blink_on = !s_shift_blink_on;
                }
                /* Toggle shift light bar visibility (cheap — small rectangle) */
                if (screen_layout.shift_light_bar) {
                    uint32_t bar_color = s_shift_blink_on ? 0xFF0000 : 0x00AAFF;
                    lv_obj_set_style_bg_color(screen_layout.shift_light_bar,
                                               lv_color_hex(bar_color), 0);
                    lv_obj_clear_flag(screen_layout.shift_light_bar, LV_OBJ_FLAG_HIDDEN);
                }
                /* Blink the RPM number text (cheap — small label area) */
                uint32_t text_color = s_shift_blink_on ? 0xFF0000 : 0x00AAFF;
                update_outlined_label_color(screen_layout.gauge_value,
                                            text_color, 0x000000);
            } else if (s_shift_active) {
                s_shift_active = false;
                s_shift_blink_on = false;
                /* Hide shift light bar */
                if (screen_layout.shift_light_bar) {
                    lv_obj_add_flag(screen_layout.shift_light_bar, LV_OBJ_FLAG_HIDDEN);
                }
                /* Restore white RPM text */
                update_outlined_label_color(screen_layout.gauge_value,
                                            0xFFFFFF, 0x000000);
            }
        }
        return;
    }

    /* ── Check the 6 data sections ─────────────────────────── */
    for (int i = 0; i < 6; i++) {
        if (data_point_id != map->section_dp[i]) continue;

        /* Apply unit conversions (internal values are metric) */
        float display_val = value;
        if (data_point_id == OPENDASH_DP_COOLANT_TEMP ||
            data_point_id == OPENDASH_DP_OIL_TEMP ||
            data_point_id == OPENDASH_DP_INTAKE_TEMP ||
            data_point_id == OPENDASH_DP_EGT ||
            data_point_id == OPENDASH_DP_EGT1 ||
            data_point_id == OPENDASH_DP_EGT2 ||
            data_point_id == OPENDASH_DP_EGT3 ||
            data_point_id == OPENDASH_DP_EGT4 ||
            data_point_id == OPENDASH_DP_TRANS_TEMP) {
            display_val = opendash_convert_temp(value, current_layout.temp_unit);
        } else if (data_point_id == OPENDASH_DP_BOOST_PRESSURE ||
                   data_point_id == OPENDASH_DP_OIL_PRESSURE ||
                   data_point_id == OPENDASH_DP_FUEL_PRESSURE) {
            display_val = opendash_convert_pressure(value, current_layout.pressure_unit);
        } else if (data_point_id == OPENDASH_DP_GPS_SPEED ||
                   data_point_id == OPENDASH_DP_VEHICLE_SPEED) {
            display_val = opendash_convert_speed(value, current_layout.speed_unit);
        } else if (data_point_id == OPENDASH_DP_ALTITUDE) {
            /* m → ft (1 m = 3.28084 ft) */
            if (current_layout.distance_unit == OPENDASH_DISTANCE_MI) {
                display_val = value * 3.28084f;
            }
        }

        /* Format value depending on type */
        if (data_point_id == OPENDASH_DP_AFR ||
            data_point_id == OPENDASH_DP_HDOP ||
            data_point_id == OPENDASH_DP_BATTERY_VOLTAGE) {
            snprintf(buf, sizeof(buf), "%.1f", display_val);
        } else if (data_point_id == OPENDASH_DP_GPS_FIX) {
            snprintf(buf, sizeof(buf), "%s", display_val >= 1.0f ? "3D FIX" : "NO FIX");
        } else if (data_point_id == OPENDASH_DP_LAP_TIME) {
            /* Lap time in ms → mm:ss.sss */
            uint32_t ms = (uint32_t)display_val;
            uint32_t min = ms / 60000;
            uint32_t sec = (ms % 60000) / 1000;
            uint32_t frac = ms % 1000;
            snprintf(buf, sizeof(buf), "%lu:%02lu.%03lu", min, sec, frac);
        } else {
            snprintf(buf, sizeof(buf), "%.0f", display_val);
        }
        lv_label_set_text(screen_layout.sections[i].value, buf);

        /* Track and display max value (in display units) */
        if (!s_section_has_data[current_mode][i]) {
            s_section_max[current_mode][i] = display_val;
            s_section_has_data[current_mode][i] = true;
        } else if (display_val > s_section_max[current_mode][i]) {
            s_section_max[current_mode][i] = display_val;
        }
        snprintf(buf, sizeof(buf), "Max: %.0f", s_section_max[current_mode][i]);
        lv_label_set_text(screen_layout.sections[i].max_val, buf);

        return;
    }

    /* Data point not mapped to current display mode — silently ignore */
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
