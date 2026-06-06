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
#include "espnow_master.h"
#include "opendash_rollover.h"
#include "boost_config_ui.h"
#include "boost_client.h"
#include "node_health.h"
#include "node_definitions.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "opendash_common.h"
#include "opendash_data_model.h"
#include "opendash_fonts.h"
#include "opendash_ui_styles.h"
#include "opendash_relay.h"
#include "opendash_uart.h"
#include "opendash_obd_config.h"
#include "opendash_i2c_protocol.h"
#include <stdarg.h>
#include <stdio.h>
#include "opendash_layout.h"
#include "opendash_layout_store.h"
#include "opendash_dp_catalog.h"
#include "layout_editor.h"
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
    },
    [DISPLAY_MODE_RELAY] = {
        .section_labels = {"", "", "", "", "", ""},  /* Not used — relay grid has own labels */
        .status_text = "DEBUG: RELAY CONTROL | Tap box to toggle | Hold here to exit"
    },
    [DISPLAY_MODE_BMS] = {
        .section_labels = {"", "", "", "", "", ""},  /* Not used — BMS grid has own labels */
        .status_text = "DEBUG: BMS DATA | rAtTrax Battery Management | Hold here to exit"
    },
    [DISPLAY_MODE_OBD] = {
        .section_labels = {"", "", "", "", "", ""},  /* Not used — OBD grid has own labels */
        .status_text = "OBD2 DATA | Engine Diagnostics | Swipe to navigate"
    },
    [DISPLAY_MODE_CONFIG] = {
        .section_labels = {"", "", "", "", "", ""},  /* Not used — config grid has own labels */
        .status_text = "MODE: SYSTEM CONFIG | OTA / Nodes / Settings"
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
    lv_obj_t *arc_outline;         /* Outer arc outline ring */
    lv_obj_t *main_gauge;          /* Center arc (RPM or Speed) */
    lv_obj_t *gauge_value;         /* Center arc value text */
    lv_obj_t *gauge_unit;          /* Arc unit label ("RPM") */
    lv_obj_t *gauge_minmax;        /* Min/Max label below arc value */
    lv_obj_t *shift_light_bar;     /* Shift light indicator bar (blinks at redline) */
    data_section_widgets_t sections[6];  /* 6 data display sections */
    lv_obj_t *status_bar;          /* Status bar at bottom */
    lv_obj_t *status_text;         /* Status text (mode indicator) */
} screen_layout_t;

/* ── Relay Control Grid ─────────────────────────────────────────────────── */
#define RELAY_GRID_COLS  7
#define RELAY_GRID_ROWS  4
#define RELAY_GRID_TOTAL (RELAY_GRID_COLS * RELAY_GRID_ROWS)

/**
 * @brief Relay box mapping — which node & channel each grid box controls
 */
typedef struct {
    const char      *label;      /**< Display label (e.g., "RAD FAN 1") */
    opendash_node_t  node;       /**< Target relay/MOS node */
    uint8_t          channel;    /**< Channel index on that node (0-based) */
    bool             is_on;      /**< Current known state */
} relay_box_mapping_t;

typedef struct {
    lv_obj_t *box;               /**< Container object */
    lv_obj_t *label;             /**< Label text */
    lv_obj_t *state_label;       /**< "ON" / "OFF" indicator */
} relay_box_widgets_t;

static relay_box_widgets_t relay_boxes[RELAY_GRID_TOTAL] = {0};
static lv_obj_t *relay_grid_container = NULL;

/**
 * @brief Relay box mappings — 15 boxes in 5×3 grid.
 * Maps each box to a specific relay node + channel.
 * Update labels to match your physical wiring.
 */
static relay_box_mapping_t relay_box_map[RELAY_GRID_TOTAL] = {
    /* Row 0: 4-CH HD relay (fans/pumps) + RELAY A CH1-3 */
    { .label = "RAD FAN 1",   .node = OPENDASH_NODE_RELAY_4CH,   .channel = 0, .is_on = false },
    { .label = "RAD FAN 2",   .node = OPENDASH_NODE_RELAY_4CH,   .channel = 1, .is_on = false },
    { .label = "WATER PUMP",  .node = OPENDASH_NODE_RELAY_4CH,   .channel = 2, .is_on = false },
    { .label = "FUEL PUMP",   .node = OPENDASH_NODE_RELAY_4CH,   .channel = 3, .is_on = false },
    { .label = "R-A CH1",     .node = OPENDASH_NODE_RELAY_8CH_A, .channel = 0, .is_on = false },
    { .label = "R-A CH2",     .node = OPENDASH_NODE_RELAY_8CH_A, .channel = 1, .is_on = false },
    { .label = "R-A CH3",     .node = OPENDASH_NODE_RELAY_8CH_A, .channel = 2, .is_on = false },
    /* Row 1: RELAY A CH4-8 + RELAY B CH1-2 */
    { .label = "R-A CH4",     .node = OPENDASH_NODE_RELAY_8CH_A, .channel = 3, .is_on = false },
    { .label = "R-A CH5",     .node = OPENDASH_NODE_RELAY_8CH_A, .channel = 4, .is_on = false },
    { .label = "R-A CH6",     .node = OPENDASH_NODE_RELAY_8CH_A, .channel = 5, .is_on = false },
    { .label = "R-A CH7",     .node = OPENDASH_NODE_RELAY_8CH_A, .channel = 6, .is_on = false },
    { .label = "R-A CH8",     .node = OPENDASH_NODE_RELAY_8CH_A, .channel = 7, .is_on = false },
    { .label = "R-B CH1",     .node = OPENDASH_NODE_RELAY_8CH_B, .channel = 0, .is_on = false },
    { .label = "R-B CH2",     .node = OPENDASH_NODE_RELAY_8CH_B, .channel = 1, .is_on = false },
    /* Row 2: RELAY B CH3-8 + MOS A CH1 */
    { .label = "R-B CH3",     .node = OPENDASH_NODE_RELAY_8CH_B, .channel = 2, .is_on = false },
    { .label = "R-B CH4",     .node = OPENDASH_NODE_RELAY_8CH_B, .channel = 3, .is_on = false },
    { .label = "R-B CH5",     .node = OPENDASH_NODE_RELAY_8CH_B, .channel = 4, .is_on = false },
    { .label = "R-B CH6",     .node = OPENDASH_NODE_RELAY_8CH_B, .channel = 5, .is_on = false },
    { .label = "R-B CH7",     .node = OPENDASH_NODE_RELAY_8CH_B, .channel = 6, .is_on = false },
    { .label = "R-B CH8",     .node = OPENDASH_NODE_RELAY_8CH_B, .channel = 7, .is_on = false },
    { .label = "MOS-A CH1",   .node = OPENDASH_NODE_MOS_4CH_A,   .channel = 0, .is_on = false },
    /* Row 3: MOS A CH2-4 + MOS B CH1-4 */
    { .label = "MOS-A CH2",   .node = OPENDASH_NODE_MOS_4CH_A,   .channel = 1, .is_on = false },
    { .label = "MOS-A CH3",   .node = OPENDASH_NODE_MOS_4CH_A,   .channel = 2, .is_on = false },
    { .label = "MOS-A CH4",   .node = OPENDASH_NODE_MOS_4CH_A,   .channel = 3, .is_on = false },
    { .label = "MOS-B CH1",   .node = OPENDASH_NODE_MOS_4CH_B,   .channel = 0, .is_on = false },
    { .label = "MOS-B CH2",   .node = OPENDASH_NODE_MOS_4CH_B,   .channel = 1, .is_on = false },
    { .label = "MOS-B CH3",   .node = OPENDASH_NODE_MOS_4CH_B,   .channel = 2, .is_on = false },
    { .label = "MOS-B CH4",   .node = OPENDASH_NODE_MOS_4CH_B,   .channel = 3, .is_on = false },
};

/* ── Data Grid (shared layout for BMS + OBD screens) ────────────────────── */
#define DATA_GRID_COLS  5
#define DATA_GRID_ROWS  3
#define DATA_GRID_TOTAL (DATA_GRID_COLS * DATA_GRID_ROWS)

typedef struct {
    const char *label;          /**< Display label (e.g., "SOC") */
    uint16_t    data_point_id;  /**< OpenDash data point ID */
    uint8_t     decimals;       /**< Decimal places for formatting (0 or 1) */
    const char *unit;           /**< Unit suffix (e.g., "V", "A", "%") */
} data_grid_mapping_t;

typedef struct {
    lv_obj_t *box;
    lv_obj_t *label;
    lv_obj_t *value;
    lv_obj_t *unit_label;
} data_grid_box_widgets_t;

/* BMS grid */
static data_grid_box_widgets_t bms_boxes[DATA_GRID_TOTAL] = {0};
static lv_obj_t *bms_grid_container = NULL;

static const data_grid_mapping_t bms_grid_map[DATA_GRID_TOTAL] = {
    /* Row 0: Pack overview */
    { .label = "SOC",        .data_point_id = OPENDASH_DP_SOC,           .decimals = 0, .unit = "%" },
    { .label = "PACK V",     .data_point_id = OPENDASH_DP_PACK_VOLTAGE, .decimals = 1, .unit = "V" },
    { .label = "PACK I",     .data_point_id = OPENDASH_DP_PACK_CURRENT, .decimals = 1, .unit = "A" },
    { .label = "POWER",      .data_point_id = OPENDASH_DP_PACK_POWER,   .decimals = 0, .unit = "W" },
    { .label = "SOH",        .data_point_id = OPENDASH_DP_SOH,          .decimals = 0, .unit = "%" },
    /* Row 1: Cell voltages + temps */
    { .label = "CELL MIN",   .data_point_id = OPENDASH_DP_CELL_V_MIN,   .decimals = 3, .unit = "V" },
    { .label = "CELL MAX",   .data_point_id = OPENDASH_DP_CELL_V_MAX,   .decimals = 3, .unit = "V" },
    { .label = "DELTA",      .data_point_id = OPENDASH_DP_CELL_V_DELTA, .decimals = 0, .unit = "mV" },
    { .label = "BMS TEMP",   .data_point_id = OPENDASH_DP_BMS_TEMP_MAX, .decimals = 0, .unit = "\xc2\xb0""F" },
    { .label = "IC TEMP",    .data_point_id = OPENDASH_DP_BMS_TEMP_IC,  .decimals = 0, .unit = "\xc2\xb0""F" },
    /* Row 2: VESC motor data */
    { .label = "VESC RPM",   .data_point_id = OPENDASH_DP_VESC_RPM,      .decimals = 0, .unit = "" },
    { .label = "MOTOR I",    .data_point_id = OPENDASH_DP_VESC_CURRENT,  .decimals = 1, .unit = "A" },
    { .label = "FET TEMP",   .data_point_id = OPENDASH_DP_VESC_TEMP_FET, .decimals = 0, .unit = "\xc2\xb0""F" },
    { .label = "MTR TEMP",   .data_point_id = OPENDASH_DP_VESC_TEMP_MOTOR,.decimals = 0, .unit = "\xc2\xb0""F" },
    { .label = "FAULT",      .data_point_id = OPENDASH_DP_VESC_FAULT,    .decimals = 0, .unit = "" },
};

/* OBD grid */
static data_grid_box_widgets_t obd_boxes[DATA_GRID_TOTAL] = {0};
static lv_obj_t *obd_grid_container = NULL;

/* OBD dashboard dedicated widgets (beyond the grid boxes) */
static lv_obj_t *obd_rpm_arc = NULL;          /* RPM sweep arc */
static lv_obj_t *obd_speed_value = NULL;      /* Large speed readout */

static const data_grid_mapping_t obd_grid_map[DATA_GRID_TOTAL] = {
    /* Row 0: Primary engine data */
    { .label = "RPM",        .data_point_id = OPENDASH_DP_RPM,            .decimals = 0, .unit = "" },
    { .label = "SPEED",      .data_point_id = OPENDASH_DP_VEHICLE_SPEED,  .decimals = 0, .unit = "MPH" },
    { .label = "COOLANT",    .data_point_id = OPENDASH_DP_COOLANT_TEMP,   .decimals = 0, .unit = "\xc2\xb0""F" },
    { .label = "INTAKE",     .data_point_id = OPENDASH_DP_INTAKE_TEMP,    .decimals = 0, .unit = "\xc2\xb0""F" },
    { .label = "LOAD",       .data_point_id = OPENDASH_DP_ENGINE_LOAD,    .decimals = 0, .unit = "%" },
    /* Row 1: Secondary engine data */
    { .label = "THROTTLE",   .data_point_id = OPENDASH_DP_THROTTLE_POS,   .decimals = 0, .unit = "%" },
    { .label = "BOOST",      .data_point_id = OPENDASH_DP_BOOST_PRESSURE, .decimals = 1, .unit = "PSI" },
    { .label = "OIL TEMP",   .data_point_id = OPENDASH_DP_OIL_TEMP,      .decimals = 0, .unit = "\xc2\xb0""F" },
    { .label = "OIL PSI",    .data_point_id = OPENDASH_DP_OIL_PRESSURE,   .decimals = 0, .unit = "PSI" },
    { .label = "FUEL PSI",   .data_point_id = OPENDASH_DP_FUEL_PRESSURE,  .decimals = 0, .unit = "PSI" },
    /* Row 2: Exhaust / electrical */
    { .label = "AFR",        .data_point_id = OPENDASH_DP_AFR,            .decimals = 1, .unit = "" },
    { .label = "LAMBDA",     .data_point_id = OPENDASH_DP_LAMBDA,         .decimals = 2, .unit = "" },
    { .label = "EGT",        .data_point_id = OPENDASH_DP_EGT,            .decimals = 0, .unit = "\xc2\xb0""F" },
    { .label = "BATT V",     .data_point_id = OPENDASH_DP_BATTERY_VOLTAGE,.decimals = 1, .unit = "V" },
    { .label = "TIMING",     .data_point_id = OPENDASH_DP_TIMING_ADVANCE, .decimals = 1, .unit = "\xc2\xb0" },
};

/* Config/OTA grid */
static lv_obj_t *config_grid_container = NULL;
static lv_obj_t *config_action_status = NULL;

/* UI component references */
static screen_layout_t screen_layout = {0};  /* Single screen with all modes */
static display_mode_t current_mode = DISPLAY_MODE_ENGINE;
static bool s_debug_mode_active = false; /**< true when RELAY/BMS/OBD debug screens are active */
static warning_box_t warning_boxes[2] = {0};  /* 0=left, 1=right */

/* Configuration & state */
static opendash_display_layout_t current_layout;
static TaskHandle_t ui_task_handle = NULL;
static uint32_t last_swipe_time = 0;

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

/* Forward declarations for callbacks/helpers referenced before definition */
static void update_display_mode(display_mode_t new_mode);
static void status_bar_longpress_cb(lv_event_t *e);
static void restore_mode_cached_values(display_mode_t mode);
static void capture_mode_defaults(void);
static void load_layouts_from_nvs(void);
static void devmgmt_layout_editor_cb(lv_event_t *e);

/* ── OBD Config Submenu ─────────────────────────────────────────────────── */
static lv_obj_t *obd_config_container = NULL;  /* OBD config submenu screen */
static lv_obj_t *obd_enable_btn_label = NULL;
static lv_obj_t *obd_mil_btn_label = NULL;
static lv_obj_t *obd_dtc_list_label = NULL;
static lv_obj_t *obd_vin_label = NULL;

/* ── MIL Indicator (status bar integration) ──────────────────────────────── */
static lv_obj_t *mil_cel_label = NULL;      /* "CEL" label on left side of status bar */
static lv_timer_t *mil_blink_timer = NULL;
static bool mil_blink_visible = true;
static bool mil_currently_active = false;   /* Track state for border restore */

/* ── BLE-OTA Indicator (status bar integration) ──────────────────────────── */
static lv_obj_t *ota_badge_label = NULL;    /* "BT-OTA: <node>" badge on right side */
static lv_timer_t *ota_blink_timer = NULL;
static bool ota_blink_visible = true;
static bool ota_currently_active = false;

/* ─────────────────────────────────────────────────────────────────────────
 * Outlined label — atomic single-widget rewrite (replaces 5-label stack)
 * ─────────────────────────────────────────────────────────────────────────
 *
 * Why: the previous implementation built a container holding FIVE labels
 * (4 black shadows at cardinal offsets + 1 colored center). Every value
 * update called lv_label_set_text on all 5 children, producing 5 separate
 * invalidation rectangles. LVGL 9 coalesces invalidations *per render
 * cycle* but on a fast-updating gauge each label could reach the painter
 * in a different cycle. Result: outlines visible without center, or
 * center visible without outlines — the user-reported "epileptic flash".
 *
 * Fix: ONE lv_label_t whose LV_EVENT_DRAW_MAIN_BEGIN handler emits the
 * 4 outline copies into the same draw layer, *before* LVGL paints the
 * label's own (colored) glyphs on top. One widget = one invalidation =
 * one frame = atomic. The strobe cannot occur because the outlines and
 * the main glyphs are emitted to the same lv_layer_t inside the same
 * paint pass.
 *
 * The function names create_outlined_label / update_outlined_label_text /
 * update_outlined_label_color are preserved so all 30+ call sites in
 * this file keep working unchanged. The returned object is now a real
 * lv_label rather than a container, but every caller treats it as an
 * opaque lv_obj_t* (align/position/visibility), so this is binary-
 * compatible with existing layout code.
 *
 * Caller contract:
 *   - obj returned IS the label (lv_label_class). lv_label_get_text(obj)
 *     and lv_obj_set_style_text_color(obj, …) work directly.
 *   - The outline pad is added via style padding, so lv_obj_align places
 *     the visible bounding box (text + outline) the same way the old
 *     container did. No layout positions need adjustment.
 *   - User data is owned by the widget and freed automatically on delete.
 */

typedef struct {
    uint32_t outline_color;     /* RGB888 packed; converted at draw time */
    int8_t   outline_px;        /* outline thickness in pixels (0..3 sane) */
} outlined_attrs_t;

/**
 * @brief Free attrs struct when the label is destroyed.
 *
 * Attached as LV_EVENT_DELETE handler so user_data never leaks even if
 * the layout is rebuilt mid-session (mode switch tears down all widgets).
 */
static void outlined_label_delete_cb(lv_event_t *e)
{
    lv_obj_t *label = lv_event_get_target_obj(e);
    outlined_attrs_t *attrs = (outlined_attrs_t *)lv_obj_get_user_data(label);
    if (attrs) {
        lv_free(attrs);
        lv_obj_set_user_data(label, NULL);
    }
}

/**
 * @brief Tell LVGL the widget paints outline_px outside its bounding box.
 *
 * Without this hook, when the label's text changes width (e.g. "100" →
 * "1000"), LVGL only invalidates the new bounding box. The outline
 * stamps that the previous frame painted at ±outline_px outside the
 * OLD bounding box are NOT inside the new dirty rect, so they linger on
 * screen as ghost pixels. With double-FB DIRECT mode this manifests as
 * the user-reported "epileptic flicker" on every numeric value update.
 *
 * Setting ext_draw_size = outline_px makes LVGL inflate the dirty rect
 * by that many pixels on every side before invalidating, so the leftover
 * outline pixels from the previous frame are guaranteed to be repainted.
 * This is the canonical LVGL pattern for any widget that draws outside
 * its own bounds (see LVGL docs: "Custom drawing").
 */
static void outlined_label_ext_draw_cb(lv_event_t *e)
{
    lv_obj_t *label = lv_event_get_target_obj(e);
    outlined_attrs_t *attrs = (outlined_attrs_t *)lv_obj_get_user_data(label);
    if (!attrs || attrs->outline_px <= 0) return;
    lv_event_set_ext_draw_size(e, attrs->outline_px);
}

/**
 * @brief Pre-main draw hook: stamp 4 outline copies into the layer.
 *
 * Runs from LV_EVENT_DRAW_MAIN_BEGIN, i.e. *before* the label class's
 * own DRAW_MAIN renders the colored glyphs. Both the outline stamps and
 * the main glyph render target the same lv_layer_t and are flushed in
 * the same paint pass — no inter-frame splitting is possible.
 */
static void outlined_label_draw_cb(lv_event_t *e)
{
    lv_obj_t *label = lv_event_get_target_obj(e);
    outlined_attrs_t *attrs = (outlined_attrs_t *)lv_obj_get_user_data(label);
    if (!attrs || attrs->outline_px <= 0) return;

    const char *text = lv_label_get_text(label);
    if (!text || !text[0]) return;

    lv_layer_t *layer = lv_event_get_layer(e);
    if (!layer) return;

    /* Pull the label's own draw descriptor (font, letter_space, opacity,
     * align, etc.) so the outline visually matches the main glyphs. */
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    lv_obj_init_draw_label_dsc(label, LV_PART_MAIN, &dsc);
    dsc.text = text;
    dsc.color = lv_color_hex(attrs->outline_color);
    /* text_local=false: dsc.text points at the label's text buffer which
     * remains valid for the duration of this paint. */
    dsc.text_local = 0;

    /* Coords for the label's text area (after style padding). The 4
     * stamps are emitted at ±outline_px on each axis — same cardinal
     * pattern the old container used, but rendered atomically. */
    lv_area_t coords;
    lv_obj_get_coords(label, &coords);
    /* Shrink coords by the label's own padding so we draw *into* the text
     * cell, not the padded box. Matches LVGL's own DRAW_MAIN behaviour. */
    int32_t pl = lv_obj_get_style_pad_left(label, LV_PART_MAIN);
    int32_t pr = lv_obj_get_style_pad_right(label, LV_PART_MAIN);
    int32_t pt = lv_obj_get_style_pad_top(label, LV_PART_MAIN);
    int32_t pb = lv_obj_get_style_pad_bottom(label, LV_PART_MAIN);
    coords.x1 += pl; coords.x2 -= pr;
    coords.y1 += pt; coords.y2 -= pb;

    const int8_t off = attrs->outline_px;
    const int8_t offsets[4][2] = {
        {-off,  0}, { off,  0},     /* left, right */
        {  0, -off}, {  0,  off},   /* top,  bottom */
    };
    for (int i = 0; i < 4; i++) {
        lv_area_t a = coords;
        lv_area_move(&a, offsets[i][0], offsets[i][1]);
        lv_draw_label(layer, &dsc, &a);
    }
}

/**
 * @brief Create an outlined label.
 *
 * @param parent        LVGL parent (e.g. a sketch container or screen).
 * @param text          Initial text (copied by lv_label_set_text).
 * @param font_size     opendash_font_size_t enum; mapped via opendash_set_font.
 * @param text_color    RGB888 main-glyph color.
 * @param outline_color RGB888 outline color.
 * @param outline_px    Outline thickness; 0 disables outline (no draw hook cost).
 * @return The lv_label object. Treat as opaque; use update_outlined_label_*
 *         helpers below to mutate text or colors so invalidation stays atomic.
 *
 * @note Padding is set to outline_px on all sides so the outline stamps
 *       are not clipped by the label's own bounding box. Layout code
 *       that uses lv_obj_align places the *padded* box exactly as the
 *       previous container did — no callsite changes required.
 */
static lv_obj_t* create_outlined_label(lv_obj_t *parent, const char *text,
                                        opendash_font_size_t font_size,
                                        uint32_t text_color, uint32_t outline_color,
                                        int8_t outline_px)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text ? text : "");
    opendash_set_font(label, font_size);
    lv_obj_set_style_text_color(label, lv_color_hex(text_color), 0);

    /* Pad equal on all sides so outline stamps fit inside the widget's
     * bounding rectangle. Without this, lv_draw_label clips the offset
     * stamps and the outline appears as half-cut letters at the edges. */
    if (outline_px > 0) {
        lv_obj_set_style_pad_all(label, outline_px, 0);
    }

    outlined_attrs_t *attrs = (outlined_attrs_t *)lv_malloc(sizeof(*attrs));
    if (attrs) {
        attrs->outline_color = outline_color;
        attrs->outline_px    = outline_px;
        lv_obj_set_user_data(label, attrs);
        lv_obj_add_event_cb(label, outlined_label_draw_cb,
                            LV_EVENT_DRAW_MAIN_BEGIN, NULL);
        /* CRITICAL: ext_draw_size hook fixes the value-strobe flicker.
         * See outlined_label_ext_draw_cb header for the full root-cause
         * analysis. Without this, every numeric update on a value label
         * leaves outline ghost pixels that flash on/off across frames. */
        lv_obj_add_event_cb(label, outlined_label_ext_draw_cb,
                            LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
        lv_obj_add_event_cb(label, outlined_label_delete_cb,
                            LV_EVENT_DELETE, NULL);
    }
    /* If lv_malloc failed we still return a working (un-outlined) label
     * rather than crash — visible degradation, not a boot failure. */

    return label;
}

/**
 * @brief Update outlined label text — atomic single invalidation.
 *
 * Backward-compatible wrapper: existing call sites pass the value
 * returned by create_outlined_label. Internally just lv_label_set_text
 * on the one widget — LVGL emits exactly one invalidation rect, no
 * cross-frame paint splitting can occur.
 */
static void update_outlined_label_text(lv_obj_t *label, const char *new_text)
{
    if (label == NULL || new_text == NULL) return;
    if (lv_obj_check_type(label, &lv_label_class)) {
        lv_label_set_text(label, new_text);
    }
}

/**
 * @brief Update outlined label colors — atomic single invalidation.
 *
 * The outline color lives in the attrs struct (read by the draw hook on
 * next paint). The main color is a normal style write. Both are reflected
 * in the very next paint pass; we explicitly invalidate once at the end
 * so LVGL re-renders even if no other style flag changed.
 */
static void update_outlined_label_color(lv_obj_t *label,
                                         uint32_t text_color, uint32_t outline_color)
{
    if (label == NULL) return;
    if (!lv_obj_check_type(label, &lv_label_class)) return;

    bool changed = false;

    outlined_attrs_t *attrs = (outlined_attrs_t *)lv_obj_get_user_data(label);
    if (attrs && attrs->outline_color != outline_color) {
        attrs->outline_color = outline_color;
        changed = true;
    }

    lv_color_t cur = lv_obj_get_style_text_color(label, LV_PART_MAIN);
    lv_color_t want = lv_color_hex(text_color);
    /* lv_color_t comparison: compare packed channels directly. */
    if (cur.red != want.red || cur.green != want.green || cur.blue != want.blue) {
        lv_obj_set_style_text_color(label, want, 0);
        changed = true;
    }

    if (changed) {
        /* One invalidation per color change — covers both outline and main.
         * Skipping invalidation when nothing changed prevents the shift-
         * light blink path from forcing a needless redraw every tick. */
        lv_obj_invalidate(label);
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
    const int arc_y_offset = -STATUS_BAR_HEIGHT / 2 + 30;
    
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
    screen_layout.arc_outline = NULL;  /* Outline arc removed — LVGL 9.2.2 circ_calc_aa4 crash */
    screen_layout.gauge_unit = rpm_unit;

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

/* ────────────────────────────────────────────────────────────────────────────
 * Relay Control Grid — 7×4 Toggle Boxes (28 channels)
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Touch callback for a relay box — toggles ON/OFF
 */
static void relay_box_touch_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED) return;

    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= RELAY_GRID_TOTAL) return;

    relay_box_mapping_t *map = &relay_box_map[idx];
    map->is_on = !map->is_on;

    /* Send relay command via ESP-NOW */
    esp_err_t ret = espnow_master_send_relay_command(map->node, map->channel,
                                                      map->is_on ? 1 : 0, 255);

    /* Update box visual */
    if (relay_boxes[idx].state_label) {
        lv_label_set_text(relay_boxes[idx].state_label, map->is_on ? "ON" : "OFF");
        lv_obj_set_style_text_color(relay_boxes[idx].state_label,
            lv_color_hex(map->is_on ? 0x00FF00 : 0xFF4444), 0);
    }
    if (relay_boxes[idx].box) {
        lv_obj_set_style_bg_color(relay_boxes[idx].box,
            lv_color_hex(map->is_on ? 0x004400 : OPENDASH_COLOR_BG_SECTION), 0);
        /* Orange border if node is offline — state still persists across screens */
        lv_obj_set_style_border_color(relay_boxes[idx].box,
            lv_color_hex(ret == ESP_OK ? 0xFFFFFF : 0xFF8800), 0);
    }

    ESP_LOGI(TAG, "Relay box %d [%s] → %s (send=%s)", (int)idx, map->label,
             map->is_on ? "ON" : "OFF", esp_err_to_name(ret));
}

/**
 * @brief Update relay box visuals from a RELAY_STATUS report.
 * Called when center receives OPENDASH_CMD_RELAY_STATUS from a node.
 */
void ui_manager_update_relay_status(opendash_node_t node, const uint8_t *states, uint8_t num_ch)
{
    for (int idx = 0; idx < RELAY_GRID_TOTAL; idx++) {
        relay_box_mapping_t *map = &relay_box_map[idx];
        if (map->node != node) continue;
        if (map->channel >= num_ch) continue;

        bool on = (states[map->channel] != 0);
        map->is_on = on;

        if (relay_boxes[idx].state_label) {
            lv_label_set_text(relay_boxes[idx].state_label, on ? "ON" : "OFF");
            lv_obj_set_style_text_color(relay_boxes[idx].state_label,
                lv_color_hex(on ? 0x00FF00 : 0xFF4444), 0);
        }
        if (relay_boxes[idx].box) {
            lv_obj_set_style_bg_color(relay_boxes[idx].box,
                lv_color_hex(on ? 0x004400 : OPENDASH_COLOR_BG_SECTION), 0);
            lv_obj_set_style_border_color(relay_boxes[idx].box,
                lv_color_hex(0xFFFFFF), 0);  /* Reset to white (was orange if offline) */
        }
    }
}

/**
 * @brief Create the 7×4 relay control grid (hidden by default)
 */
static void create_relay_grid(lv_obj_t *parent)
{
    /* Full-screen container for the relay grid */
    relay_grid_container = lv_obj_create(parent);
    lv_obj_set_size(relay_grid_container, LCD_H_RES, LCD_V_RES - STATUS_BAR_HEIGHT - 20);
    lv_obj_align(relay_grid_container, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(relay_grid_container, 0, 0);
    lv_obj_set_style_border_width(relay_grid_container, 0, 0);
    lv_obj_set_style_pad_all(relay_grid_container, 10, 0);
    lv_obj_clear_flag(relay_grid_container, LV_OBJ_FLAG_SCROLLABLE);

    /* Calculate box sizes */
    const int grid_w = LCD_H_RES - 20;      /* 10px padding each side */
    const int grid_h = LCD_V_RES - STATUS_BAR_HEIGHT - 40;
    const int spacing = 6;
    const int box_w = (grid_w - (RELAY_GRID_COLS - 1) * spacing) / RELAY_GRID_COLS;
    const int box_h = (grid_h - (RELAY_GRID_ROWS - 1) * spacing) / RELAY_GRID_ROWS;

    for (int row = 0; row < RELAY_GRID_ROWS; row++) {
        for (int col = 0; col < RELAY_GRID_COLS; col++) {
            int idx = row * RELAY_GRID_COLS + col;
            int x = col * (box_w + spacing);
            int y = row * (box_h + spacing);

            /* Box container */
            lv_obj_t *box = lv_obj_create(relay_grid_container);
            lv_obj_set_size(box, box_w, box_h);
            lv_obj_set_pos(box, x, y);
            lv_obj_set_style_bg_color(box, lv_color_hex(OPENDASH_COLOR_BG_SECTION), 0);
            lv_obj_set_style_bg_opa(box, 242, 0);
            lv_obj_set_style_border_color(box, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_border_width(box, 3, 0);
            lv_obj_set_style_radius(box, 8, 0);
            lv_obj_set_style_pad_all(box, 0, 0);
            lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);

            /* Label — SMALL (14px) fits 7-column layout */
            lv_obj_t *label = create_simple_label(box, relay_box_map[idx].label,
                                                     OPENDASH_FONT_SIZE_SMALL,
                                                     OPENDASH_COLOR_TEXT_PRIMARY);
            lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 4);

            /* ON/OFF state indicator — LARGE (32px) for 7×4 boxes */
            bool on = relay_box_map[idx].is_on;
            lv_obj_t *state = create_simple_label(box, on ? "ON" : "OFF",
                                                   OPENDASH_FONT_SIZE_LARGE,
                                                   on ? 0x00FF00 : 0xFF4444);
            lv_obj_align(state, LV_ALIGN_CENTER, 0, 8);

            /* Restore box background for ON state */
            if (on) {
                lv_obj_set_style_bg_color(box,
                    lv_color_hex(0x004400), 0);
            }

            /* Touch handler — pass box index */
            lv_obj_add_event_cb(box, relay_box_touch_cb, LV_EVENT_CLICKED,
                                (void *)(intptr_t)idx);

            relay_boxes[idx].box = box;
            relay_boxes[idx].label = label;
            relay_boxes[idx].state_label = state;
        }
    }

    /* Start hidden — shown only in DISPLAY_MODE_RELAY */
    lv_obj_add_flag(relay_grid_container, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "Relay control grid created (%dx%d = %d boxes)",
             RELAY_GRID_COLS, RELAY_GRID_ROWS, RELAY_GRID_TOTAL);
}

/**
 * @brief Create a 5×3 data display grid (used for BMS and OBD screens)
 *
 * @param parent       LVGL parent object
 * @param map          Array of DATA_GRID_TOTAL data_grid_mapping_t entries
 * @param widgets_out  Array of DATA_GRID_TOTAL data_grid_box_widgets_t to fill
 * @return The grid container object (starts hidden)
 */
static lv_obj_t* create_data_grid(lv_obj_t *parent,
                                   const data_grid_mapping_t *map,
                                   data_grid_box_widgets_t *widgets_out)
{
    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_set_size(container, LCD_H_RES, LCD_V_RES - STATUS_BAR_HEIGHT - 20);
    lv_obj_align(container, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(container, 0, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 10, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    const int grid_w = LCD_H_RES - 20;
    const int grid_h = LCD_V_RES - STATUS_BAR_HEIGHT - 40;
    const int spacing = 6;
    const int box_w = (grid_w - (DATA_GRID_COLS - 1) * spacing) / DATA_GRID_COLS;
    const int box_h = (grid_h - (DATA_GRID_ROWS - 1) * spacing) / DATA_GRID_ROWS;

    for (int row = 0; row < DATA_GRID_ROWS; row++) {
        for (int col = 0; col < DATA_GRID_COLS; col++) {
            int idx = row * DATA_GRID_COLS + col;
            int x = col * (box_w + spacing);
            int y = row * (box_h + spacing);

            lv_obj_t *box = lv_obj_create(container);
            lv_obj_set_size(box, box_w, box_h);
            lv_obj_set_pos(box, x, y);
            lv_obj_set_style_bg_color(box, lv_color_hex(OPENDASH_COLOR_BG_SECTION), 0);
            lv_obj_set_style_bg_opa(box, 242, 0);
            lv_obj_set_style_border_color(box, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_border_width(box, 3, 0);
            lv_obj_set_style_radius(box, 8, 0);
            lv_obj_set_style_pad_all(box, 0, 0);
            lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

            /* Label at top */
            lv_obj_t *label = create_simple_label(box, map[idx].label,
                                                     OPENDASH_FONT_SIZE_MEDIUM,
                                                     OPENDASH_COLOR_TEXT_PRIMARY);
            lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 4);

            /* Value in center */
            lv_obj_t *value = create_simple_label(box, "---",
                                                   OPENDASH_FONT_SIZE_XLARGE,
                                                   OPENDASH_COLOR_TEXT_PRIMARY);
            lv_obj_align(value, LV_ALIGN_CENTER, 0, 5);

            /* Unit at bottom */
            lv_obj_t *unit_lbl = create_simple_label(box, map[idx].unit,
                                                      OPENDASH_FONT_SIZE_SMALL,
                                                      0xAAAAAA);
            lv_obj_align(unit_lbl, LV_ALIGN_BOTTOM_MID, 0, -4);

            widgets_out[idx].box = box;
            widgets_out[idx].label = label;
            widgets_out[idx].value = value;
            widgets_out[idx].unit_label = unit_lbl;
        }
    }

    lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);
    return container;
}

/* ── Config / OTA Grid ────────────────────────────────────────────────────── */

/**
 * @brief Config screen node entries
 */
typedef struct {
    const char *name;
    opendash_node_t node;
    lv_obj_t *status_label;
    lv_obj_t *box;             /* chip container — used for OTA blink border */
} config_node_entry_t;

#define CONFIG_NODE_COUNT   11
#define CONFIG_NODES_ROW1   6   /* LEFT RIGHT GPS BMS POD1 POD2 */
#define CONFIG_NODES_ROW2   5   /* RELAY-4CH RELAY-8A RELAY-8B MOS-A MOS-B */

/* CONFIG-screen per-node BLE-OTA blink state. When any node's status_flags
 * include OPENDASH_STATUS_FLAG_BLE_OTA, its chip border + label flash blue. */
static lv_timer_t *config_ota_blink_timer = NULL;
static bool config_ota_blink_visible = true;

/* Cache the last-rendered (state, flags) per chip so the 5 Hz heartbeat
 * doesn't keep invalidating identical labels. 0xFF means "never rendered". */
static uint8_t config_chip_last_state[11] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static uint8_t config_chip_last_flags[11] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static config_node_entry_t config_nodes[CONFIG_NODE_COUNT] = {
    { "LEFT",      OPENDASH_NODE_LEFT,       NULL, NULL },
    { "RIGHT",     OPENDASH_NODE_RIGHT,      NULL, NULL },
    { "GPS",       OPENDASH_NODE_GPS,        NULL, NULL },
    { "BMS",       OPENDASH_NODE_BMS,        NULL, NULL },
    { "POD1",      OPENDASH_NODE_POD1,       NULL, NULL },
    { "POD2",      OPENDASH_NODE_POD2,       NULL, NULL },
    { "RELAY-4CH", OPENDASH_NODE_RELAY_4CH,  NULL, NULL },
    { "RELAY-8A",  OPENDASH_NODE_RELAY_8CH_A,NULL, NULL },
    { "RELAY-8B",  OPENDASH_NODE_RELAY_8CH_B,NULL, NULL },
    { "MOS-A",     OPENDASH_NODE_MOS_4CH_A,  NULL, NULL },
    { "MOS-B",     OPENDASH_NODE_MOS_4CH_B,  NULL, NULL },
};

static void config_ota_blink_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    config_ota_blink_visible = !config_ota_blink_visible;
    /* Only repaint chips currently in OTA — do NOT redraw the entire grid.
     * (Full grid refresh happens on every STATUS_REPORT in
     * ui_manager_update_config_node_status; we don't need to duplicate that
     * work here.) */
    static const opendash_node_t node_map[] = {
        OPENDASH_NODE_LEFT, OPENDASH_NODE_RIGHT, OPENDASH_NODE_GPS,
        OPENDASH_NODE_BMS, OPENDASH_NODE_POD1, OPENDASH_NODE_POD2,
        OPENDASH_NODE_RELAY_4CH, OPENDASH_NODE_RELAY_8CH_A,
        OPENDASH_NODE_RELAY_8CH_B, OPENDASH_NODE_MOS_4CH_A,
        OPENDASH_NODE_MOS_4CH_B,
    };
    for (int i = 0; i < CONFIG_NODE_COUNT; i++) {
        if (!config_nodes[i].status_label || !config_nodes[i].box) continue;
        if ((node_health_get_status_flags(node_map[i]) &
             OPENDASH_STATUS_FLAG_BLE_OTA) == 0) continue;
        lv_obj_set_style_text_color(config_nodes[i].status_label,
            lv_color_hex(config_ota_blink_visible ? 0x00CCFF : 0x004466), 0);
        lv_obj_set_style_border_color(config_nodes[i].box,
            lv_color_hex(config_ota_blink_visible ? 0x00CCFF : 0x003355), 0);
    }
}

static int config_selected_node = -1;

static void config_node_touch_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= CONFIG_NODE_COUNT) return;

    config_selected_node = idx;

    /* Update selection highlight */
    for (int i = 0; i < CONFIG_NODE_COUNT; i++) {
        lv_obj_t *box = lv_obj_get_parent(config_nodes[i].status_label);
        if (i == idx) {
            lv_obj_set_style_border_color(box, lv_color_hex(0x00AAFF), 0);
            lv_obj_set_style_border_width(box, 3, 0);
        } else {
            lv_obj_set_style_border_color(box, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_border_width(box, 1, 0);
        }
    }
    ESP_LOGI(TAG, "Config: selected node %s", config_nodes[idx].name);
}

/* True if @p node has firmware that handles OPENDASH_SUBCMD_ENTER_BT_OTA.
 *
 * Kept in sync with the slave `case OPENDASH_SUBCMD_ENTER_BT_OTA:` branches in
 * each <node>/main/main.c. If you add OTA to a new node, list it here so the
 * Config-screen OTA FLASH button will actually send the message instead of
 * silently refusing. BMS is excluded because that firmware lives in the
 * separate multidisplay-firmware/MDLogger repo (PlatformIO) and uses its own
 * OTA path. */
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

/* True if @p node has firmware that handles OPENDASH_SUBCMD_SELF_TEST. */
static bool node_supports_self_test(opendash_node_t node)
{
    switch (node) {
        case OPENDASH_NODE_RELAY_8CH_A:
        case OPENDASH_NODE_RELAY_8CH_B:
            return true;
        default:
            return false;
    }
}

static void config_set_status(const char *fmt, ...)
{
    if (!config_action_status) return;
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    lv_label_set_text(config_action_status, buf);
}

static void config_ota_btn_cb(lv_event_t *e)
{
    (void)e;
    if (config_selected_node < 0) {
        config_set_status("Select a node first, then tap OTA FLASH.");
        return;
    }
    opendash_node_t node = config_nodes[config_selected_node].node;
    const char *name     = config_nodes[config_selected_node].name;
    ESP_LOGW(TAG, "Config: OTA Flash → %s", name);
    if (!node_supports_ota(node)) {
        config_set_status("%s firmware has no OTA handler yet (MOS-A/B + RELAY-8A/B only).", name);
        ESP_LOGW(TAG, "OTA target %s lacks ENTER_BT_OTA handler", name);
        return;
    }
    /* ESP-NOW can drop a single control frame on a busy link, and LEFT/RIGHT
     * spend long stretches inside LVGL paints where their RX task lags.
     * Burst several sends so ENTER_BT_OTA is overwhelmingly likely to land
     * on the first tap. Each force_send already does up to 5 internal radio
     * retries, so this loop is the "did the recipient process it?" layer. */
    esp_err_t last_err = ESP_FAIL;
    int sent_ok = 0;
    for (int i = 0; i < 6; ++i) {
        esp_err_t err = espnow_master_send_system_subcmd(node, OPENDASH_SUBCMD_ENTER_BT_OTA);
        last_err = err;
        if (err == ESP_OK) sent_ok++;
        vTaskDelay(pdMS_TO_TICKS(60));
    }

    if (sent_ok > 0) {
        config_set_status("Sent ENTER_BT_OTA x%d to %s — watch for OpenDash-%s-OTA over BLE.",
                          sent_ok, name, name);
    } else {
        config_set_status("OTA send failed for %s (err=0x%x) — node may be offline or MAC not yet learned.",
                          name, last_err);
    }
}

/* Legacy CONFIG-row Self-Test handler. Button was relocated into the Device
 * Management submenu (devmgmt_test_btn_cb) per TODO #11; kept callable in case
 * a CONFIG-grid SELF-TEST is re-added later. */
__attribute__((unused))
static void config_test_btn_cb(lv_event_t *e)
{
    (void)e;
    if (config_selected_node < 0) {
        config_set_status("Select a node first, then tap SELF-TEST.");
        return;
    }
    opendash_node_t node = config_nodes[config_selected_node].node;
    const char *name     = config_nodes[config_selected_node].name;
    ESP_LOGI(TAG, "Config: Self-Test → %s", name);
    if (!node_supports_self_test(node)) {
        config_set_status("%s firmware has no SELF_TEST handler yet (RELAY-8A/B only).", name);
        return;
    }
    esp_err_t err = espnow_master_send_system_subcmd(node, OPENDASH_SUBCMD_SELF_TEST);
    config_set_status(err == ESP_OK ? "Sent SELF_TEST to %s." : "SELF_TEST send failed for %s (err=0x%x).",
                      name, err);
}static void config_reboot_btn_cb(lv_event_t *e)
{
    (void)e;
    if (config_selected_node < 0) {
        config_set_status("Select a node first, then tap REBOOT.");
        return;
    }
    opendash_node_t node = config_nodes[config_selected_node].node;
    const char *name     = config_nodes[config_selected_node].name;
    ESP_LOGW(TAG, "Config: Reboot → %s", name);
    esp_err_t err = espnow_master_send_system_subcmd(node, OPENDASH_SUBCMD_REBOOT);
    config_set_status(err == ESP_OK ? "Sent REBOOT to %s." : "REBOOT send failed for %s (err=0x%x).",
                      name, err);
}

static void config_debug_btn_cb(lv_event_t *e)
{
    (void)e;
    s_debug_mode_active = true;
    update_display_mode(DISPLAY_MODE_RELAY);
    ESP_LOGI(TAG, "Config: Entering Debug mode → RELAY screen");
}

/* ── OBD2 Config Submenu Callbacks ────────────────────────────────────── */

static void destroy_obd_config_screen(void);
static void create_obd_config_screen(lv_obj_t *parent);

static void obd_config_back_cb(lv_event_t *e)
{
    (void)e;
    destroy_obd_config_screen();
    ESP_LOGI(TAG, "OBD config: returning to system config");
}

static void obd_enable_toggle_cb(lv_event_t *e)
{
    (void)e;
    bool new_state = obd_config_toggle_enabled();
    if (obd_enable_btn_label) {
        lv_label_set_text(obd_enable_btn_label,
                          new_state ? "OBD PAGE: ON" : "OBD PAGE: OFF");
    }
    ESP_LOGI(TAG, "OBD config: page %s", new_state ? "enabled" : "disabled");
}

static void obd_mil_toggle_cb(lv_event_t *e)
{
    (void)e;
    bool new_state = obd_config_toggle_mil_indicator();
    if (obd_mil_btn_label) {
        lv_label_set_text(obd_mil_btn_label,
                          new_state ? "MIL INDICATOR: ON" : "MIL INDICATOR: OFF");
    }
    ESP_LOGI(TAG, "OBD config: MIL indicator %s", new_state ? "enabled" : "disabled");
}

static void obd_clear_dtc_cb(lv_event_t *e)
{
    (void)e;
    esp_err_t ret = espnow_master_send_obd_command(0x43);
    if (ret == ESP_OK) {
        if (obd_dtc_list_label) {
            lv_label_set_text(obd_dtc_list_label, "DTC clear sent via LEFT pod — waiting...");
        }
        ESP_LOGW(TAG, "OBD config: DTC clear relayed via ESP-NOW");
    } else {
        if (obd_dtc_list_label) {
            lv_label_set_text(obd_dtc_list_label, "LEFT pod offline — cannot clear DTCs");
        }
        ESP_LOGW(TAG, "OBD config: DTC clear failed (LEFT offline)");
    }
}

static void obd_read_dtc_cb(lv_event_t *e)
{
    (void)e;
    esp_err_t ret = espnow_master_send_obd_command(0x44);
    if (ret == ESP_OK) {
        if (obd_dtc_list_label) {
            lv_label_set_text(obd_dtc_list_label, "DTCs: Reading from ECU...");
        }
        ESP_LOGI(TAG, "OBD config: DTC read request relayed via ESP-NOW");
    } else {
        if (obd_dtc_list_label) {
            lv_label_set_text(obd_dtc_list_label, "LEFT pod offline — cannot read DTCs");
        }
        ESP_LOGW(TAG, "OBD config: DTC read failed (LEFT offline)");
    }
}

static void obd_request_vin_cb(lv_event_t *e)
{
    (void)e;
    esp_err_t ret = espnow_master_send_obd_command(0x56);
    if (ret == ESP_OK) {
        if (obd_vin_label) {
            lv_label_set_text(obd_vin_label, "VIN: request sent via LEFT pod...");
        }
        ESP_LOGI(TAG, "OBD config: VIN request relayed via ESP-NOW");
    } else {
        ESP_LOGW(TAG, "OBD config: VIN request failed (LEFT offline)");
    }
}

static void boost_config_ui_closed_cb(void)
{
    /* Boost editor was a modal over the config grid; restore visibility so
     * the user lands back on System Config instead of a blank screen. */
    if (config_grid_container) {
        lv_obj_clear_flag(config_grid_container, LV_OBJ_FLAG_HIDDEN);
    }
    if (config_action_status) {
        lv_label_set_text(config_action_status,
                          "Boost editor closed.  Select node \xe2\x86\x92 tap action.");
    }
}

static void boost_config_open_async(void *user_data)
{
    (void)user_data;
    boost_config_ui_create(screen_layout.screen);
}

static void config_boost_btn_cb(lv_event_t *e)
{
    (void)e;
    /* Hide the SYSTEM CONFIG grid behind the editor so taps don't bleed through */
    if (config_grid_container) {
        lv_obj_add_flag(config_grid_container, LV_OBJ_FLAG_HIDDEN);
    }
    /* Make sure the close callback is wired so we re-show the grid on exit. */
    boost_config_ui_set_close_cb(boost_config_ui_closed_cb);
    /* Defer the heavy boost-UI build to the next LVGL tick so the click event
     * fully finishes first. Building synchronously inside the indev release
     * handler corrupts the style-transition linked list (dangling entry for
     * this BOOST button), causing lv_obj_add_style to loop forever when the
     * 50th-ish child is themed. Mirrors layout_editor.c open_picker_modal_async. */
    lv_async_call(boost_config_open_async, NULL);
}

static void config_obd_btn_cb(lv_event_t *e)
{
    (void)e;
    /* Show OBD config submenu on top of config grid */
    if (obd_config_container == NULL) {
        create_obd_config_screen(screen_layout.screen);
    }
    ESP_LOGI(TAG, "Config: Opening OBD2 Setup");
}

/* ── Data Source Config Submenu ──────────────────────────────────────────── */
static lv_obj_t *datasrc_config_container = NULL;

/** @brief Active data source identifier stored in NVS */
typedef enum {
    OPENDASH_DATASRC_DEMO       = 0,   /**< Built-in drag-race demo data */
    OPENDASH_DATASRC_MULTIDISPLAY = 1, /**< MultiDisplay via UART (native sensors) */
    OPENDASH_DATASRC_OBD2       = 2,   /**< MultiDisplay OBD-II data */
    OPENDASH_DATASRC_BMS        = 3,   /**< rAtTrax BMS */
    OPENDASH_DATASRC_CAN_DIRECT = 4,   /**< Direct CAN bus (future) */
    OPENDASH_DATASRC_VESC       = 5,   /**< VESC motor controller */
    OPENDASH_DATASRC_COUNT      = 6
} opendash_datasrc_t;

static const char *datasrc_names[OPENDASH_DATASRC_COUNT] = {
    "DEMO DATA",
    "MULTIDISPLAY",
    "OBD-II (via MD)",
    "rAtTrax BMS",
    "DIRECT CAN",
    "VESC MOTOR",
};
static const uint32_t datasrc_colors[OPENDASH_DATASRC_COUNT] = {
    0x7B2FBE, 0x2E7D32, 0x0077B6, 0xFF6F00, 0xE94560, 0x4ECCA3
};

static opendash_datasrc_t s_active_datasrc = OPENDASH_DATASRC_DEMO;

static void datasrc_select_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= OPENDASH_DATASRC_COUNT) return;
    s_active_datasrc = (opendash_datasrc_t)idx;
    ESP_LOGI(TAG, "Data source changed to: %s", datasrc_names[idx]);
    /* Rebuild the submenu to show selection */
    if (datasrc_config_container) {
        lv_obj_del(datasrc_config_container);
        datasrc_config_container = NULL;
    }
    /* Return to config */
    if (config_grid_container) {
        lv_obj_clear_flag(config_grid_container, LV_OBJ_FLAG_HIDDEN);
    }
}

static void datasrc_back_cb(lv_event_t *e)
{
    (void)e;
    if (datasrc_config_container) {
        lv_obj_del(datasrc_config_container);
        datasrc_config_container = NULL;
    }
    if (config_grid_container) {
        lv_obj_clear_flag(config_grid_container, LV_OBJ_FLAG_HIDDEN);
    }
}

static void config_datasrc_btn_cb(lv_event_t *e)
{
    (void)e;
    if (config_grid_container) {
        lv_obj_add_flag(config_grid_container, LV_OBJ_FLAG_HIDDEN);
    }

    datasrc_config_container = lv_obj_create(screen_layout.screen);
    lv_obj_set_size(datasrc_config_container, LCD_H_RES, LCD_V_RES - STATUS_BAR_HEIGHT - 20);
    lv_obj_align(datasrc_config_container, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(datasrc_config_container, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(datasrc_config_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(datasrc_config_container, 0, 0);
    lv_obj_set_style_pad_all(datasrc_config_container, 15, 0);
    lv_obj_clear_flag(datasrc_config_container, LV_OBJ_FLAG_SCROLLABLE);

#if HAS_BACKGROUND_IMAGE
    lv_obj_t *bg_img = lv_image_create(datasrc_config_container);
    lv_image_set_src(bg_img, &background_center_dsc);
    lv_obj_set_size(bg_img, LCD_H_RES, LCD_V_RES);
    lv_obj_align(bg_img, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_img_opa(bg_img, 76, 0);
    lv_obj_move_to_index(bg_img, 0);
#endif

    lv_obj_t *title = create_simple_label(datasrc_config_container, "DATA SOURCE SELECT",
                                           OPENDASH_FONT_SIZE_LARGE, 0x00AAFF);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 10, 5);

    /* Back button */
    lv_obj_t *btn_back = lv_btn_create(datasrc_config_container);
    lv_obj_set_size(btn_back, 120, 45);
    lv_obj_align(btn_back, LV_ALIGN_TOP_RIGHT, -10, 0);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(btn_back, 8, 0);
    lv_obj_add_event_cb(btn_back, datasrc_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = create_simple_label(btn_back, "\xe2\x86\x90 BACK",
                                              OPENDASH_FONT_SIZE_MEDIUM, 0xFFFFFF);
    lv_obj_center(back_lbl);

    /* Data source buttons — 2 rows × 3 columns */
    const int ds_btn_w = 220;
    const int ds_btn_h = 80;
    const int ds_spacing = 15;
    const int ds_start_y = 65;

    for (int i = 0; i < OPENDASH_DATASRC_COUNT; i++) {
        int row = i / 3;
        int col = i % 3;
        int x = 20 + col * (ds_btn_w + ds_spacing);
        int y = ds_start_y + row * (ds_btn_h + ds_spacing);

        lv_obj_t *btn = lv_btn_create(datasrc_config_container);
        lv_obj_set_size(btn, ds_btn_w, ds_btn_h);
        lv_obj_set_pos(btn, x, y);
        lv_obj_set_style_radius(btn, 12, 0);

        bool active = ((int)s_active_datasrc == i);
        lv_obj_set_style_bg_color(btn, lv_color_hex(active ? datasrc_colors[i] : 0x333333), 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(active ? 0xFFFFFF : 0x555555), 0);
        lv_obj_set_style_border_width(btn, active ? 3 : 1, 0);

        lv_obj_add_event_cb(btn, datasrc_select_cb, LV_EVENT_CLICKED,
                             (void *)(intptr_t)i);

        lv_obj_t *lbl = create_simple_label(btn, datasrc_names[i],
                                             OPENDASH_FONT_SIZE_LARGE, 0xFFFFFF);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -8);

        if (active) {
            lv_obj_t *act = create_simple_label(btn, "\xe2\x9c\x93 ACTIVE",
                                                 OPENDASH_FONT_SIZE_SMALL, 0x00FF00);
            lv_obj_align(act, LV_ALIGN_BOTTOM_MID, 0, -4);
        }
    }

    /* Info text */
    const char *info = "Select the primary data source for engine/vehicle data.\n"
                       "Multiple sources can be active simultaneously — this sets priority.";
    lv_obj_t *info_lbl = create_simple_label(datasrc_config_container, info,
                                              OPENDASH_FONT_SIZE_SMALL, 0xAAAAAA);
    lv_obj_set_pos(info_lbl, 20, ds_start_y + 2 * (ds_btn_h + ds_spacing) + 10);
    lv_obj_set_width(info_lbl, LCD_H_RES - 60);

    ESP_LOGI(TAG, "Data source config screen created (active=%s)",
             datasrc_names[s_active_datasrc]);
}

/* ── Device Management Config Submenu ────────────────────────────────────── */
static lv_obj_t *devmgmt_config_container = NULL;
static lv_obj_t *devmgmt_action_status    = NULL;  /* Bottom-row feedback line */

static void devmgmt_set_status(const char *fmt, ...)
{
    if (!devmgmt_action_status) return;
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    lv_label_set_text(devmgmt_action_status, buf);
}

static void devmgmt_test_btn_cb(lv_event_t *e)
{
    (void)e;
    /* Reuses the CONFIG screen's selection so the user can pick a node on the
     * main config grid and then come into Device Mgmt to fire SELF-TEST. */
    if (config_selected_node < 0) {
        devmgmt_set_status("Pick a node from CONFIG first, then come back here to SELF-TEST.");
        return;
    }
    opendash_node_t node = config_nodes[config_selected_node].node;
    const char *name     = config_nodes[config_selected_node].name;
    ESP_LOGI(TAG, "DevMgmt: Self-Test → %s", name);
    if (!node_supports_self_test(node)) {
        devmgmt_set_status("%s firmware has no SELF_TEST handler yet (RELAY-8A/B only).", name);
        return;
    }
    esp_err_t err = espnow_master_send_system_subcmd(node, OPENDASH_SUBCMD_SELF_TEST);
    devmgmt_set_status(err == ESP_OK ? "Sent SELF_TEST to %s." : "SELF_TEST send failed for %s (err=0x%x).",
                       name, err);
}

static void devmgmt_back_cb(lv_event_t *e)
{
    (void)e;
    if (devmgmt_config_container) {
        lv_obj_del(devmgmt_config_container);
        devmgmt_config_container = NULL;
        devmgmt_action_status    = NULL;
    }
    if (config_grid_container) {
        lv_obj_clear_flag(config_grid_container, LV_OBJ_FLAG_HIDDEN);
    }
}

static void devmgmt_layout_editor_cb(lv_event_t *e)
{
    (void)e;
    /* layout_editor_open() builds ~30 widgets including 7 dropdowns wired
     * to the full DP catalog. Done inline it monopolises ui_task for
     * 5+ seconds, freezes the LVGL flush callback, trips the IDLE1
     * task watchdog, and the user sees a frozen + tearing screen
     * (perceived as a crash).
     *
     * Defer the open onto the next LVGL tick via lv_async_call so this
     * touch event returns immediately. The UI repaints (giving visual
     * feedback that the tap registered), THEN the editor builds on the
     * following tick — at which point ui_task is no longer holding any
     * LVGL event-dispatch state and the build can yield freely. */
    lv_async_call((void (*)(void *))layout_editor_open, lv_scr_act());
}

/* ════════════════════════════════════════════════════════════════════════════
 *  DEPLOYMENT SYSTEM panel  (inside DEVICE MGMT, gated on MOS-A / MOS-B)
 *
 *  Lets the user configure a MOS node's parachute/deployment settings:
 *  enable, which CH1..CH4 fire on deploy, and the 5 detection tunables.
 *  Center pushes the config to the MOS (which persists it) and shows the
 *  MOS's live STATUS echo. ARM lives on the home screen (Slice 3), not here.
 * ════════════════════════════════════════════════════════════════════════════ */

typedef enum {
    DEPLOY_FIELD_MIN_SPEED = 0,
    DEPLOY_FIELD_ROLL_DEG,
    DEPLOY_FIELD_ROLL_RATE,
    DEPLOY_FIELD_SUSTAIN,
    DEPLOY_FIELD_PULSE,
    DEPLOY_FIELD_COUNT,
} deploy_field_t;

static lv_obj_t   *deploy_container   = NULL;
static lv_obj_t   *deploy_target_btn[2] = { NULL };  /* MOS-A, MOS-B */
static lv_obj_t   *deploy_enable_btn  = NULL;
static lv_obj_t   *deploy_enable_lbl  = NULL;
static lv_obj_t   *deploy_chan_btn[4] = { NULL };
static lv_obj_t   *deploy_firemode_btn = NULL;  /* latch vs pulse toggle */
static lv_obj_t   *deploy_firemode_lbl = NULL;
static lv_obj_t   *deploy_auto_btn    = NULL;   /* AUTO_DETECT opt-in toggle */
static lv_obj_t   *deploy_auto_lbl    = NULL;
static lv_obj_t   *deploy_fire_btn   = NULL;   /* manual HOLD-to-deploy */
static lv_obj_t   *deploy_fire_lbl   = NULL;
static lv_obj_t   *deploy_val_lbl[DEPLOY_FIELD_COUNT] = { NULL };
static lv_obj_t   *deploy_status_lbl  = NULL;   /* push/echo feedback */
static lv_obj_t   *deploy_live_lbl    = NULL;   /* MOS-reported live state */
static lv_timer_t *deploy_poll_timer  = NULL;
static opendash_parachute_config_t deploy_cfg;
static opendash_node_t deploy_node   = OPENDASH_NODE_MOS_4CH_A;
static const char     *deploy_node_name = "MOS-A";
static bool            deploy_seeded  = false;   /* first echo seeds the editor */

/* Push reconciliation: pressing PUSH commits the user's intent. The center
 * then continuously drives the MOS to this EXACT config and confirms only when
 * the MOS echoes it back (0x95). esp_now_send returning OK just means queued,
 * not delivered — and a safety deploy screen must never tell the user to
 * "press again", so there is no give-up timeout: it self-heals until confirmed
 * or the user changes target / refreshes / leaves. */
static bool                        deploy_push_pending    = false;
static uint32_t                    deploy_push_started_ms = 0;   /* when PUSH pressed   */
static uint32_t                    deploy_push_resend_ms  = 0;   /* next auto re-send   */
static opendash_parachute_config_t deploy_push_expect;          /* committed snapshot  */

/* REFRESH uses the same self-resolving loop: solicit the MOS's persisted
 * config and show an animated loading state until a status echo arrives, then
 * seed the editor from it. Never a static "requesting..." that hangs. */
static bool                        deploy_refresh_pending    = false;
static uint32_t                    deploy_refresh_started_ms = 0;
static uint32_t                    deploy_refresh_resend_ms  = 0;

/* ── Home status-bar ARM latch ───────────────────────────────────────────
 * Global arm/disarm for the parachute actuator(s), reachable from any screen
 * via the persistent status bar. ARM is a latched INTENT the center reconciles
 * to every online MOS exactly like config: send SET_ARM -> wait for the MOS to
 * echo armed==intent -> confirm. Safety rules:
 *   - ARMING requires a deliberate LONG-PRESS (hold ~1s); a short TAP disarms.
 *   - DISARM always wins instantly and is re-sent until every MOS echoes safe.
 *   - Intent defaults SAFE and is never persisted — a center reboot comes up
 *     DISARMED, and each MOS independently boots DISARMED too.
 *   - Firing stays inhibited until Slice 4; arming here only latches the flag. */
static lv_obj_t   *home_arm_btn      = NULL;
static lv_obj_t   *home_arm_lbl      = NULL;
static lv_timer_t *home_arm_timer    = NULL;
static bool        home_arm_intent   = false;  /* desired armed state (SAFE default) */
static bool        home_arm_pending  = false;  /* reconciling to intent              */
static uint32_t    home_arm_resend_ms = 0;     /* next auto re-send                  */

/* MOS nodes that can hold the deploy actuator. */
static const opendash_node_t home_arm_nodes[] = {
    OPENDASH_NODE_MOS_4CH_A,
    OPENDASH_NODE_MOS_4CH_B,
};
#define HOME_ARM_NODE_COUNT (sizeof(home_arm_nodes) / sizeof(home_arm_nodes[0]))

/* Keypad modal */
static lv_obj_t       *deploy_kp_root = NULL;
static lv_obj_t       *deploy_kp_ta   = NULL;
static deploy_field_t  deploy_kp_field = DEPLOY_FIELD_MIN_SPEED;

static const char *deploy_field_name(deploy_field_t f)
{
    switch (f) {
        case DEPLOY_FIELD_MIN_SPEED: return "MIN SPEED";
        case DEPLOY_FIELD_ROLL_DEG:  return "ROLL ANGLE";
        case DEPLOY_FIELD_ROLL_RATE: return "ROLL RATE";
        case DEPLOY_FIELD_SUSTAIN:   return "SUSTAIN";
        case DEPLOY_FIELD_PULSE:     return "PULSE";
        default:                     return "?";
    }
}

static void deploy_field_text(deploy_field_t f, char *buf, size_t n)
{
    switch (f) {
        case DEPLOY_FIELD_MIN_SPEED:
            snprintf(buf, n, "MIN SPEED\n%.0f mph", deploy_cfg.min_speed_mph); break;
        case DEPLOY_FIELD_ROLL_DEG:
            snprintf(buf, n, "ROLL ANGLE\n%.0f deg", deploy_cfg.roll_deploy_deg); break;
        case DEPLOY_FIELD_ROLL_RATE:
            snprintf(buf, n, "ROLL RATE\n%.0f deg/s", deploy_cfg.roll_rate_deg_s); break;
        case DEPLOY_FIELD_SUSTAIN:
            snprintf(buf, n, "SUSTAIN\n%u ms", deploy_cfg.sustain_ms); break;
        case DEPLOY_FIELD_PULSE:
            snprintf(buf, n, "PULSE\n%u ms", deploy_cfg.pulse_ms); break;
        default: buf[0] = '\0'; break;
    }
}

static float deploy_field_value(deploy_field_t f)
{
    switch (f) {
        case DEPLOY_FIELD_MIN_SPEED: return deploy_cfg.min_speed_mph;
        case DEPLOY_FIELD_ROLL_DEG:  return deploy_cfg.roll_deploy_deg;
        case DEPLOY_FIELD_ROLL_RATE: return deploy_cfg.roll_rate_deg_s;
        case DEPLOY_FIELD_SUSTAIN:   return (float)deploy_cfg.sustain_ms;
        case DEPLOY_FIELD_PULSE:     return (float)deploy_cfg.pulse_ms;
        default:                     return 0.0f;
    }
}

static void deploy_field_apply(deploy_field_t f, float v)
{
    if (v < 0.0f) v = 0.0f;
    switch (f) {
        case DEPLOY_FIELD_MIN_SPEED: deploy_cfg.min_speed_mph  = (v > 500.0f)  ? 500.0f  : v; break;
        case DEPLOY_FIELD_ROLL_DEG:  deploy_cfg.roll_deploy_deg = (v > 180.0f)  ? 180.0f  : v; break;
        case DEPLOY_FIELD_ROLL_RATE: deploy_cfg.roll_rate_deg_s = (v > 2000.0f) ? 2000.0f : v; break;
        case DEPLOY_FIELD_SUSTAIN:   deploy_cfg.sustain_ms = (uint16_t)((v > 5000.0f)  ? 5000  : v); break;
        case DEPLOY_FIELD_PULSE:     deploy_cfg.pulse_ms   = (uint16_t)((v > 10000.0f) ? 10000 : v); break;
        default: break;
    }
}

static void deploy_set_status(const char *fmt, ...);

static void deploy_refresh_widgets(void)
{
    if (!deploy_container) return;

    /* Channels the boost controller has claimed on the same MOS are off-limits
     * to the safety system. Defensively drop any overlap so the two subsystems
     * can run in tandem without fighting over a FET (committed-wins). */
    uint8_t boost_reserved = boost_client_reserved_mask(deploy_node);
    if (deploy_cfg.channel_mask & boost_reserved) {
        deploy_cfg.channel_mask &= (uint8_t)~boost_reserved;
    }

    for (int i = 0; i < 2; i++) {
        if (!deploy_target_btn[i]) continue;
        bool sel = (i == 0) ? (deploy_node == OPENDASH_NODE_MOS_4CH_A)
                            : (deploy_node == OPENDASH_NODE_MOS_4CH_B);
        lv_obj_set_style_bg_color(deploy_target_btn[i],
            lv_color_hex(sel ? 0xFF6F00 : 0x444444), 0);
    }
    if (deploy_enable_lbl) {
        lv_label_set_text(deploy_enable_lbl,
                          deploy_cfg.enabled ? "ENABLE: ON" : "ENABLE: OFF");
    }
    if (deploy_enable_btn) {
        lv_obj_set_style_bg_color(deploy_enable_btn,
            lv_color_hex(deploy_cfg.enabled ? 0x118811 : 0x555555), 0);
    }
    for (int c = 0; c < 4; c++) {
        if (!deploy_chan_btn[c]) continue;
        bool reserved = (boost_reserved >> c) & 0x1;
        bool on = (deploy_cfg.channel_mask >> c) & 0x1;
        if (reserved) {
            /* Channel owned by the boost controller on this MOS — lock it out. */
            lv_obj_add_state(deploy_chan_btn[c], LV_STATE_DISABLED);
            lv_obj_set_style_bg_color(deploy_chan_btn[c], lv_color_hex(0x2A2A2A), 0);
            lv_obj_set_style_bg_opa(deploy_chan_btn[c], LV_OPA_70, 0);
        } else {
            lv_obj_remove_state(deploy_chan_btn[c], LV_STATE_DISABLED);
            lv_obj_set_style_bg_opa(deploy_chan_btn[c], LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(deploy_chan_btn[c],
                lv_color_hex(on ? 0xCC2222 : 0x444444), 0);
        }
    }
    if (deploy_firemode_btn && deploy_firemode_lbl) {
        bool pulse = (deploy_cfg.flags & OPENDASH_PARACHUTE_FLAG_FIRE_PULSE) != 0;
        lv_label_set_text(deploy_firemode_lbl, pulse ? "FIRE: PULSE" : "FIRE: LATCH");
        lv_obj_set_style_bg_color(deploy_firemode_btn,
            lv_color_hex(pulse ? 0x884400 : 0x333355), 0);
    }
    if (deploy_auto_btn && deploy_auto_lbl) {
        bool autod = (deploy_cfg.flags & OPENDASH_PARACHUTE_FLAG_AUTO_DETECT) != 0;
        lv_label_set_text(deploy_auto_lbl, autod ? "AUTO: ON" : "AUTO: OFF");
        lv_obj_set_style_bg_color(deploy_auto_btn,
            lv_color_hex(autod ? 0x006622 : 0x333333), 0);
    }
    for (int f = 0; f < DEPLOY_FIELD_COUNT; f++) {
        if (!deploy_val_lbl[f]) continue;
        char buf[40];
        deploy_field_text((deploy_field_t)f, buf, sizeof(buf));
        lv_label_set_text(deploy_val_lbl[f], buf);
    }
}

/* ── Keypad ─────────────────────────────────────────────────────────────── */
static void deploy_kp_close(void)
{
    if (deploy_kp_root) {
        lv_obj_del(deploy_kp_root);
        deploy_kp_root = NULL;
        deploy_kp_ta   = NULL;
    }
}

static void deploy_kp_ok_cb(lv_event_t *e)
{
    (void)e;
    if (deploy_kp_ta) {
        const char *txt = lv_textarea_get_text(deploy_kp_ta);
        deploy_field_apply(deploy_kp_field, (float)atof(txt));
        deploy_seeded = true;   /* user edit wins over MOS echo-seed */
        deploy_refresh_widgets();
    }
    deploy_kp_close();
}

static void deploy_kp_cancel_cb(lv_event_t *e)
{
    (void)e;
    deploy_kp_close();
}

static void deploy_kp_kbd_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY)  deploy_kp_ok_cb(e);
    if (code == LV_EVENT_CANCEL) deploy_kp_cancel_cb(e);
}

static void deploy_val_btn_cb(lv_event_t *e)
{
    deploy_field_t f = (deploy_field_t)(intptr_t)lv_event_get_user_data(e);
    deploy_kp_close();
    deploy_kp_field = f;

    deploy_kp_root = lv_obj_create(lv_layer_top());
    lv_obj_set_size(deploy_kp_root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(deploy_kp_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(deploy_kp_root, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(deploy_kp_root, 8, 0);
    lv_obj_set_layout(deploy_kp_root, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(deploy_kp_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(deploy_kp_root, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(deploy_kp_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(deploy_kp_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(deploy_kp_root, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *title = lv_label_create(deploy_kp_root);
    lv_label_set_text_fmt(title, "%s — enter value", deploy_field_name(f));
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);

    deploy_kp_ta = lv_textarea_create(deploy_kp_root);
    lv_obj_set_width(deploy_kp_ta, 240);
    lv_textarea_set_one_line(deploy_kp_ta, true);
    lv_textarea_set_accepted_chars(deploy_kp_ta, "0123456789.");
    char preload[16];
    snprintf(preload, sizeof(preload), "%.0f", deploy_field_value(f));
    lv_textarea_set_text(deploy_kp_ta, preload);

    lv_obj_t *btn_row = lv_obj_create(deploy_kp_root);
    lv_obj_set_size(btn_row, 260, LV_SIZE_CONTENT);
    lv_obj_set_layout(btn_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *btn_ok = lv_button_create(btn_row);
    lv_obj_add_event_cb(btn_ok, deploy_kp_ok_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_ok = lv_label_create(btn_ok); lv_label_set_text(lbl_ok, "OK");
    lv_obj_center(lbl_ok);

    lv_obj_t *btn_cancel = lv_button_create(btn_row);
    lv_obj_add_event_cb(btn_cancel, deploy_kp_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_cancel = lv_label_create(btn_cancel); lv_label_set_text(lbl_cancel, "Cancel");
    lv_obj_center(lbl_cancel);

    lv_obj_t *kbd = lv_keyboard_create(deploy_kp_root);
    lv_keyboard_set_mode(kbd, LV_KEYBOARD_MODE_NUMBER);
    lv_keyboard_set_textarea(kbd, deploy_kp_ta);
    lv_obj_set_width(kbd, lv_pct(95));
    lv_obj_set_height(kbd, 200);
    lv_obj_add_event_cb(kbd, deploy_kp_kbd_cb, LV_EVENT_READY,  NULL);
    lv_obj_add_event_cb(kbd, deploy_kp_kbd_cb, LV_EVENT_CANCEL, NULL);
}

/* ── Toggle / action callbacks ───────────────────────────────────────────── */
static void deploy_enable_cb(lv_event_t *e)
{
    (void)e;
    deploy_cfg.enabled = deploy_cfg.enabled ? 0 : 1;
    deploy_seeded = true;   /* user edit wins over MOS echo-seed */
    deploy_refresh_widgets();
}

static void deploy_chan_cb(lv_event_t *e)
{
    int c = (int)(intptr_t)lv_event_get_user_data(e);
    if (c < 0 || c > 3) return;
    if (boost_client_reserved_mask(deploy_node) & (1u << c)) {
        deploy_set_status("CH%d reserved by boost controller on %s.",
                          c + 1, deploy_node_name);
        deploy_refresh_widgets();
        return;
    }
    deploy_cfg.channel_mask ^= (uint8_t)(1u << c);
    deploy_cfg.channel_mask &= 0x0F;
    deploy_seeded = true;   /* user edit wins over MOS echo-seed */
    deploy_refresh_widgets();
}

static void deploy_firemode_cb(lv_event_t *e)
{
    (void)e;
    deploy_cfg.flags ^= OPENDASH_PARACHUTE_FLAG_FIRE_PULSE;
    deploy_seeded = true;   /* user edit wins over MOS echo-seed */
    deploy_refresh_widgets();
}

/* AUTO_DETECT opt-in: arms the distributed gyro auto-deploy path. OFF by
 * default so autonomous fire is never enabled by accident; manual DEPLOY is
 * unaffected by this flag. */
static void deploy_auto_cb(lv_event_t *e)
{
    (void)e;
    deploy_cfg.flags ^= OPENDASH_PARACHUTE_FLAG_AUTO_DETECT;
    deploy_seeded = true;   /* user edit wins over MOS echo-seed */
    deploy_refresh_widgets();
}

static void deploy_set_status(const char *fmt, ...)
{
    if (!deploy_status_lbl) return;
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    lv_label_set_text(deploy_status_lbl, buf);
}

/* Mirror the deploy config to every gyro detector node (RIGHT/POD1/POD2) so
 * their local roll-detection uses the operator's chosen thresholds + the
 * AUTO_DETECT opt-in. Detectors persist + echo it; they are best-effort
 * recipients (the UI confirm gate stays on the MOS), and the autonomous quorum
 * still requires fresh `rolling` votes regardless. */
static void deploy_push_to_detectors(const opendash_parachute_config_t *cfg)
{
    static const opendash_node_t det[] = OPENDASH_ROLLOVER_DETECTORS;
    for (size_t i = 0; i < sizeof(det) / sizeof(det[0]); i++) {
        espnow_master_send_parachute_config(det[i], cfg);
    }
}

static void deploy_push_cb(lv_event_t *e)
{
    (void)e;
    deploy_cfg.version = OPENDASH_PARACHUTE_CONFIG_VERSION;
    deploy_seeded = true;  /* user-pushed config is authoritative on the UI */

    /* Commit the intent. From here the poll loop continuously reconciles the
     * MOS to this exact snapshot and confirms via the 0x95 echo — one press is
     * enough; the UI never asks the user to re-press. Even if this first send
     * can't queue right now, the reconciler will keep re-sending. */
    deploy_push_expect     = deploy_cfg;
    deploy_push_pending    = true;
    deploy_refresh_pending = false;
    deploy_push_started_ms = lv_tick_get();
    deploy_push_resend_ms  = deploy_push_started_ms + 1000;

    espnow_master_send_parachute_config(deploy_node, &deploy_push_expect);
    espnow_master_send_parachute_pull(deploy_node);
    deploy_push_to_detectors(&deploy_push_expect);
    deploy_set_status("Syncing to %s (ch=0x%X)\xe2\x80\xa6",
                      deploy_node_name, deploy_push_expect.channel_mask);
}

/* Manual DEPLOY — the top-priority fire path. Deliberate LONG-PRESS only, and
 * the center pre-checks the MOS is ARMED (via the cached 0x95 echo) before it
 * will even send the interlocked DEPLOY command; the MOS re-checks its own
 * interlock again before energizing anything. */
static void deploy_fire_cb(lv_event_t *e)
{
    (void)e;
    opendash_parachute_status_t st;
    bool have = espnow_master_get_parachute_status(deploy_node, &st);

    if (!espnow_master_node_online(deploy_node)) {
        deploy_set_status("DEPLOY blocked: %s offline.", deploy_node_name);
        return;
    }
    if (!have || !st.armed) {
        deploy_set_status("DEPLOY blocked: %s not ARMED (arm on home screen).",
                          deploy_node_name);
        return;
    }
    if (!st.cfg.enabled || st.cfg.channel_mask == 0) {
        deploy_set_status("DEPLOY blocked: %s disabled or no channel selected.",
                          deploy_node_name);
        return;
    }

    espnow_master_send_parachute_deploy(deploy_node);
    espnow_master_send_parachute_pull(deploy_node);
    deploy_set_status("\xe2\x9a\xa0 DEPLOY sent to %s (ch=0x%X)\xe2\x80\xa6",
                      deploy_node_name, st.cfg.channel_mask);
    ESP_LOGW(TAG, "Manual DEPLOY issued to %s", deploy_node_name);
}

static void deploy_refresh_cb(lv_event_t *e)
{
    (void)e;
    deploy_push_pending = false;
    deploy_seeded = false;  /* re-seed editor from the MOS's persisted config */
    deploy_refresh_pending    = true;
    deploy_refresh_started_ms = lv_tick_get();
    deploy_refresh_resend_ms  = deploy_refresh_started_ms + 1000;
    espnow_master_send_parachute_pull(deploy_node);
    deploy_set_status("Refreshing from %s\xe2\x80\xa6", deploy_node_name);
}

/* ZERO/CAL — capture each gyro detector's current resting roll as its zero
 * baseline (persisted on the node), so a non-level mount reads ~0 deg at rest
 * and a true rollover is measured relative to it. Broadcast to all detectors;
 * each confirms by NVS-saving + sending a fresh (level) vote that drops its
 * ROLL VOTES tally back to 0. Manual DEPLOY is unaffected. */
static void deploy_calibrate_cb(lv_event_t *e)
{
    (void)e;
    static const opendash_node_t det[] = OPENDASH_ROLLOVER_DETECTORS;
    for (size_t i = 0; i < sizeof(det) / sizeof(det[0]); i++) {
        espnow_master_send_parachute_calibrate(det[i]);
    }
    deploy_set_status("Zeroing detectors (RIGHT/POD1/POD2) to current resting angle\xe2\x80\xa6");
    ESP_LOGI(TAG, "ZERO/CAL sent to all rollover detectors");
}

static void deploy_poll_timer_cb(lv_timer_t *t)
{
    (void)t;
    opendash_parachute_status_t st;
    bool have_status = espnow_master_get_parachute_status(deploy_node, &st);

    /* Push reconciliation: success is the MOS echoing back the EXACT config we
     * sent (the struct is packed, so memcmp is authoritative). Until then keep
     * driving the MOS to it — re-send ~1Hz and show an honest animated working
     * state. No give-up, no "press again". */
    if (deploy_push_pending) {
        if (have_status &&
            memcmp(&st.cfg, &deploy_push_expect, sizeof(st.cfg)) == 0) {
            deploy_push_pending = false;
            deploy_set_status("\xe2\x9c\x93 %s CONFIRMED (ch=0x%X). Ready to ARM.",
                              deploy_node_name, st.cfg.channel_mask);
        } else {
            uint32_t now = lv_tick_get();
            if ((int32_t)(now - deploy_push_resend_ms) >= 0) {
                espnow_master_send_parachute_config(deploy_node, &deploy_push_expect);
                espnow_master_send_parachute_pull(deploy_node);
                deploy_push_to_detectors(&deploy_push_expect);
                deploy_push_resend_ms = now + 1000;
            }
            uint32_t waited = now - deploy_push_started_ms;
            static const char *spin[4] = { "", ".", "..", "..." };
            const char *d = spin[(waited / 350) % 4];
            bool online = espnow_master_node_online(deploy_node);
            if (!online && waited > 3000) {
                deploy_set_status("Connecting to %s%s   (syncing ch=0x%X)",
                                  deploy_node_name, d, deploy_push_expect.channel_mask);
            } else {
                deploy_set_status("Syncing to %s%s   (ch=0x%X)",
                                  deploy_node_name, d, deploy_push_expect.channel_mask);
            }
        }
    }

    /* REFRESH reconciliation: solicit the MOS's persisted config and resolve as
     * soon as a status echo is available (the seed step below loads it), with
     * the same animated working state and ~1Hz re-solicit until then. */
    if (deploy_refresh_pending) {
        if (have_status) {
            deploy_refresh_pending = false;
            deploy_set_status("\xe2\x9c\x93 Loaded %s config (ch=0x%X).",
                              deploy_node_name, st.cfg.channel_mask);
        } else {
            uint32_t now = lv_tick_get();
            if ((int32_t)(now - deploy_refresh_resend_ms) >= 0) {
                espnow_master_send_parachute_pull(deploy_node);
                deploy_refresh_resend_ms = now + 1000;
            }
            uint32_t waited = now - deploy_refresh_started_ms;
            static const char *rspin[4] = { "", ".", "..", "..." };
            const char *d = rspin[(waited / 350) % 4];
            bool online = espnow_master_node_online(deploy_node);
            deploy_set_status((!online && waited > 3000)
                ? "Connecting to %s%s   (loading config)"
                : "Refreshing from %s%s",
                deploy_node_name, d);
        }
    }

    if (!have_status) {
        if (deploy_live_lbl)
            lv_label_set_text(deploy_live_lbl, "MOS: no status echo yet");
        return;
    }

    /* First echo (or after REFRESH) seeds the editable config from the MOS. */
    if (!deploy_seeded) {
        deploy_cfg   = st.cfg;
        deploy_seeded = true;
        deploy_refresh_widgets();
    }

    const char *act = "?";
    switch (st.act_state) {
        case OPENDASH_PARACHUTE_ACT_UNINIT:   act = "UNINIT";   break;
        case OPENDASH_PARACHUTE_ACT_SAFE:     act = "SAFE";     break;
        case OPENDASH_PARACHUTE_ACT_ARMED:    act = "ARMED";    break;
        case OPENDASH_PARACHUTE_ACT_DEPLOYED: act = "DEPLOYED"; break;
        default: break;
    }
    if (deploy_live_lbl) {
        bool man = false; int total = 0;
        int rolling = espnow_master_rollover_status(&man, &total);
        lv_label_set_text_fmt(deploy_live_lbl,
            "MOS echo: %s  ch=0x%X  actuator=%s  %s%s\n"
            "ROLL VOTES %d/%d%s%s",
            st.cfg.enabled ? "ENABLED" : "disabled",
            st.cfg.channel_mask, act,
            st.armed ? "ARMED" : "DISARMED",
            st.deployed ? "  [DEPLOYED]" : "",
            rolling, total,
            man ? "  MANUAL!" : "",
            (st.cfg.flags & OPENDASH_PARACHUTE_FLAG_AUTO_DETECT)
                ? "  [AUTO ON]" : "  [auto off]");
    }
}

static void deploy_back_cb(lv_event_t *e)
{
    (void)e;
    deploy_push_pending = false;
    deploy_refresh_pending = false;
    deploy_kp_close();
    if (deploy_poll_timer) {
        lv_timer_del(deploy_poll_timer);
        deploy_poll_timer = NULL;
    }
    if (deploy_container) {
        lv_obj_del(deploy_container);
        deploy_container  = NULL;
        deploy_enable_btn = NULL;
        deploy_enable_lbl = NULL;
        deploy_status_lbl = NULL;
        deploy_live_lbl   = NULL;
        deploy_firemode_btn = NULL;
        deploy_firemode_lbl = NULL;
        deploy_auto_btn     = NULL;
        deploy_auto_lbl     = NULL;
        deploy_fire_btn   = NULL;
        deploy_fire_lbl   = NULL;
        for (int i = 0; i < 2; i++) deploy_target_btn[i] = NULL;
        for (int c = 0; c < 4; c++) deploy_chan_btn[c] = NULL;
        for (int f = 0; f < DEPLOY_FIELD_COUNT; f++) deploy_val_lbl[f] = NULL;
    }
}

static void deploy_target_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    opendash_node_t node = (i == 1) ? OPENDASH_NODE_MOS_4CH_B
                                    : OPENDASH_NODE_MOS_4CH_A;
    if (node == deploy_node) return;
    deploy_node      = node;
    deploy_node_name = (i == 1) ? "MOS-B" : "MOS-A";
    deploy_push_pending = false;
    deploy_refresh_pending = false;
    deploy_seeded    = false;                 /* re-seed from this MOS's config */
    opendash_parachute_config_default(&deploy_cfg);
    deploy_refresh_widgets();
    espnow_master_send_parachute_pull(deploy_node);
    deploy_set_status("Target %s — requesting its current config...", deploy_node_name);
}

static void config_deploy_btn_cb(lv_event_t *e)
{
    (void)e;
    /* Pick an initial target: honour a MOS selected on CONFIG if there is one,
     * otherwise default to MOS-A. The user can switch in-panel. */
    deploy_node      = OPENDASH_NODE_MOS_4CH_A;
    deploy_node_name = "MOS-A";
    if (config_selected_node >= 0) {
        opendash_node_t sel = config_nodes[config_selected_node].node;
        if (sel == OPENDASH_NODE_MOS_4CH_A || sel == OPENDASH_NODE_MOS_4CH_B) {
            deploy_node      = sel;
            deploy_node_name = config_nodes[config_selected_node].name;
        }
    }
    deploy_seeded    = false;
    opendash_parachute_config_default(&deploy_cfg);

    deploy_container = lv_obj_create(screen_layout.screen);
    lv_obj_set_size(deploy_container, LCD_H_RES, LCD_V_RES - STATUS_BAR_HEIGHT - 20);
    lv_obj_align(deploy_container, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(deploy_container, lv_color_hex(0x0E0E0E), 0);
    lv_obj_set_style_bg_opa(deploy_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(deploy_container, 0, 0);
    lv_obj_set_style_pad_all(deploy_container, 15, 0);
    lv_obj_clear_flag(deploy_container, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = create_simple_label(deploy_container, "DEPLOYMENT SYSTEM",
                                           OPENDASH_FONT_SIZE_LARGE, 0xFF4444);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 10, 5);

    /* MOS target selector (MOS-A / MOS-B) */
    const char *tgt_lbl[2] = { "MOS-A", "MOS-B" };
    for (int i = 0; i < 2; i++) {
        deploy_target_btn[i] = lv_btn_create(deploy_container);
        lv_obj_set_size(deploy_target_btn[i], 110, 44);
        /* Pushed right so the buttons clear the LARGE "DEPLOYMENT SYSTEM"
         * title (ends ~x=290) and stay left of the top-right BACK button. */
        lv_obj_align(deploy_target_btn[i], LV_ALIGN_TOP_LEFT, 360 + i * 118, 4);
        lv_obj_set_style_radius(deploy_target_btn[i], 8, 0);
        lv_obj_add_event_cb(deploy_target_btn[i], deploy_target_cb,
                            LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = create_simple_label(deploy_target_btn[i], tgt_lbl[i],
                                          OPENDASH_FONT_SIZE_MEDIUM, 0xFFFFFF);
        lv_obj_center(l);
    }

    lv_obj_t *btn_back = lv_btn_create(deploy_container);
    lv_obj_set_size(btn_back, 120, 45);
    lv_obj_align(btn_back, LV_ALIGN_TOP_RIGHT, -10, 0);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(btn_back, 8, 0);
    lv_obj_add_event_cb(btn_back, deploy_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = create_simple_label(btn_back, "\xe2\x86\x90 BACK",
                                              OPENDASH_FONT_SIZE_MEDIUM, 0xFFFFFF);
    lv_obj_center(back_lbl);

    /* ENABLE toggle */
    deploy_enable_btn = lv_btn_create(deploy_container);
    lv_obj_set_size(deploy_enable_btn, 180, 48);
    lv_obj_align(deploy_enable_btn, LV_ALIGN_TOP_LEFT, 10, 58);
    lv_obj_set_style_radius(deploy_enable_btn, 8, 0);
    lv_obj_add_event_cb(deploy_enable_btn, deploy_enable_cb, LV_EVENT_CLICKED, NULL);
    deploy_enable_lbl = create_simple_label(deploy_enable_btn, "ENABLE: OFF",
                                             OPENDASH_FONT_SIZE_MEDIUM, 0xFFFFFF);
    lv_obj_center(deploy_enable_lbl);

    /* Channel selector CH1..CH4 */
    lv_obj_t *chan_hdr = create_simple_label(deploy_container, "DEPLOY CHANNELS",
                                              OPENDASH_FONT_SIZE_SMALL, 0xAAAAAA);
    lv_obj_align(chan_hdr, LV_ALIGN_TOP_LEFT, 210, 58);
    for (int c = 0; c < 4; c++) {
        deploy_chan_btn[c] = lv_btn_create(deploy_container);
        lv_obj_set_size(deploy_chan_btn[c], 68, 48);
        lv_obj_align(deploy_chan_btn[c], LV_ALIGN_TOP_LEFT, 210 + c * 76, 78);
        lv_obj_set_style_radius(deploy_chan_btn[c], 8, 0);
        lv_obj_add_event_cb(deploy_chan_btn[c], deploy_chan_cb,
                            LV_EVENT_CLICKED, (void *)(intptr_t)c);
        char cl[8];
        snprintf(cl, sizeof(cl), "CH%d", c + 1);
        lv_obj_t *l = create_simple_label(deploy_chan_btn[c], cl,
                                          OPENDASH_FONT_SIZE_MEDIUM, 0xFFFFFF);
        lv_obj_center(l);
    }

    /* FIRE MODE toggle — latch (default) vs pulse. Sits right of the channels. */
    deploy_firemode_btn = lv_btn_create(deploy_container);
    lv_obj_set_size(deploy_firemode_btn, 130, 48);
    lv_obj_align(deploy_firemode_btn, LV_ALIGN_TOP_LEFT, 514, 78);
    lv_obj_set_style_radius(deploy_firemode_btn, 8, 0);
    lv_obj_add_event_cb(deploy_firemode_btn, deploy_firemode_cb, LV_EVENT_CLICKED, NULL);
    deploy_firemode_lbl = create_simple_label(deploy_firemode_btn, "FIRE: LATCH",
                                              OPENDASH_FONT_SIZE_MEDIUM, 0xFFFFFF);
    lv_obj_center(deploy_firemode_lbl);

    /* AUTO_DETECT toggle — opt-in for the distributed gyro auto-deploy path.
     * OFF by default; sits right of FIRE MODE, above the manual DEPLOY button. */
    deploy_auto_btn = lv_btn_create(deploy_container);
    lv_obj_set_size(deploy_auto_btn, 130, 48);
    lv_obj_align(deploy_auto_btn, LV_ALIGN_TOP_LEFT, 652, 78);
    lv_obj_set_style_radius(deploy_auto_btn, 8, 0);
    lv_obj_add_event_cb(deploy_auto_btn, deploy_auto_cb, LV_EVENT_CLICKED, NULL);
    deploy_auto_lbl = create_simple_label(deploy_auto_btn, "AUTO: OFF",
                                          OPENDASH_FONT_SIZE_MEDIUM, 0xFFFFFF);
    lv_obj_center(deploy_auto_lbl);

    /* Manual DEPLOY — the always-available top-priority fire button. Big, red,
     * and LONG-PRESS only so it can't be triggered by an accidental tap. Lives
     * in the empty right column under FIRE MODE. */
    deploy_fire_btn = lv_btn_create(deploy_container);
    lv_obj_set_size(deploy_fire_btn, 270, 150);
    lv_obj_align(deploy_fire_btn, LV_ALIGN_TOP_LEFT, 510, 140);
    lv_obj_set_style_bg_color(deploy_fire_btn, lv_color_hex(0xCC0000), 0);
    lv_obj_set_style_radius(deploy_fire_btn, 10, 0);
    lv_obj_set_style_border_color(deploy_fire_btn, lv_color_hex(0xFFAA00), 0);
    lv_obj_set_style_border_width(deploy_fire_btn, 3, 0);
    lv_obj_add_event_cb(deploy_fire_btn, deploy_fire_cb, LV_EVENT_LONG_PRESSED, NULL);
    deploy_fire_lbl = create_simple_label(deploy_fire_btn,
                                          "\xe2\x9a\xa0 HOLD\nTO DEPLOY",
                                          OPENDASH_FONT_SIZE_LARGE, 0xFFFFFF);
    lv_obj_center(deploy_fire_lbl);

    /* Tunable value buttons (open keypad). 3 + 2 grid. */
    const int vbw = 240, vbh = 54, vgap = 8, vy0 = 130;
    for (int f = 0; f < DEPLOY_FIELD_COUNT; f++) {
        int col = f / 3;          /* 0,0,0,1,1 */
        int row = f % 3;
        int x = 10 + col * (vbw + vgap);
        int y = vy0 + row * (vbh + vgap);
        lv_obj_t *b = lv_btn_create(deploy_container);
        lv_obj_set_size(b, vbw, vbh);
        lv_obj_set_pos(b, x, y);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x223344), 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_add_event_cb(b, deploy_val_btn_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)f);
        char buf[40];
        deploy_field_text((deploy_field_t)f, buf, sizeof(buf));
        deploy_val_lbl[f] = create_simple_label(b, buf,
                                                 OPENDASH_FONT_SIZE_MEDIUM, 0xFFFFFF);
        lv_obj_center(deploy_val_lbl[f]);
    }

    /* PUSH + REFRESH buttons (right column lower) */
    lv_obj_t *btn_push = lv_btn_create(deploy_container);
    lv_obj_set_size(btn_push, 240, 56);
    lv_obj_set_pos(btn_push, 10 + (vbw + vgap), vy0 + 2 * (vbh + vgap));
    lv_obj_set_style_bg_color(btn_push, lv_color_hex(0x006622), 0);
    lv_obj_set_style_radius(btn_push, 8, 0);
    lv_obj_add_event_cb(btn_push, deploy_push_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *push_lbl = create_simple_label(btn_push, "PUSH CONFIG",
                                              OPENDASH_FONT_SIZE_MEDIUM, 0xFFFFFF);
    lv_obj_center(push_lbl);

    lv_obj_t *btn_ref = lv_btn_create(deploy_container);
    lv_obj_set_size(btn_ref, 150, 44);
    lv_obj_align(btn_ref, LV_ALIGN_BOTTOM_RIGHT, -10, -56);
    lv_obj_set_style_bg_color(btn_ref, lv_color_hex(0x004488), 0);
    lv_obj_set_style_radius(btn_ref, 8, 0);
    lv_obj_add_event_cb(btn_ref, deploy_refresh_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ref_lbl = create_simple_label(btn_ref, "REFRESH",
                                             OPENDASH_FONT_SIZE_MEDIUM, 0xFFFFFF);
    lv_obj_center(ref_lbl);

    /* ZERO/CAL — stacked directly below REFRESH. Zeroes the gyro detectors'
     * roll to the current resting mount angle (persisted per node). */
    lv_obj_t *btn_cal = lv_btn_create(deploy_container);
    lv_obj_set_size(btn_cal, 150, 40);
    lv_obj_align(btn_cal, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
    lv_obj_set_style_bg_color(btn_cal, lv_color_hex(0x665500), 0);
    lv_obj_set_style_radius(btn_cal, 8, 0);
    lv_obj_add_event_cb(btn_cal, deploy_calibrate_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cal_lbl = create_simple_label(btn_cal, "ZERO/CAL",
                                            OPENDASH_FONT_SIZE_MEDIUM, 0xFFFFFF);
    lv_obj_center(cal_lbl);

    /* Live MOS echo + push feedback — pinned to the very bottom as single-line
     * labels (LONG_DOT) so a long status string can never wrap upward and bleed
     * over the value buttons / PUSH CONFIG above. */
    deploy_live_lbl = create_simple_label(deploy_container, "MOS: no status echo yet",
                                           OPENDASH_FONT_SIZE_SMALL, 0x66CCFF);
    lv_obj_set_width(deploy_live_lbl, LCD_H_RES - 40);
    lv_label_set_long_mode(deploy_live_lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(deploy_live_lbl, LV_ALIGN_BOTTOM_LEFT, 10, -36);

    deploy_status_lbl = create_simple_label(deploy_container,
        "Configure, then PUSH CONFIG.  ARM is on the home screen.",
        OPENDASH_FONT_SIZE_SMALL, 0xCCCCCC);
    lv_obj_set_width(deploy_status_lbl, LCD_H_RES - 40);
    lv_label_set_long_mode(deploy_status_lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(deploy_status_lbl, LV_ALIGN_BOTTOM_LEFT, 10, -14);

    deploy_refresh_widgets();

    /* Pull the MOS's current persisted config and poll the echo cache. */
    espnow_master_send_parachute_pull(deploy_node);
    deploy_poll_timer = lv_timer_create(deploy_poll_timer_cb, 500, NULL);

    ESP_LOGI(TAG, "Deployment screen opened for %s", deploy_node_name);
}

static void config_devmgmt_btn_cb(lv_event_t *e)
{
    (void)e;
    if (config_grid_container) {
        lv_obj_add_flag(config_grid_container, LV_OBJ_FLAG_HIDDEN);
    }

    devmgmt_config_container = lv_obj_create(screen_layout.screen);
    lv_obj_set_size(devmgmt_config_container, LCD_H_RES, LCD_V_RES - STATUS_BAR_HEIGHT - 20);
    lv_obj_align(devmgmt_config_container, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(devmgmt_config_container, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(devmgmt_config_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(devmgmt_config_container, 0, 0);
    lv_obj_set_style_pad_all(devmgmt_config_container, 15, 0);
    lv_obj_clear_flag(devmgmt_config_container, LV_OBJ_FLAG_SCROLLABLE);

#if HAS_BACKGROUND_IMAGE
    lv_obj_t *bg_img = lv_image_create(devmgmt_config_container);
    lv_image_set_src(bg_img, &background_center_dsc);
    lv_obj_set_size(bg_img, LCD_H_RES, LCD_V_RES);
    lv_obj_align(bg_img, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_img_opa(bg_img, 76, 0);
    lv_obj_move_to_index(bg_img, 0);
#endif

    lv_obj_t *title = create_simple_label(devmgmt_config_container, "DEVICE MANAGEMENT",
                                           OPENDASH_FONT_SIZE_LARGE, 0xFF6F00);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 10, 5);

    /* Back button */
    lv_obj_t *btn_back = lv_btn_create(devmgmt_config_container);
    lv_obj_set_size(btn_back, 120, 45);
    lv_obj_align(btn_back, LV_ALIGN_TOP_RIGHT, -10, 0);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(btn_back, 8, 0);
    lv_obj_add_event_cb(btn_back, devmgmt_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = create_simple_label(btn_back, "\xe2\x86\x90 BACK",
                                              OPENDASH_FONT_SIZE_MEDIUM, 0xFFFFFF);
    lv_obj_center(back_lbl);

    /* Node status grid with detailed info */
    espnow_master_node_status_t status;
    espnow_master_get_status(&status);
    const bool online_arr[] = {
        status.left_online, status.right_online, status.gps_online,
        status.bms_online, status.pod1_online, status.pod2_online,
        status.relay_4ch_online, status.relay_8ch_a_online, status.relay_8ch_b_online,
        status.mos_4ch_a_online, status.mos_4ch_b_online,
    };
    const char *node_names[] = {
        "LEFT Gauge", "RIGHT Gauge", "GPS/Telemetry",
        "rAtTrax BMS", "POD 1", "POD 2",
        "Relay 4CH HD", "Relay 8CH-A", "Relay 8CH-B",
        "MOS 4CH-A", "MOS 4CH-B",
    };
    const char *node_hw[] = {
        "ESP32-S3 LCD-2.8C", "ESP32-S3 LCD-2.8C", "ESP32-S3 AMOLED-1.75",
        "ESP32 rAtTrax", "ESP32-S3 AMOLED-1.75", "ESP32-S3 AMOLED-1.75",
        "ESP32 4-CH Relay", "ESP32 8-CH Relay", "ESP32 8-CH Relay",
        "ESP32 4-CH MOSFET", "ESP32 4-CH MOSFET",
    };

    const int dev_box_w = 240;
    const int dev_box_h = 55;
    const int dev_cols = 3;
    const int dev_gap = 8;
    const int dev_start_y = 55;

    for (int i = 0; i < 11; i++) {
        int row = i / dev_cols;
        int col = i % dev_cols;
        int x = 10 + col * (dev_box_w + dev_gap);
        int y = dev_start_y + row * (dev_box_h + dev_gap);

        lv_obj_t *box = lv_obj_create(devmgmt_config_container);
        lv_obj_set_size(box, dev_box_w, dev_box_h);
        lv_obj_set_pos(box, x, y);
        lv_obj_set_style_bg_color(box, lv_color_hex(online_arr[i] ? 0x0A2E0A : 0x1A0A0A), 0);
        lv_obj_set_style_bg_opa(box, 240, 0);
        lv_obj_set_style_border_color(box, lv_color_hex(online_arr[i] ? 0x4ECCA3 : 0xFF4444), 0);
        lv_obj_set_style_border_width(box, 2, 0);
        lv_obj_set_style_radius(box, 8, 0);
        lv_obj_set_style_pad_all(box, 4, 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *name = create_simple_label(box, node_names[i],
                                              OPENDASH_FONT_SIZE_MEDIUM,
                                              online_arr[i] ? 0x4ECCA3 : 0xFF4444);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 4, 2);

        lv_obj_t *hw = create_simple_label(box, node_hw[i],
                                            OPENDASH_FONT_SIZE_SMALL, 0x888888);
        lv_obj_align(hw, LV_ALIGN_BOTTOM_LEFT, 4, -2);

        lv_obj_t *st = create_simple_label(box, online_arr[i] ? "\xe2\x9c\x93" : "\xe2\x9c\x97",
                                            OPENDASH_FONT_SIZE_LARGE,
                                            online_arr[i] ? 0x00FF00 : 0xFF0000);
        lv_obj_align(st, LV_ALIGN_RIGHT_MID, -8, 0);
    }

    lv_obj_t *info = create_simple_label(devmgmt_config_container,
        "Device management: per-node PID assignments, warnings, and audio alerts.",
        OPENDASH_FONT_SIZE_SMALL, 0xAAAAAA);
    lv_obj_set_pos(info, 10, dev_start_y + 4 * (dev_box_h + dev_gap) + 5);
    lv_obj_set_width(info, LCD_H_RES - 40);

    /* ── SELF-TEST button (relocated from CONFIG action row per TODO #11) ── */
    lv_obj_t *btn_st = lv_btn_create(devmgmt_config_container);
    lv_obj_set_size(btn_st, 200, 50);
    lv_obj_align(btn_st, LV_ALIGN_BOTTOM_LEFT, 15, -15);
    lv_obj_set_style_bg_color(btn_st, lv_color_hex(0xF0A500), 0);
    lv_obj_set_style_radius(btn_st, 8, 0);
    lv_obj_add_event_cb(btn_st, devmgmt_test_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *st_lbl = create_simple_label(btn_st, "SELF-TEST",
                                            OPENDASH_FONT_SIZE_MEDIUM, 0xFFFFFF);
    lv_obj_center(st_lbl);

    /* ── DEPLOY SYS button — opens deployment config for the selected MOS ── */
    lv_obj_t *btn_dep = lv_btn_create(devmgmt_config_container);
    lv_obj_set_size(btn_dep, 200, 50);
    lv_obj_align(btn_dep, LV_ALIGN_BOTTOM_LEFT, 225, -15);
    lv_obj_set_style_bg_color(btn_dep, lv_color_hex(0xAA2222), 0);
    lv_obj_set_style_radius(btn_dep, 8, 0);
    lv_obj_add_event_cb(btn_dep, config_deploy_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *dep_lbl = create_simple_label(btn_dep, "DEPLOY SYS",
                                             OPENDASH_FONT_SIZE_MEDIUM, 0xFFFFFF);
    lv_obj_center(dep_lbl);

    /* Action-status line — overwritten on each SELF-TEST / DEPLOY tap. */
    devmgmt_action_status = create_simple_label(devmgmt_config_container,
        "SELF-TEST / DEPLOY SYS target the node selected on CONFIG (DEPLOY needs MOS-A/B).",
        OPENDASH_FONT_SIZE_SMALL, 0xCCCCCC);
    lv_obj_align(devmgmt_action_status, LV_ALIGN_BOTTOM_LEFT, 15, -75);
    lv_obj_set_width(devmgmt_action_status, LCD_H_RES - 300);

    /* ── LAYOUT EDITOR launch button ───────────────────────────── */
    lv_obj_t *btn_le = lv_btn_create(devmgmt_config_container);
    lv_obj_set_size(btn_le, 260, 50);
    lv_obj_align(btn_le, LV_ALIGN_BOTTOM_RIGHT, -15, -15);
    lv_obj_set_style_bg_color(btn_le, lv_color_hex(0x004488), 0);
    lv_obj_set_style_radius(btn_le, 8, 0);
    lv_obj_add_event_cb(btn_le, devmgmt_layout_editor_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *le_lbl = create_simple_label(btn_le, "LAYOUT EDITOR",
                                            OPENDASH_FONT_SIZE_MEDIUM, 0xFFFFFF);
    lv_obj_center(le_lbl);

    ESP_LOGI(TAG, "Device management screen created");
}

/**
 * @brief Destroy the OBD config submenu overlay
 */
static void destroy_obd_config_screen(void)
{
    if (obd_config_container) {
        lv_obj_del(obd_config_container);
        obd_config_container = NULL;
        obd_enable_btn_label = NULL;
        obd_mil_btn_label = NULL;
        obd_dtc_list_label = NULL;
        obd_vin_label = NULL;
        /* Restore config grid visibility */
        if (config_grid_container) {
            lv_obj_clear_flag(config_grid_container, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/**
 * @brief Create the OBD2 Config submenu overlay
 *
 * Layout (800×480 overlay):
 * ┌────────────────────────────────────────────────────────┐
 * │  OBD2 SETUP                                [← BACK]  │
 * ├────────────────────────────────────────────────────────┤
 * │  [OBD PAGE: OFF]   [MIL INDICATOR: ON]                │
 * ├────────────────────────────────────────────────────────┤
 * │  VIN: -not available-                                 │
 * │  DTCs: None detected                                  │
 * │  [CLEAR DTCS]   [REQUEST VIN]                         │
 * └────────────────────────────────────────────────────────┘
 */
static void create_obd_config_screen(lv_obj_t *parent)
{
    const obd_config_t *cfg = obd_config_get();

    /* Hide config grid so it doesn't bleed through */
    if (config_grid_container) {
        lv_obj_add_flag(config_grid_container, LV_OBJ_FLAG_HIDDEN);
    }

    obd_config_container = lv_obj_create(parent);
    lv_obj_set_size(obd_config_container, LCD_H_RES, LCD_V_RES - STATUS_BAR_HEIGHT - 20);
    lv_obj_align(obd_config_container, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(obd_config_container, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(obd_config_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obd_config_container, 0, 0);
    lv_obj_set_style_pad_all(obd_config_container, 15, 0);
    lv_obj_clear_flag(obd_config_container, LV_OBJ_FLAG_SCROLLABLE);

    /* Background logo */
#if HAS_BACKGROUND_IMAGE
    lv_obj_t *bg_img = lv_image_create(obd_config_container);
    lv_image_set_src(bg_img, &background_center_dsc);
    lv_obj_set_size(bg_img, LCD_H_RES, LCD_V_RES);
    lv_obj_align(bg_img, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_img_opa(bg_img, 76, 0);
    lv_obj_move_to_index(bg_img, 0);
#endif

    /* Title */
    lv_obj_t *title = create_simple_label(obd_config_container, "OBD2 SETUP",
                                           OPENDASH_FONT_SIZE_LARGE, 0x00AAFF);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 10, 5);

    /* Back button */
    lv_obj_t *btn_back = lv_btn_create(obd_config_container);
    lv_obj_set_size(btn_back, 120, 45);
    lv_obj_align(btn_back, LV_ALIGN_TOP_RIGHT, -10, 0);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(btn_back, 8, 0);
    lv_obj_add_event_cb(btn_back, obd_config_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = create_simple_label(btn_back, "\xe2\x86\x90 BACK",
                                              OPENDASH_FONT_SIZE_MEDIUM, 0xFFFFFF);
    lv_obj_center(back_lbl);

    /* ── Toggle buttons row ── */
    const int toggle_y = 55;
    const int toggle_w = 260;
    const int toggle_h = 50;

    /* OBD Page Enable/Disable */
    lv_obj_t *btn_enable = lv_btn_create(obd_config_container);
    lv_obj_set_size(btn_enable, toggle_w, toggle_h);
    lv_obj_set_pos(btn_enable, 20, toggle_y);
    lv_obj_set_style_bg_color(btn_enable, lv_color_hex(cfg->obd_enabled ? 0x2E7D32 : 0x555555), 0);
    lv_obj_set_style_radius(btn_enable, 8, 0);
    lv_obj_add_event_cb(btn_enable, obd_enable_toggle_cb, LV_EVENT_CLICKED, NULL);
    obd_enable_btn_label = create_simple_label(btn_enable,
        cfg->obd_enabled ? "OBD PAGE: ON" : "OBD PAGE: OFF",
        OPENDASH_FONT_SIZE_LARGE, 0xFFFFFF);
    lv_obj_center(obd_enable_btn_label);

    /* MIL Indicator Enable/Disable */
    lv_obj_t *btn_mil = lv_btn_create(obd_config_container);
    lv_obj_set_size(btn_mil, toggle_w, toggle_h);
    lv_obj_set_pos(btn_mil, 310, toggle_y);
    lv_obj_set_style_bg_color(btn_mil, lv_color_hex(cfg->mil_indicator_enabled ? 0x2E7D32 : 0x555555), 0);
    lv_obj_set_style_radius(btn_mil, 8, 0);
    lv_obj_add_event_cb(btn_mil, obd_mil_toggle_cb, LV_EVENT_CLICKED, NULL);
    obd_mil_btn_label = create_simple_label(btn_mil,
        cfg->mil_indicator_enabled ? "MIL INDICATOR: ON" : "MIL INDICATOR: OFF",
        OPENDASH_FONT_SIZE_LARGE, 0xFFFFFF);
    lv_obj_center(obd_mil_btn_label);

    /* ── VIN display ── */
    opendash_md_data_t md_snap;
    bool have_data = opendash_uart_get_data(&md_snap);

    char vin_buf[48];
    if (have_data && md_snap.vin_valid) {
        snprintf(vin_buf, sizeof(vin_buf), "VIN: %.17s", md_snap.vin);
    } else {
        snprintf(vin_buf, sizeof(vin_buf), "VIN: — not available —");
    }
    obd_vin_label = create_simple_label(obd_config_container, vin_buf,
                                         OPENDASH_FONT_SIZE_MEDIUM, 0xFFFFFF);
    lv_obj_set_pos(obd_vin_label, 20, toggle_y + toggle_h + 20);

    /* ── DTC list — prefer ESP-NOW data from left pod, fallback to local UART ── */
    char dtc_buf[256];
    char espnow_codes[16][6];
    uint8_t espnow_count = 0;
    bool espnow_valid = false;
    espnow_master_get_dtc_data(espnow_codes, &espnow_count, &espnow_valid);

    if (espnow_valid && espnow_count > 0) {
        int off = snprintf(dtc_buf, sizeof(dtc_buf), "DTCs (%d): ", espnow_count);
        for (int i = 0; i < espnow_count && i < 16; i++) {
            off += snprintf(dtc_buf + off, sizeof(dtc_buf) - off,
                           "%s%s", espnow_codes[i],
                           (i < espnow_count - 1) ? ", " : "");
            if (off >= (int)sizeof(dtc_buf) - 10) break;
        }
    } else if (have_data && md_snap.dtc_data_valid && md_snap.dtc_count > 0) {
        int off = snprintf(dtc_buf, sizeof(dtc_buf), "DTCs (%d): ", md_snap.dtc_count);
        for (int i = 0; i < md_snap.dtc_count && i < OPENDASH_MAX_DTCS; i++) {
            off += snprintf(dtc_buf + off, sizeof(dtc_buf) - off,
                           "%s%s", md_snap.dtc_codes[i],
                           (i < md_snap.dtc_count - 1) ? ", " : "");
            if (off >= (int)sizeof(dtc_buf) - 10) break;
        }
    } else if (have_data && md_snap.mil_on) {
        snprintf(dtc_buf, sizeof(dtc_buf), "DTCs: MIL on — press READ DTCS to fetch codes");
    } else {
        snprintf(dtc_buf, sizeof(dtc_buf), "DTCs: None detected");
    }
    obd_dtc_list_label = create_simple_label(obd_config_container, dtc_buf,
                                              OPENDASH_FONT_SIZE_MEDIUM, 0xFFCC00);
    lv_obj_set_pos(obd_dtc_list_label, 20, toggle_y + toggle_h + 50);
    lv_obj_set_width(obd_dtc_list_label, LCD_H_RES - 60);

    /* ── Action buttons ── */
    const int act_y = toggle_y + toggle_h + 95;
    const int act_w = 185;
    const int act_h = 50;

    /* Read DTCs button */
    lv_obj_t *btn_read = lv_btn_create(obd_config_container);
    lv_obj_set_size(btn_read, act_w, act_h);
    lv_obj_set_pos(btn_read, 20, act_y);
    lv_obj_set_style_bg_color(btn_read, lv_color_hex(0xFF8C00), 0);
    lv_obj_set_style_radius(btn_read, 8, 0);
    lv_obj_add_event_cb(btn_read, obd_read_dtc_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *read_lbl = create_simple_label(btn_read, "READ DTCS",
                                              OPENDASH_FONT_SIZE_LARGE, 0xFFFFFF);
    lv_obj_center(read_lbl);

    /* Clear DTCs button */
    lv_obj_t *btn_clear = lv_btn_create(obd_config_container);
    lv_obj_set_size(btn_clear, act_w, act_h);
    lv_obj_set_pos(btn_clear, 220, act_y);
    lv_obj_set_style_bg_color(btn_clear, lv_color_hex(0xE94560), 0);
    lv_obj_set_style_radius(btn_clear, 8, 0);
    lv_obj_add_event_cb(btn_clear, obd_clear_dtc_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *clear_lbl = create_simple_label(btn_clear, "CLEAR DTCS",
                                               OPENDASH_FONT_SIZE_LARGE, 0xFFFFFF);
    lv_obj_center(clear_lbl);

    /* Request VIN button */
    lv_obj_t *btn_vin = lv_btn_create(obd_config_container);
    lv_obj_set_size(btn_vin, act_w, act_h);
    lv_obj_set_pos(btn_vin, 420, act_y);
    lv_obj_set_style_bg_color(btn_vin, lv_color_hex(0x0077B6), 0);
    lv_obj_set_style_radius(btn_vin, 8, 0);
    lv_obj_add_event_cb(btn_vin, obd_request_vin_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *vin_lbl = create_simple_label(btn_vin, "REQUEST VIN",
                                             OPENDASH_FONT_SIZE_LARGE, 0xFFFFFF);
    lv_obj_center(vin_lbl);

    /* ── Warning thresholds info ── */
    const char *warn_names[] = { "COOLANT", "OIL TEMP", "OIL PSI", "BATT V", "BOOST", "AFR" };
    char thresh_buf[512];
    int toff = snprintf(thresh_buf, sizeof(thresh_buf), "Warning Thresholds:\n");
    for (int i = 0; i < OBD_WARN_THRESHOLD_COUNT; i++) {
        const obd_warning_threshold_t *w = &cfg->warnings[i];
        if (w->caution == 0.0f && w->critical == 0.0f) {
            toff += snprintf(thresh_buf + toff, sizeof(thresh_buf) - toff,
                            "  %s: disabled\n", warn_names[i]);
        } else {
            toff += snprintf(thresh_buf + toff, sizeof(thresh_buf) - toff,
                            "  %s: caution=%.0f critical=%.0f (%s)\n",
                            warn_names[i], w->caution, w->critical,
                            w->above ? "above" : "below");
        }
        if (toff >= (int)sizeof(thresh_buf) - 50) break;
    }
    lv_obj_t *thresh_lbl = create_simple_label(obd_config_container, thresh_buf,
                                                OPENDASH_FONT_SIZE_SMALL, 0xDDDDDD);
    lv_obj_set_pos(thresh_lbl, 20, act_y + act_h + 20);
    lv_obj_set_width(thresh_lbl, LCD_H_RES - 60);

    ESP_LOGI(TAG, "OBD config screen created");
}

/* ── OBD Performance Dashboard ───────────────────────────────────────────── */

/**
 * @brief Create the OBD Performance Dashboard
 *
 * Automotive-styled layout with RPM arc gauge, speed readout,
 * critical gauges, and data cards. Populates obd_boxes[] for
 * compatibility with the existing data update path.
 *
 * Layout (800×430):
 * ┌─────────────────────────────────────────────────────────────┐
 * │ ┌── SPEED ──┐  ┌────── RPM ARC ──────┐  ┌── CRITICAL ──┐  │
 * │ │   65      │  │    ╱‾‾‾‾‾‾‾‾‾╲      │  │   COOLANT    │  │
 * │ │   MPH     │  │   │   4500    │      │  │    195°F     │  │
 * │ │           │  │   │   RPM     │      │  ├──────────────┤  │
 * │ │  LOAD 45% │  │    ╲_________╱       │  │  OIL TEMP    │  │
 * │ │  ████░░░  │  │                      │  │    210°F     │  │
 * │ └───────────┘  └──────────────────────┘  ├──────────────┤  │
 * │                                          │   BATT V     │  │
 * │                                          │    14.2      │  │
 * │                                          └──────────────┘  │
 * │ ┌─────┐┌─────┐┌─────┐┌─────┐┌─────┐┌─────┐┌─────┐       │
 * │ │ TPS ││ IAT ││BOOST││ TMG ││ AFR ││FUEL%││ EGT │       │
 * │ │ 32% ││105°F││14psi││ 14° ││14.7 ││ 75% ││1200 │       │
 * │ └─────┘└─────┘└─────┘└─────┘└─────┘└─────┘└─────┘       │
 * └─────────────────────────────────────────────────────────────┘
 *
 * @param parent  LVGL parent (screen)
 * @return Container object (starts visible)
 */
static lv_obj_t* create_obd_dashboard(lv_obj_t *parent)
{
    /* ── Container ── */
    lv_obj_t *container = lv_obj_create(parent);
    const int cont_h = LCD_V_RES - STATUS_BAR_HEIGHT - 20;
    lv_obj_set_size(container, LCD_H_RES, cont_h);
    lv_obj_align(container, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(container, 0, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Layout constants ── */
    const int top_h     = 290;   /* Top row height */
    const int bot_h     = 100;   /* Bottom card row height */
    const int gap       = 6;     /* Spacing */
    const int left_w    = 200;   /* Speed + Load panel */
    const int right_w   = 180;   /* Critical gauges column */
    const int center_w  = LCD_H_RES - left_w - right_w - gap * 4;
    const int card_count = 7;    /* Bottom row cards */
    const int card_w    = (LCD_H_RES - (card_count + 1) * gap) / card_count;
    const int card_y    = top_h + gap;

    /* ─────────────────────────────────────────────────────────────────
     * LEFT PANEL — Speed + Engine Load bar
     * ───────────────────────────────────────────────────────────────── */
    lv_obj_t *left_panel = lv_obj_create(container);
    lv_obj_set_size(left_panel, left_w, top_h);
    lv_obj_set_pos(left_panel, gap, gap);
    lv_obj_set_style_bg_color(left_panel, lv_color_hex(0x0A0E17), 0);
    lv_obj_set_style_bg_opa(left_panel, 240, 0);
    lv_obj_set_style_border_color(left_panel, lv_color_hex(0x1A2332), 0);
    lv_obj_set_style_border_width(left_panel, 2, 0);
    lv_obj_set_style_radius(left_panel, 12, 0);
    lv_obj_set_style_pad_all(left_panel, 8, 0);
    lv_obj_clear_flag(left_panel, LV_OBJ_FLAG_SCROLLABLE);

    /* Speed label */
    lv_obj_t *spd_label = create_simple_label(left_panel, "SPEED",
                                               OPENDASH_FONT_SIZE_SMALL, 0x00BFFF);
    lv_obj_align(spd_label, LV_ALIGN_TOP_MID, 0, 0);

    /* Speed value — large */
    obd_speed_value = create_simple_label(left_panel, "---",
                                           OPENDASH_FONT_SIZE_XXLARGE, 0xFFFFFF);
    lv_obj_align(obd_speed_value, LV_ALIGN_TOP_MID, 0, 18);

    /* Speed unit */
    lv_obj_t *spd_unit = create_simple_label(left_panel, "MPH",
                                              OPENDASH_FONT_SIZE_MEDIUM, 0x7B8DA0);
    lv_obj_align(spd_unit, LV_ALIGN_TOP_MID, 0, 115);

    /* Engine Load section */
    lv_obj_t *load_label = create_simple_label(left_panel, "ENGINE LOAD",
                                                OPENDASH_FONT_SIZE_SMALL, 0x00BFFF);
    lv_obj_set_pos(load_label, 4, 150);

    /* Load value */
    lv_obj_t *load_val = create_simple_label(left_panel, "---",
                                              OPENDASH_FONT_SIZE_LARGE, 0xFFFFFF);
    lv_obj_set_pos(load_val, 4, 168);

    lv_obj_t *load_unit = create_simple_label(left_panel, "%",
                                               OPENDASH_FONT_SIZE_MEDIUM, 0x7B8DA0);
    lv_obj_align(load_unit, LV_ALIGN_TOP_RIGHT, -8, 175);

    /* Load bar */
    lv_obj_t *load_bar = lv_bar_create(left_panel);
    lv_obj_set_size(load_bar, left_w - 24, 14);
    lv_obj_set_pos(load_bar, 4, 210);
    lv_bar_set_range(load_bar, 0, 100);
    lv_bar_set_value(load_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(load_bar, lv_color_hex(0x1A2332), LV_PART_MAIN);
    lv_obj_set_style_bg_color(load_bar, lv_color_hex(0x00BFFF), LV_PART_INDICATOR);
    lv_obj_set_style_radius(load_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(load_bar, 4, LV_PART_INDICATOR);

    /* Boost section */
    lv_obj_t *boost_label = create_simple_label(left_panel, "BOOST",
                                                 OPENDASH_FONT_SIZE_SMALL, 0x00BFFF);
    lv_obj_set_pos(boost_label, 4, 235);

    lv_obj_t *boost_val = create_simple_label(left_panel, "---",
                                               OPENDASH_FONT_SIZE_LARGE, 0xFFFFFF);
    lv_obj_set_pos(boost_val, 4, 252);

    lv_obj_t *boost_unit_lbl = create_simple_label(left_panel, "PSI",
                                                    OPENDASH_FONT_SIZE_MEDIUM, 0x7B8DA0);
    lv_obj_align(boost_unit_lbl, LV_ALIGN_BOTTOM_RIGHT, -8, -4);

    /* ─────────────────────────────────────────────────────────────────
     * CENTER — RPM Arc Gauge
     * ───────────────────────────────────────────────────────────────── */
    const int arc_size = top_h - 30;
    const int arc_x = left_w + gap * 2 + center_w / 2;
    const int arc_y = top_h / 2 + gap;

    obd_rpm_arc = lv_arc_create(container);
    lv_obj_set_size(obd_rpm_arc, arc_size, arc_size);
    lv_obj_set_pos(obd_rpm_arc, arc_x - arc_size / 2, arc_y - arc_size / 2);
    lv_arc_set_rotation(obd_rpm_arc, 135);
    lv_arc_set_bg_angles(obd_rpm_arc, 0, 270);
    lv_arc_set_value(obd_rpm_arc, 0);
    lv_arc_set_range(obd_rpm_arc, 0, 8000);
    lv_arc_set_mode(obd_rpm_arc, LV_ARC_MODE_NORMAL);
    lv_obj_remove_style(obd_rpm_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(obd_rpm_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(obd_rpm_arc, 28, LV_PART_MAIN);
    lv_obj_set_style_arc_width(obd_rpm_arc, 28, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(obd_rpm_arc, lv_color_hex(0x1A0000), LV_PART_MAIN);
    lv_obj_set_style_arc_color(obd_rpm_arc, lv_color_hex(0x00BFFF), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(obd_rpm_arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(obd_rpm_arc, true, LV_PART_INDICATOR);

    /* RPM value (center of arc) */
    lv_obj_t *rpm_val = create_simple_label(container, "0",
                                             OPENDASH_FONT_SIZE_XLARGE, 0xFFFFFF);
    lv_obj_set_pos(rpm_val, arc_x - 50, arc_y - 35);
    lv_obj_set_width(rpm_val, 100);
    lv_obj_set_style_text_align(rpm_val, LV_TEXT_ALIGN_CENTER, 0);

    /* RPM unit label */
    lv_obj_t *rpm_unit = create_simple_label(container, "RPM",
                                              OPENDASH_FONT_SIZE_MEDIUM, 0x7B8DA0);
    lv_obj_set_pos(rpm_unit, arc_x - 20, arc_y + 30);

    /* ─────────────────────────────────────────────────────────────────
     * RIGHT PANEL — Critical gauges (Coolant, Oil Temp, Battery V)
     * ───────────────────────────────────────────────────────────────── */
    const int right_x = LCD_H_RES - right_w - gap;
    const int crit_box_h = (top_h - gap * 4) / 3;

    /* Accent colors for critical gauges */
    const uint32_t crit_accents[3] = { 0x00BFFF, 0xFF6B35, 0x4ECCA3 };
    const char *crit_labels[3] = { "COOLANT", "OIL TEMP", "BATT V" };
    const char *crit_units[3] = { "\xc2\xb0""F", "\xc2\xb0""F", "V" };
    /* Map to obd_grid_map indices: COOLANT=2, OIL_TEMP=7, BATT_V=13 */
    const int crit_box_idx[3] = { 2, 7, 13 };

    for (int i = 0; i < 3; i++) {
        lv_obj_t *crit_box = lv_obj_create(container);
        lv_obj_set_size(crit_box, right_w, crit_box_h);
        lv_obj_set_pos(crit_box, right_x, gap + i * (crit_box_h + gap));
        lv_obj_set_style_bg_color(crit_box, lv_color_hex(0x0A0E17), 0);
        lv_obj_set_style_bg_opa(crit_box, 240, 0);
        lv_obj_set_style_border_color(crit_box, lv_color_hex(0x1A2332), 0);
        lv_obj_set_style_border_width(crit_box, 2, 0);
        lv_obj_set_style_radius(crit_box, 10, 0);
        lv_obj_set_style_pad_all(crit_box, 4, 0);
        lv_obj_clear_flag(crit_box, LV_OBJ_FLAG_SCROLLABLE);

        /* Accent bar on left edge */
        lv_obj_t *accent = lv_obj_create(crit_box);
        lv_obj_set_size(accent, 4, crit_box_h - 16);
        lv_obj_set_pos(accent, 0, 4);
        lv_obj_set_style_bg_color(accent, lv_color_hex(crit_accents[i]), 0);
        lv_obj_set_style_bg_opa(accent, 255, 0);
        lv_obj_set_style_radius(accent, 2, 0);
        lv_obj_set_style_border_width(accent, 0, 0);

        /* Label */
        lv_obj_t *lbl = create_simple_label(crit_box, crit_labels[i],
                                             OPENDASH_FONT_SIZE_SMALL, crit_accents[i]);
        lv_obj_set_pos(lbl, 12, 2);

        /* Value */
        lv_obj_t *val = create_simple_label(crit_box, "---",
                                             OPENDASH_FONT_SIZE_LARGE, 0xFFFFFF);
        lv_obj_align(val, LV_ALIGN_CENTER, 8, 6);

        /* Unit */
        lv_obj_t *u = create_simple_label(crit_box, crit_units[i],
                                           OPENDASH_FONT_SIZE_SMALL, 0x7B8DA0);
        lv_obj_align(u, LV_ALIGN_BOTTOM_RIGHT, -4, -2);

        /* Store in obd_boxes at the correct index for data updates */
        int idx = crit_box_idx[i];
        obd_boxes[idx].box = crit_box;
        obd_boxes[idx].label = lbl;
        obd_boxes[idx].value = val;
        obd_boxes[idx].unit_label = u;
    }

    /* ─────────────────────────────────────────────────────────────────
     * BOTTOM ROW — 7 compact data cards
     * Map: TPS=5, IAT=3, BOOST=6, TIMING=14, AFR=10, FUEL_LEVEL=idx, EGT=12
     * ───────────────────────────────────────────────────────────────── */
    const char *card_labels[7]  = { "TPS",  "IAT",   "BOOST", "TIMING", "AFR",  "FUEL%", "EGT" };
    const char *card_units_str[7] = { "%",    "\xc2\xb0""F", "PSI",   "\xc2\xb0",     "",     "%",     "\xc2\xb0""F" };
    const int card_idx[7]       = { 5,      3,       6,       14,       10,     -1,      12 };
    /* Note: FUEL% is not in the 15-slot grid — we use a separate label below */

    lv_obj_t *fuel_val_label __attribute__((unused)) = NULL;

    for (int i = 0; i < card_count; i++) {
        int cx = gap + i * (card_w + gap);
        lv_obj_t *card = lv_obj_create(container);
        lv_obj_set_size(card, card_w, bot_h);
        lv_obj_set_pos(card, cx, card_y);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x0A0E17), 0);
        lv_obj_set_style_bg_opa(card, 240, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0x1A2332), 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_radius(card, 8, 0);
        lv_obj_set_style_pad_all(card, 2, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        /* Card label */
        lv_obj_t *clbl = create_simple_label(card, card_labels[i],
                                              OPENDASH_FONT_SIZE_SMALL, 0x00BFFF);
        lv_obj_align(clbl, LV_ALIGN_TOP_MID, 0, 2);

        /* Card value */
        lv_obj_t *cval = create_simple_label(card, "---",
                                              OPENDASH_FONT_SIZE_LARGE, 0xFFFFFF);
        lv_obj_align(cval, LV_ALIGN_CENTER, 0, 4);

        /* Card unit */
        lv_obj_t *cu = create_simple_label(card, card_units_str[i],
                                            OPENDASH_FONT_SIZE_SMALL, 0x7B8DA0);
        lv_obj_align(cu, LV_ALIGN_BOTTOM_MID, 0, -2);

        /* Wire into obd_boxes for data updates */
        int idx = card_idx[i];
        if (idx >= 0 && idx < DATA_GRID_TOTAL) {
            obd_boxes[idx].box = card;
            obd_boxes[idx].label = clbl;
            obd_boxes[idx].value = cval;
            obd_boxes[idx].unit_label = cu;
        }
        if (idx == -1) {
            /* Fuel level — special handling */
            fuel_val_label = cval;
        }
    }

    /* ── Map RPM (idx=0) and SPEED (idx=1) into obd_boxes for data update ── */
    obd_boxes[0].value = rpm_val;       /* RPM value label */
    obd_boxes[0].box   = NULL;          /* No box — arc container */
    obd_boxes[1].value = obd_speed_value; /* Speed value label */
    obd_boxes[1].box   = left_panel;

    /* Load (idx=4) → already in left panel */
    obd_boxes[4].value = load_val;
    obd_boxes[4].box   = left_panel;

    /* Boost (idx=6) → assigned above in card loop already */

    /* Remaining unmapped slots: OIL_PSI(8), FUEL_PSI(9), LAMBDA(11)
     * These don't have dedicated widgets in this layout — leave NULL */

    ESP_LOGI(TAG, "OBD Performance Dashboard created");
    return container;
}

/**
 * @brief Create the config/OTA grid screen
 *
 * Layout (800×480):
 * ┌────────────────────────────────────────────────────────┐
 * │  [LEFT]  [RIGHT]  [GPS]  [BMS]  [R4CH]  [R8A]  [R8B] │  <- node row
 * ├────────────────────────────────────────────────────────┤
 * │                                                        │
 * │     [ OTA FLASH ]   [ SELF-TEST ]   [ REBOOT ]        │  <- action row
 * │                                                        │
 * │         Select a node, then tap an action              │
 * └────────────────────────────────────────────────────────┘
 */
static void create_config_grid(lv_obj_t *parent)
{
    config_grid_container = lv_obj_create(parent);
    lv_obj_set_size(config_grid_container, LCD_H_RES, LCD_V_RES - STATUS_BAR_HEIGHT - 20);
    lv_obj_align(config_grid_container, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(config_grid_container, 0, 0);
    lv_obj_set_style_border_width(config_grid_container, 0, 0);
    lv_obj_set_style_pad_all(config_grid_container, 10, 0);
    lv_obj_clear_flag(config_grid_container, LV_OBJ_FLAG_SCROLLABLE);

    /* Title */
    lv_obj_t *title = create_simple_label(config_grid_container, "SYSTEM CONFIG",
                                           OPENDASH_FONT_SIZE_LARGE, 0xFF4444);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    /* ── Node selection rows (2 rows: 6 top, 5 bottom) ── */
    const int node_box_w = 85;
    const int node_box_h = 65;
    const int node_spacing = 6;

    for (int i = 0; i < CONFIG_NODE_COUNT; i++) {
        int row      = (i < CONFIG_NODES_ROW1) ? 0 : 1;
        int col      = (i < CONFIG_NODES_ROW1) ? i : (i - CONFIG_NODES_ROW1);
        int row_cnt  = (row == 0) ? CONFIG_NODES_ROW1 : CONFIG_NODES_ROW2;
        int row_w    = row_cnt * node_box_w + (row_cnt - 1) * node_spacing;
        int row_sx   = (LCD_H_RES - 20 - row_w) / 2;
        int box_x    = row_sx + col * (node_box_w + node_spacing);
        int box_y    = 35 + row * (node_box_h + node_spacing);

        lv_obj_t *box = lv_obj_create(config_grid_container);
        lv_obj_set_size(box, node_box_w, node_box_h);
        lv_obj_set_pos(box, box_x, box_y);
        lv_obj_set_style_bg_color(box, lv_color_hex(OPENDASH_COLOR_BG_SECTION), 0);
        lv_obj_set_style_bg_opa(box, 242, 0);
        lv_obj_set_style_border_color(box, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_width(box, 1, 0);
        lv_obj_set_style_radius(box, 8, 0);
        lv_obj_set_style_pad_all(box, 0, 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);

        /* Node name */
        lv_obj_t *name_lbl = create_simple_label(box, config_nodes[i].name,
                                                   OPENDASH_FONT_SIZE_MEDIUM, 0xFFFFFF);
        lv_obj_align(name_lbl, LV_ALIGN_TOP_MID, 0, 6);

        /* Status (updated by timer) */
        lv_obj_t *stat_lbl = create_simple_label(box, "OFFLINE",
                                                   OPENDASH_FONT_SIZE_SMALL, 0xFF4444);
        lv_obj_align(stat_lbl, LV_ALIGN_BOTTOM_MID, 0, -6);
        config_nodes[i].status_label = stat_lbl;
        config_nodes[i].box          = box;

        /* Touch handler */
        lv_obj_add_event_cb(box, config_node_touch_cb, LV_EVENT_CLICKED,
                             (void *)(intptr_t)i);
    }

    /* ── Action buttons row (below both node rows) ── */
    const int btn_w = 160;
    const int btn_h = 50;
    const int btn_y = 35 + 2 * (node_box_h + node_spacing) + 4;  /* below row 2 */
    const int btn_spacing = 10;

    /* Row 1: OTA Flash, Reboot, Debug Mode
     * (SELF-TEST relocated into the Device Management submenu — TODO #11) */
    const int row1_total = 3;
    const int row1_w = row1_total * btn_w + (row1_total - 1) * btn_spacing;
    const int row1_sx = (LCD_H_RES - 20 - row1_w) / 2;

    /* OTA Flash */
    lv_obj_t *btn_ota = lv_btn_create(config_grid_container);
    lv_obj_set_size(btn_ota, btn_w, btn_h);
    lv_obj_set_pos(btn_ota, row1_sx, btn_y);
    lv_obj_set_style_bg_color(btn_ota, lv_color_hex(0xE94560), 0);
    lv_obj_set_style_radius(btn_ota, 10, 0);
    lv_obj_add_event_cb(btn_ota, config_ota_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ota_lbl = create_simple_label(btn_ota, "OTA FLASH",
                                             OPENDASH_FONT_SIZE_MEDIUM, 0xFFFFFF);
    lv_obj_center(ota_lbl);

    /* Reboot */
    lv_obj_t *btn_reboot = lv_btn_create(config_grid_container);
    lv_obj_set_size(btn_reboot, btn_w, btn_h);
    lv_obj_set_pos(btn_reboot, row1_sx + (btn_w + btn_spacing), btn_y);
    lv_obj_set_style_bg_color(btn_reboot, lv_color_hex(0x4ECCA3), 0);
    lv_obj_set_style_radius(btn_reboot, 10, 0);
    lv_obj_add_event_cb(btn_reboot, config_reboot_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *reboot_lbl = create_simple_label(btn_reboot, "REBOOT",
                                                OPENDASH_FONT_SIZE_MEDIUM, 0xFFFFFF);
    lv_obj_center(reboot_lbl);

    /* DEBUG MODE */
    lv_obj_t *btn_debug = lv_btn_create(config_grid_container);
    lv_obj_set_size(btn_debug, btn_w, btn_h);
    lv_obj_set_pos(btn_debug, row1_sx + 2 * (btn_w + btn_spacing), btn_y);
    lv_obj_set_style_bg_color(btn_debug, lv_color_hex(0x7B2FBE), 0);
    lv_obj_set_style_radius(btn_debug, 10, 0);
    lv_obj_add_event_cb(btn_debug, config_debug_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *debug_lbl = create_simple_label(btn_debug, "DEBUG MODE",
                                               OPENDASH_FONT_SIZE_MEDIUM, 0xFFFFFF);
    lv_obj_center(debug_lbl);

    /* Row 2: OBD2 Setup, Data Source, Device Mgmt, Boost */
    const int row2_y = btn_y + btn_h + 8;
    const int row2_total = 4;
    const int row2_w = row2_total * btn_w + (row2_total - 1) * btn_spacing;
    const int row2_sx = (LCD_H_RES - 20 - row2_w) / 2;

    /* OBD2 SETUP */
    lv_obj_t *btn_obd = lv_btn_create(config_grid_container);
    lv_obj_set_size(btn_obd, btn_w, btn_h);
    lv_obj_set_pos(btn_obd, row2_sx, row2_y);
    lv_obj_set_style_bg_color(btn_obd, lv_color_hex(0x0077B6), 0);
    lv_obj_set_style_radius(btn_obd, 10, 0);
    lv_obj_add_event_cb(btn_obd, config_obd_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *obd_lbl = create_simple_label(btn_obd, "OBD2 SETUP",
                                             OPENDASH_FONT_SIZE_MEDIUM, 0xFFFFFF);
    lv_obj_center(obd_lbl);

    /* DATA SOURCE */
    lv_obj_t *btn_datasrc = lv_btn_create(config_grid_container);
    lv_obj_set_size(btn_datasrc, btn_w, btn_h);
    lv_obj_set_pos(btn_datasrc, row2_sx + (btn_w + btn_spacing), row2_y);
    lv_obj_set_style_bg_color(btn_datasrc, lv_color_hex(0x2E7D32), 0);
    lv_obj_set_style_radius(btn_datasrc, 10, 0);
    lv_obj_add_event_cb(btn_datasrc, config_datasrc_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *datasrc_lbl = create_simple_label(btn_datasrc, "DATA SOURCE",
                                                  OPENDASH_FONT_SIZE_MEDIUM, 0xFFFFFF);
    lv_obj_center(datasrc_lbl);

    /* DEVICE MGMT */
    lv_obj_t *btn_devmgmt = lv_btn_create(config_grid_container);
    lv_obj_set_size(btn_devmgmt, btn_w, btn_h);
    lv_obj_set_pos(btn_devmgmt, row2_sx + 2 * (btn_w + btn_spacing), row2_y);
    lv_obj_set_style_bg_color(btn_devmgmt, lv_color_hex(0xFF6F00), 0);
    lv_obj_set_style_radius(btn_devmgmt, 10, 0);
    lv_obj_add_event_cb(btn_devmgmt, config_devmgmt_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *devmgmt_lbl = create_simple_label(btn_devmgmt, "DEVICE MGMT",
                                                  OPENDASH_FONT_SIZE_MEDIUM, 0xFFFFFF);
    lv_obj_center(devmgmt_lbl);

    /* BOOST CONTROLLER — opens the boost map/PID editor (RACE/NORMAL maps,
     * setpoint + duty per gear, live telemetry from MOS slave). */
    lv_obj_t *btn_boost = lv_btn_create(config_grid_container);
    lv_obj_set_size(btn_boost, btn_w, btn_h);
    lv_obj_set_pos(btn_boost, row2_sx + 3 * (btn_w + btn_spacing), row2_y);
    lv_obj_set_style_bg_color(btn_boost, lv_color_hex(0xD32F2F), 0);
    lv_obj_set_style_radius(btn_boost, 10, 0);
    lv_obj_add_event_cb(btn_boost, config_boost_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *boost_lbl = create_simple_label(btn_boost, "BOOST",
                                                OPENDASH_FONT_SIZE_MEDIUM, 0xFFFFFF);
    lv_obj_center(boost_lbl);

    /* Help / action-status line — starts as a hint, gets overwritten on each
     * OTA / SELF-TEST / REBOOT click so the user sees that the tap registered
     * (and what the slave is expected to do). */
    config_action_status = create_simple_label(config_grid_container,
                                                "Select node \xe2\x86\x92 tap action.  OTA supported on all nodes except BMS.",
                                                OPENDASH_FONT_SIZE_SMALL, 0xFFFFFF);
    lv_obj_set_pos(config_action_status, 10, row2_y + btn_h + 6);
    lv_obj_set_width(config_action_status, LCD_H_RES - 60);
    lv_obj_set_style_text_align(config_action_status, LV_TEXT_ALIGN_CENTER, 0);

    /* Version info — bottom right corner */
    lv_obj_t *ver = create_simple_label(config_grid_container,
                                         "OpenDash " OPENDASH_VERSION_STR,
                                         OPENDASH_FONT_SIZE_SMALL, 0x888888);
    lv_obj_align(ver, LV_ALIGN_BOTTOM_RIGHT, -15, -5);

    config_selected_node = -1;
    lv_obj_add_flag(config_grid_container, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief Show/hide grid containers and arc widgets based on active mode
 *
 * Grid-based modes (RELAY, BMS, OBD, CONFIG) hide the arc + 6 data sections.
 * Arc-based modes (ENGINE, GPS, MD) hide all grids.
 */
static void toggle_grid_visibility(display_mode_t mode)
{
    bool is_grid_mode = (mode == DISPLAY_MODE_RELAY ||
                         mode == DISPLAY_MODE_BMS ||
                         mode == DISPLAY_MODE_OBD ||
                         mode == DISPLAY_MODE_CONFIG);

    /*
     * Destroy-on-leave / Create-on-enter:
     * Only ONE grid exists in memory at a time. This keeps LVGL object
     * count low (~61 per grid) and avoids OOM when the 3rd grid would
     * have been created.  Min/max values live in static C arrays and
     * are NOT affected.  Relay is_on state is in relay_box_map[] statics.
     */

    /* ── Tear down any grid that is NOT the one we're switching to ── */
    if (mode != DISPLAY_MODE_RELAY && relay_grid_container) {
        ESP_LOGI(TAG, "Destroying relay grid (leaving mode)");
        lv_obj_del(relay_grid_container);
        relay_grid_container = NULL;
        memset(relay_boxes, 0, sizeof(relay_boxes));
    }
    if (mode != DISPLAY_MODE_BMS && bms_grid_container) {
        ESP_LOGI(TAG, "Destroying BMS grid (leaving mode)");
        lv_obj_del(bms_grid_container);
        bms_grid_container = NULL;
        memset(bms_boxes, 0, sizeof(bms_boxes));
    }
    if (mode != DISPLAY_MODE_OBD && obd_grid_container) {
        ESP_LOGI(TAG, "Destroying OBD grid (leaving mode)");
        lv_obj_del(obd_grid_container);
        obd_grid_container = NULL;
        obd_rpm_arc = NULL;
        obd_speed_value = NULL;
        memset(obd_boxes, 0, sizeof(obd_boxes));
    }
    if (mode != DISPLAY_MODE_CONFIG && config_grid_container) {
        ESP_LOGI(TAG, "Destroying config grid (leaving mode)");
        destroy_obd_config_screen();  /* Also kill OBD submenu overlay */
        lv_obj_del(config_grid_container);
        config_grid_container = NULL;
        for (int i = 0; i < CONFIG_NODE_COUNT; i++) {
            config_nodes[i].status_label = NULL;
            config_nodes[i].box          = NULL;
            config_chip_last_state[i]    = 0xFF;
            config_chip_last_flags[i]    = 0xFF;
        }
        if (config_ota_blink_timer) {
            lv_timer_delete(config_ota_blink_timer);
            config_ota_blink_timer = NULL;
        }
    }

    /* ── Create the needed grid if it doesn't exist yet ── */
    if (mode == DISPLAY_MODE_RELAY && relay_grid_container == NULL) {
        ESP_LOGI(TAG, "Creating relay grid...");
        create_relay_grid(screen_layout.screen);
        lv_obj_clear_flag(relay_grid_container, LV_OBJ_FLAG_HIDDEN);
    }
    if (mode == DISPLAY_MODE_BMS && bms_grid_container == NULL) {
        ESP_LOGI(TAG, "Creating BMS data grid...");
        bms_grid_container = create_data_grid(screen_layout.screen, bms_grid_map, bms_boxes);
        lv_obj_clear_flag(bms_grid_container, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "BMS data grid created (%dx%d = %d boxes)",
                 DATA_GRID_COLS, DATA_GRID_ROWS, DATA_GRID_TOTAL);
    }
    if (mode == DISPLAY_MODE_OBD && obd_grid_container == NULL) {
        if (s_debug_mode_active) {
            ESP_LOGI(TAG, "Creating OBD debug grid (5×3)...");
            obd_grid_container = create_data_grid(screen_layout.screen, obd_grid_map, obd_boxes);
            lv_obj_clear_flag(obd_grid_container, LV_OBJ_FLAG_HIDDEN);
            ESP_LOGI(TAG, "OBD debug grid created (%dx%d = %d boxes)",
                     DATA_GRID_COLS, DATA_GRID_ROWS, DATA_GRID_TOTAL);
        } else {
            ESP_LOGI(TAG, "Creating OBD dashboard...");
            obd_grid_container = create_obd_dashboard(screen_layout.screen);
            lv_obj_clear_flag(obd_grid_container, LV_OBJ_FLAG_HIDDEN);
            ESP_LOGI(TAG, "OBD dashboard created");
        }
    }
    if (mode == DISPLAY_MODE_CONFIG && config_grid_container == NULL) {
        ESP_LOGI(TAG, "Creating config grid...");
        create_config_grid(screen_layout.screen);
        lv_obj_clear_flag(config_grid_container, LV_OBJ_FLAG_HIDDEN);
    }

    /* Hide/show the arc group objects */
    lv_obj_t *arc_objects[] = {
        screen_layout.arc_outline,
        screen_layout.main_gauge,
        screen_layout.gauge_value,
        screen_layout.gauge_unit,
        screen_layout.gauge_minmax,
        screen_layout.shift_light_bar,
    };
    for (int i = 0; i < 6; i++) {
        if (arc_objects[i]) {
            if (is_grid_mode)
                lv_obj_add_flag(arc_objects[i], LV_OBJ_FLAG_HIDDEN);
            else
                lv_obj_clear_flag(arc_objects[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* 6 data sections — hide when in grid mode */
    for (int i = 0; i < 6; i++) {
        if (screen_layout.sections[i].section) {
            if (is_grid_mode)
                lv_obj_add_flag(screen_layout.sections[i].section, LV_OBJ_FLAG_HIDDEN);
            else
                lv_obj_clear_flag(screen_layout.sections[i].section, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Status bar always stays on top and visible */
    if (screen_layout.status_bar) {
        lv_obj_move_foreground(screen_layout.status_bar);
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 * Home status-bar ARM latch — reconciler + handlers
 * ════════════════════════════════════════════════════════════════════════════ */

/** Paint the ARM button to reflect current intent / reconcile state. */
static void home_arm_paint(const char *text, uint32_t bg, uint32_t border)
{
    if (home_arm_btn) {
        lv_obj_set_style_bg_color(home_arm_btn, lv_color_hex(bg), 0);
        lv_obj_set_style_border_color(home_arm_btn, lv_color_hex(border), 0);
    }
    if (home_arm_lbl) lv_label_set_text(home_arm_lbl, text);
}

/** Begin reconciling to a new arm intent (re-send fires on next timer tick). */
static void home_arm_set_intent(bool armed)
{
    home_arm_intent   = armed;
    home_arm_pending  = true;
    home_arm_resend_ms = lv_tick_get();   /* due immediately */
}

/* Hold-to-ARM: deliberate long-press only (guards against accidental taps). */
static void home_arm_longpress_cb(lv_event_t *e)
{
    (void)e;
    if (!home_arm_intent) {
        home_arm_set_intent(true);
        ESP_LOGW(TAG, "Parachute ARM requested (hold) — reconciling to MOS");
    }
}

/* Tap-to-DISARM: a short click always disarms (disarm must be easy + instant). */
static void home_arm_short_cb(lv_event_t *e)
{
    (void)e;
    if (home_arm_intent || home_arm_pending) {
        home_arm_set_intent(false);
        ESP_LOGW(TAG, "Parachute DISARM requested (tap) — reconciling to MOS");
    }
}

/**
 * @brief 400ms reconciler: drive every online MOS to the arm intent and confirm
 *        via its status echo. Self-healing — re-sends until all online MOS agree;
 *        offline MOS are ignored (they boot DISARMED, so safe by construction).
 */
static void home_arm_reconcile_cb(lv_timer_t *t)
{
    (void)t;
    if (!home_arm_btn) return;

    int online = 0, agree = 0;
    for (size_t i = 0; i < HOME_ARM_NODE_COUNT; i++) {
        if (!espnow_master_node_online(home_arm_nodes[i])) continue;
        online++;
        opendash_parachute_status_t st;
        if (espnow_master_get_parachute_status(home_arm_nodes[i], &st) &&
            ((st.armed != 0) == home_arm_intent)) {
            agree++;
        }
    }

    /* Confirmed once every online MOS echoes the intended arm state. */
    if (home_arm_pending && online > 0 && agree == online) {
        home_arm_pending = false;
    }

    /* While reconciling, re-send the arm command + solicit an echo ~1Hz. */
    if (home_arm_pending) {
        uint32_t now = lv_tick_get();
        if ((int32_t)(now - home_arm_resend_ms) >= 0) {
            for (size_t i = 0; i < HOME_ARM_NODE_COUNT; i++) {
                if (!espnow_master_node_online(home_arm_nodes[i])) continue;
                espnow_master_send_parachute_arm(home_arm_nodes[i], home_arm_intent);
                espnow_master_send_parachute_pull(home_arm_nodes[i]);
            }
            home_arm_resend_ms = now + 1000;
        }
    }

    /* Visual state. Red = live armed, amber = transitioning, grey = safe. */
    if (home_arm_intent) {
        if (home_arm_pending || online == 0) {
            home_arm_paint(online == 0 ? "ARMING (no MOS)" : "ARMING\xe2\x80\xa6",
                           0x884400, 0xFFAA00);
        } else {
            home_arm_paint("\xe2\x97\x8f ARMED", 0xCC1111, 0xFF3333);
        }
    } else {
        if (home_arm_pending && online > 0) {
            home_arm_paint("DISARMING\xe2\x80\xa6", 0x884400, 0xFFAA00);
        } else {
            home_arm_paint("ARM", 0x333333, 0x888888);
        }
    }
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
    /* Make status bar touchable for Debug-mode long-press exit */
    lv_obj_add_flag(screen_layout.status_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(screen_layout.status_bar, status_bar_longpress_cb,
                        LV_EVENT_LONG_PRESSED, NULL);
    
    screen_layout.status_text = create_outlined_label(screen_layout.status_bar,
                                                       "Initializing...",
                                                       OPENDASH_FONT_SIZE_MEDIUM,
                                                       OPENDASH_COLOR_TEXT_PRIMARY,
                                                       OPENDASH_COLOR_TEXT_OUTLINE,
                                                       1);
    lv_obj_center(screen_layout.status_text);

    /* ── Parachute ARM latch — lower-left of the persistent status bar ────
     * Hold to ARM (deliberate), tap to DISARM (instant). Red when live-armed.
     * Reconciler (home_arm_reconcile_cb) keeps every online MOS in sync and
     * confirms via each MOS's status echo. Defaults SAFE; never persisted. */
    home_arm_btn = lv_btn_create(screen_layout.status_bar);
    lv_obj_set_size(home_arm_btn, 132, 40);
    lv_obj_align(home_arm_btn, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_bg_color(home_arm_btn, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_color(home_arm_btn, lv_color_hex(0x888888), 0);
    lv_obj_set_style_border_width(home_arm_btn, 2, 0);
    lv_obj_set_style_radius(home_arm_btn, 6, 0);
    lv_obj_clear_flag(home_arm_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(home_arm_btn, home_arm_longpress_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(home_arm_btn, home_arm_short_cb, LV_EVENT_SHORT_CLICKED, NULL);
    home_arm_lbl = lv_label_create(home_arm_btn);
    lv_label_set_text(home_arm_lbl, "ARM");
    lv_obj_set_style_text_color(home_arm_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(home_arm_lbl);

    if (!home_arm_timer) {
        home_arm_timer = lv_timer_create(home_arm_reconcile_cb, 400, NULL);
    }

    /* All grids deferred to first mode switch — creating 120+ LVGL objects at
     * init makes the first full-screen render (splash→main in DIRECT mode)
     * too heavy, starving the WDT and corrupting framebuffers. */
    relay_grid_container = NULL;
    bms_grid_container = NULL;
    obd_grid_container = NULL;
    config_grid_container = NULL;
    ESP_LOGI(TAG, "Relay/BMS/OBD/Config grids deferred (will create on first switch)");

    /* ── MIL (Check Engine Light) — integrated into status bar ────────────
     * When CEL is active: status bar border turns orange, "CEL" label
     * appears on the left side of the bar. Blinks for visibility.
     * Visible on ALL screens including OBD config (status bar always visible). */
    mil_cel_label = create_simple_label(screen_layout.status_bar, "CEL",
                                         OPENDASH_FONT_SIZE_MEDIUM, 0xFF4400);
    lv_obj_align(mil_cel_label, LV_ALIGN_LEFT_MID, 144, 0);
    lv_obj_add_flag(mil_cel_label, LV_OBJ_FLAG_HIDDEN);  /* Hidden until MIL active */

    /* ── BLE-OTA badge — integrated into status bar (right side) ─────────
     * When any slave reports OPENDASH_STATUS_FLAG_BLE_OTA, the badge
     * appears as "BT-OTA: <node>" and the status-bar border turns cyan
     * and blinks for visibility. Cleared automatically when the node
     * either drops the flag (returns to RUNNING) or goes OFFLINE. */
    ota_badge_label = create_simple_label(screen_layout.status_bar, "BT-OTA",
                                           OPENDASH_FONT_SIZE_MEDIUM, 0x00CCFF);
    lv_obj_align(ota_badge_label, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_add_flag(ota_badge_label, LV_OBJ_FLAG_HIDDEN);  /* Hidden until any node enters BT OTA */

    ESP_LOGI(TAG, "Screen layout created with 6 data sections + center arc (grids deferred)");
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
    /* Track whether we are in any debug screen.
     * OBD is only "debug" if it wasn't entered from the normal cycle
     * (i.e., when we were already in debug mode via the DEBUG button). */
    if (new_mode == DISPLAY_MODE_RELAY || new_mode == DISPLAY_MODE_BMS) {
        s_debug_mode_active = true;
    } else if (new_mode == DISPLAY_MODE_OBD && !obd_config_get()->obd_enabled) {
        s_debug_mode_active = true;  /* OBD via debug cycle */
    } else if (new_mode != DISPLAY_MODE_OBD) {
        s_debug_mode_active = false;
    }
    /* If OBD is enabled + reached via normal cycle, s_debug_mode_active stays false */
    const display_mode_config_t *config = &mode_configs[current_mode];

    /* Toggle grid containers vs engine widgets */
    toggle_grid_visibility(current_mode);
    
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

    /* Re-apply cached values so the screen shows last-known data immediately
     * rather than "---" until the next incoming update batch.             */
    restore_mode_cached_values(new_mode);

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

/* ── Display mode cycles ─────────────────────────────────────────────────
 * Normal cycle: ENGINE → GPS → MD → [OBD if enabled] → CONFIG
 * Debug cycle:  RELAY → BMS → OBD (entered via CONFIG DEBUG button)
 * When OBD is enabled via NVS config, it appears in the normal cycle.
 * In debug mode, OBD is always accessible regardless of NVS setting.
 * ──────────────────────────────────────────────────────────────────────── */
static const display_mode_t s_normal_cycle_base[] = {
    DISPLAY_MODE_ENGINE, DISPLAY_MODE_GPS, DISPLAY_MODE_MD, DISPLAY_MODE_CONFIG
};
#define NORMAL_CYCLE_BASE_LEN  4

static const display_mode_t s_normal_cycle_obd[] = {
    DISPLAY_MODE_ENGINE, DISPLAY_MODE_GPS, DISPLAY_MODE_MD, DISPLAY_MODE_OBD, DISPLAY_MODE_CONFIG
};
#define NORMAL_CYCLE_OBD_LEN  5

static const display_mode_t s_debug_cycle[] = {
    DISPLAY_MODE_RELAY, DISPLAY_MODE_BMS, DISPLAY_MODE_OBD
};
#define DEBUG_CYCLE_LEN   3

static display_mode_t cycle_next(int delta)
{
    if (!s_debug_mode_active) {
        /* Choose cycle based on OBD enabled setting */
        const obd_config_t *cfg = obd_config_get();
        const display_mode_t *cycle;
        int len;
        if (cfg->obd_enabled) {
            cycle = s_normal_cycle_obd;
            len = NORMAL_CYCLE_OBD_LEN;
        } else {
            cycle = s_normal_cycle_base;
            len = NORMAL_CYCLE_BASE_LEN;
        }
        int pos = 0;
        for (int i = 0; i < len; i++) {
            if (cycle[i] == current_mode) { pos = i; break; }
        }
        pos = (pos + len + delta) % len;
        return cycle[pos];
    } else {
        int pos = 0;
        for (int i = 0; i < DEBUG_CYCLE_LEN; i++) {
            if (s_debug_cycle[i] == current_mode) { pos = i; break; }
        }
        pos = (pos + DEBUG_CYCLE_LEN + delta) % DEBUG_CYCLE_LEN;
        return s_debug_cycle[pos];
    }
}

/**
 * @brief Long-press handler for status bar — exits Debug mode.
 *
 * Tap-and-hold the status bar at the bottom of any debug screen
 * (RELAY, BMS, OBD) to return to the SYSTEM CONFIG screen.
 */
static void status_bar_longpress_cb(lv_event_t *e)
{
    (void)e;
    if (!s_debug_mode_active) return;
    s_debug_mode_active = false;
    update_display_mode(DISPLAY_MODE_CONFIG);
    ESP_LOGI(TAG, "Debug mode exited via status bar long-press → CONFIG");
}

/**
 * @brief Gesture event handler for screen navigation.
 *
 * Uses LVGL 9's built-in gesture detection (LV_EVENT_GESTURE)
 * which properly distinguishes swipes from taps and works
 * reliably even when child objects (grids, relay boxes) are present.
 * Normal mode cycles: ENGINE → GPS → MD → CONFIG
 * Debug mode cycles:  RELAY → BMS → OBD
 */
static void gesture_event_handler(lv_event_t *e)
{
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());

    /* Swallow all gestures while a modal editor is open so its contents
     * (cells, keyboard) don't get interrupted and the screen-level swipe
     * navigation can't tear down the modal mid-event. */
    if (boost_config_ui_is_active()) return;

    uint32_t now = esp_log_timestamp();
    if (now - last_swipe_time < 500) return;  /* Debounce */

    display_mode_t next_mode;
    if (dir == LV_DIR_LEFT) {
        next_mode = cycle_next(+1);
    } else if (dir == LV_DIR_RIGHT) {
        next_mode = cycle_next(-1);
    } else {
        return;  /* Ignore up/down gestures */
    }

    last_swipe_time = now;
    update_display_mode(next_mode);
    ESP_LOGI(TAG, "Gesture swipe %s → mode %d (debug=%d)",
             dir == LV_DIR_LEFT ? "LEFT" : "RIGHT", next_mode,
             s_debug_mode_active);
}

/**
 * @brief UI rendering task
 */
static void ui_task(void *pvParameters)
{
    ESP_LOGI(TAG, "UI task started");
    
    /* Subscribe to task watchdog so heavy renders don't trigger WDT */
    esp_task_wdt_add(NULL);
    
    while (1) {
        /* LVGL is NOT thread-safe.  All API calls (including
         * lv_timer_handler which renders & processes events) must
         * be serialised with the same mutex used by espnow_master
         * and any other task that touches LVGL objects.            */
        if (display_lvgl_lock(100)) {
            lv_timer_handler();
            display_lvgl_unlock();
        }
        esp_task_wdt_reset();
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

    /* Capture compiled defaults BEFORE NVS load so Factory Reset works,
     * then overlay any persisted per-mode layouts on top.               */
    capture_mode_defaults();
    load_layouts_from_nvs();

    /* Create the single screen with all LVGL objects */
    ESP_ERROR_CHECK(create_screen_layout());
    
    /* Set initial display mode and load screen */
    update_display_mode(DISPLAY_MODE_ENGINE);
    lv_scr_load(screen_layout.screen);

    /* Register gesture handler for swipe navigation (LVGL 9 built-in) */
    lv_obj_add_event_cb(screen_layout.screen, gesture_event_handler, LV_EVENT_GESTURE, NULL);

    ESP_LOGI(TAG, "Display initialized - swipe or boot button to cycle modes");
    ESP_LOGI(TAG, "Modes: ENGINE, GPS, MULTIDISPLAY, RELAY, BMS, OBD, CONFIG (%d total)", DISPLAY_MODE_COUNT);
    
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

/* Mutable so the Layout Editor / NVS-loaded layouts can rewrite it at
 * runtime. The compiled defaults below are also captured into
 * s_mode_dp_defaults[] so a Factory Reset can restore them.            */
static mode_dp_map_t mode_dp_maps[DISPLAY_MODE_COUNT] = {
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
    [DISPLAY_MODE_RELAY] = {
        /* Relay mode doesn't use section data points or arc — grid is interactive */
        .section_dp = {0, 0, 0, 0, 0, 0},
        .arc_dp  = 0,
        .arc_min = 0.0f,
        .arc_max = 0.0f,
    },
    [DISPLAY_MODE_BMS] = {
        /* BMS mode uses data grid — section data points not used */
        .section_dp = {0, 0, 0, 0, 0, 0},
        .arc_dp  = 0,
        .arc_min = 0.0f,
        .arc_max = 0.0f,
    },
    [DISPLAY_MODE_OBD] = {
        /* OBD mode uses data grid — section data points not used */
        .section_dp = {0, 0, 0, 0, 0, 0},
        .arc_dp  = 0,
        .arc_min = 0.0f,
        .arc_max = 0.0f,
    },
    [DISPLAY_MODE_CONFIG] = {
        /* Config mode — no data points, interactive buttons only */
        .section_dp = {0, 0, 0, 0, 0, 0},
        .arc_dp  = 0,
        .arc_min = 0.0f,
        .arc_max = 0.0f,
    },
};

/* Snapshot of compiled defaults — captured once at boot, used to provide
 * the initial layout for any mode that has no NVS entry, and as the
 * "Factory Reset" baseline. Filled by capture_mode_defaults().          */
static mode_dp_map_t s_mode_dp_defaults[DISPLAY_MODE_COUNT];
static bool          s_mode_dp_defaults_captured = false;

static void capture_mode_defaults(void)
{
    if (s_mode_dp_defaults_captured) return;
    memcpy(s_mode_dp_defaults, mode_dp_maps, sizeof(s_mode_dp_defaults));
    s_mode_dp_defaults_captured = true;
}

/* Convert a runtime mode_dp_map_t into the wire-format screen_layout_v1_t
 * (and vice-versa). The wire format uses a single arc_dp + N slots; we
 * map slot[0..5] ↔ section_dp[0..5].                                     */
static void mode_map_to_layout(uint8_t mode, const mode_dp_map_t *m,
                               screen_layout_v1_t *out)
{
    memset(out, 0, sizeof(*out));
    out->version    = OPENDASH_LAYOUT_VERSION;
    out->mode       = mode;
    out->slot_count = 6;
    out->arc_dp_id  = m->arc_dp;
    out->arc_min    = m->arc_min;
    out->arc_max    = m->arc_max;
    for (int i = 0; i < 6; i++) out->slot_dp_ids[i] = m->section_dp[i];
}

static void layout_to_mode_map(const screen_layout_v1_t *in, mode_dp_map_t *m)
{
    m->arc_dp  = in->arc_dp_id;
    m->arc_min = in->arc_min;
    m->arc_max = in->arc_max;
    int n = (in->slot_count < 6) ? in->slot_count : 6;
    for (int i = 0; i < n; i++) m->section_dp[i] = in->slot_dp_ids[i];
    for (int i = n; i < 6; i++) m->section_dp[i] = 0;
}

/* Load the on-disk layout for every mode and write it into mode_dp_maps[].
 * Modes with no NVS entry keep their compiled defaults. Called once
 * from ui_manager_init() after capture_mode_defaults().                  */
static void load_layouts_from_nvs(void)
{
    for (uint8_t mode = 0; mode < DISPLAY_MODE_COUNT; mode++) {
        screen_layout_v1_t layout;
        screen_layout_v1_t fallback;
        mode_map_to_layout(mode, &s_mode_dp_defaults[mode], &fallback);
        if (opendash_layout_load_or_default(mode, &fallback, &layout) == ESP_OK) {
            layout_to_mode_map(&layout, &mode_dp_maps[mode]);
        }
    }
    ESP_LOGI(TAG, "mode_dp_maps initialized from NVS (with compiled fallbacks)");
}

/* Apply a layout to the runtime table for the given mode. Persists to
 * NVS, propagates to the slave that owns the mode's UI (if any), and
 * (if it's the active mode) repaints cached values. Used by the
 * Layout Editor screen.                                                  */
esp_err_t ui_manager_apply_layout(uint8_t mode, const screen_layout_v1_t *layout)
{
    if (mode >= DISPLAY_MODE_COUNT || layout == NULL) return ESP_ERR_INVALID_ARG;
    layout_to_mode_map(layout, &mode_dp_maps[mode]);
    esp_err_t err = opendash_layout_save(mode, layout);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ui_manager_apply_layout: NVS save failed (0x%x)", err);
    }
    /* If this mode is currently displayed, refresh from cache so the
     * newly-mapped widgets pick up the latest known DP values.          */
    extern void ui_manager_refresh_current_mode(void);
    ui_manager_refresh_current_mode();
    return err;
}

esp_err_t ui_manager_get_layout(uint8_t mode, screen_layout_v1_t *out)
{
    if (mode >= DISPLAY_MODE_COUNT || out == NULL) return ESP_ERR_INVALID_ARG;
    mode_map_to_layout(mode, &mode_dp_maps[mode], out);
    return ESP_OK;
}

/* Track max values per section for the "Max:" label */
static float s_section_max[DISPLAY_MODE_COUNT][6];
static bool  s_section_has_data[DISPLAY_MODE_COUNT][6];

/* ── Data-point value cache ────────────────────────────────────────────
 * Stores the last-received value for every data point so that switching
 * screens can immediately show the previous reading instead of "---".   */
#define DP_CACHE_SIZE 64
typedef struct { uint16_t id; float val; bool valid; } dp_cache_entry_t;
static dp_cache_entry_t s_dp_cache[DP_CACHE_SIZE];

static void dp_cache_store(uint16_t id, float val)
{
    for (int i = 0; i < DP_CACHE_SIZE; i++) {
        if (s_dp_cache[i].valid && s_dp_cache[i].id == id) {
            s_dp_cache[i].val = val;
            return;
        }
    }
    for (int i = 0; i < DP_CACHE_SIZE; i++) {
        if (!s_dp_cache[i].valid) {
            s_dp_cache[i] = (dp_cache_entry_t){ .id = id, .val = val, .valid = true };
            return;
        }
    }
    /* Cache full — overwrite slot 0 (simple eviction) */
    s_dp_cache[0] = (dp_cache_entry_t){ .id = id, .val = val, .valid = true };
}

static bool dp_cache_get(uint16_t id, float *out)
{
    for (int i = 0; i < DP_CACHE_SIZE; i++) {
        if (s_dp_cache[i].valid && s_dp_cache[i].id == id) {
            *out = s_dp_cache[i].val;
            return true;
        }
    }
    return false;
}

/**
 * Re-apply the last-known cached values for the arc and 6 sections of the
 * given mode.  Called at the end of update_display_mode() so the screen
 * shows real data immediately instead of "---" after every mode switch.
 */
static void restore_mode_cached_values(display_mode_t mode)
{
    const mode_dp_map_t *m = &mode_dp_maps[mode];
    float cached;
    if (m->arc_dp != 0 && dp_cache_get(m->arc_dp, &cached)) {
        ui_manager_update_value(m->arc_dp, cached);
    }
    for (int i = 0; i < 6; i++) {
        if (m->section_dp[i] != 0 && dp_cache_get(m->section_dp[i], &cached)) {
            ui_manager_update_value(m->section_dp[i], cached);
        }
    }
}

/* Public: rebind/repaint the currently-active mode after a layout edit. */
void ui_manager_refresh_current_mode(void)
{
    restore_mode_cached_values(current_mode);
}

void ui_manager_update_value(uint16_t data_point_id, float value)
{
    /* Always cache every incoming value so mode-switches can restore it */
    dp_cache_store(data_point_id, value);

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

    /* ── Check BMS data grid ───────────────────────────────── */
    if (current_mode == DISPLAY_MODE_BMS) {
        for (int i = 0; i < DATA_GRID_TOTAL; i++) {
            if (data_point_id != bms_grid_map[i].data_point_id) continue;
            if (bms_boxes[i].value == NULL) return;

            float display_val = value;
            /* Convert temps to °F for BMS/VESC temp fields */
            if (data_point_id == OPENDASH_DP_BMS_TEMP_MAX ||
                data_point_id == OPENDASH_DP_BMS_TEMP_IC ||
                data_point_id == OPENDASH_DP_VESC_TEMP_FET ||
                data_point_id == OPENDASH_DP_VESC_TEMP_MOTOR) {
                display_val = opendash_convert_temp(value, current_layout.temp_unit);
            }

            char fmt_buf[24];
            switch (bms_grid_map[i].decimals) {
                case 1:  snprintf(fmt_buf, sizeof(fmt_buf), "%.1f", display_val); break;
                case 2:  snprintf(fmt_buf, sizeof(fmt_buf), "%.2f", display_val); break;
                case 3:  snprintf(fmt_buf, sizeof(fmt_buf), "%.3f", display_val); break;
                default: snprintf(fmt_buf, sizeof(fmt_buf), "%.0f", display_val); break;
            }
            lv_label_set_text(bms_boxes[i].value, fmt_buf);
            return;
        }
    }

    /* ── Check OBD dashboard ─────────────────────────────────── */
    if (current_mode == DISPLAY_MODE_OBD) {
        for (int i = 0; i < DATA_GRID_TOTAL; i++) {
            if (data_point_id != obd_grid_map[i].data_point_id) continue;
            if (obd_boxes[i].value == NULL) return;

            float display_val = value;
            /* Apply unit conversions */
            if (data_point_id == OPENDASH_DP_COOLANT_TEMP ||
                data_point_id == OPENDASH_DP_INTAKE_TEMP ||
                data_point_id == OPENDASH_DP_OIL_TEMP ||
                data_point_id == OPENDASH_DP_EGT) {
                display_val = opendash_convert_temp(value, current_layout.temp_unit);
            } else if (data_point_id == OPENDASH_DP_BOOST_PRESSURE ||
                       data_point_id == OPENDASH_DP_OIL_PRESSURE ||
                       data_point_id == OPENDASH_DP_FUEL_PRESSURE) {
                display_val = opendash_convert_pressure(value, current_layout.pressure_unit);
            } else if (data_point_id == OPENDASH_DP_VEHICLE_SPEED) {
                display_val = opendash_convert_speed(value, current_layout.speed_unit);
            }

            char fmt_buf[24];
            switch (obd_grid_map[i].decimals) {
                case 1:  snprintf(fmt_buf, sizeof(fmt_buf), "%.1f", display_val); break;
                case 2:  snprintf(fmt_buf, sizeof(fmt_buf), "%.2f", display_val); break;
                default: snprintf(fmt_buf, sizeof(fmt_buf), "%.0f", display_val); break;
            }
            lv_label_set_text(obd_boxes[i].value, fmt_buf);

            /* Update RPM arc gauge if this is RPM data */
            if (data_point_id == OPENDASH_DP_RPM && obd_rpm_arc) {
                lv_arc_set_value(obd_rpm_arc, (int32_t)value);
                /* Color shift: cyan → red as RPM increases */
                if (value > 6500.0f) {
                    lv_obj_set_style_arc_color(obd_rpm_arc,
                        lv_color_hex(0xFF0040), LV_PART_INDICATOR);
                } else if (value > 5000.0f) {
                    lv_obj_set_style_arc_color(obd_rpm_arc,
                        lv_color_hex(0xFF6B35), LV_PART_INDICATOR);
                } else {
                    lv_obj_set_style_arc_color(obd_rpm_arc,
                        lv_color_hex(0x00BFFF), LV_PART_INDICATOR);
                }
            }
            return;
        }
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

/* ── Power Fault Notification ─────────────────────────────────────────────
 * Temporarily flashes the status bar RED and shows an alert message when a
 * relay/MOS node reboots (power loss detected). Auto-restores after 5s.
 * ──────────────────────────────────────────────────────────────────────── */
static lv_timer_t *s_power_fault_timer = NULL;
static uint32_t    s_power_fault_start = 0;

static void power_fault_timer_cb(lv_timer_t *timer)
{
    uint32_t elapsed = esp_log_timestamp() - s_power_fault_start;

    /* Flash: toggle status bar border between RED and normal every 250ms */
    bool red_phase = ((elapsed / 250) % 2) == 0;
    if (screen_layout.status_bar) {
        lv_obj_set_style_border_color(screen_layout.status_bar,
            lv_color_hex(red_phase ? 0xFF0000 : 0xFFFFFF), 0);
    }

    /* After 5s: restore normal status bar and clear */
    if (elapsed > 5000) {
        if (screen_layout.status_bar) {
            lv_obj_set_style_border_color(screen_layout.status_bar,
                lv_color_hex(0xFFFFFF), 0);
        }
        if (screen_layout.status_text) {
            const display_mode_config_t *cfg = &mode_configs[current_mode];
            update_outlined_label_text(screen_layout.status_text, cfg->status_text);
        }
        lv_timer_delete(s_power_fault_timer);
        s_power_fault_timer = NULL;
    }
}

void ui_manager_show_power_fault(const char *fault_node_name)
{
    /* Build alert text */
    static char fault_msg[80];
    snprintf(fault_msg, sizeof(fault_msg),
             "\xE2\x9A\xA1 POWER FAULT: %s — RESTORING STATE", fault_node_name);

    /* Override status bar */
    if (screen_layout.status_text) {
        update_outlined_label_text(screen_layout.status_text, fault_msg);
    }
    if (screen_layout.status_bar) {
        lv_obj_set_style_border_color(screen_layout.status_bar,
            lv_color_hex(0xFF0000), 0);
    }

    /* Start or restart the flash/auto-clear timer */
    s_power_fault_start = esp_log_timestamp();
    if (s_power_fault_timer) {
        lv_timer_reset(s_power_fault_timer);
    } else {
        s_power_fault_timer = lv_timer_create(power_fault_timer_cb, 250, NULL);
    }

    ESP_LOGW(TAG, "⚡ Power fault displayed: %s", fault_node_name);
}

esp_err_t ui_manager_next_screen(void)
{
    update_display_mode(cycle_next(+1));
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

void ui_manager_update_config_node_status(const espnow_master_node_status_t *status)
{
    if (current_mode != DISPLAY_MODE_CONFIG || !config_grid_container) return;
    (void)status;  /* Legacy struct — we now use node_health directly */

    /* Map config_nodes[] index → opendash_node_t for health lookup */
    static const opendash_node_t node_map[CONFIG_NODE_COUNT] = {
        OPENDASH_NODE_LEFT,
        OPENDASH_NODE_RIGHT,
        OPENDASH_NODE_GPS,
        OPENDASH_NODE_BMS,
        OPENDASH_NODE_POD1,
        OPENDASH_NODE_POD2,
        OPENDASH_NODE_RELAY_4CH,
        OPENDASH_NODE_RELAY_8CH_A,
        OPENDASH_NODE_RELAY_8CH_B,
        OPENDASH_NODE_MOS_4CH_A,
        OPENDASH_NODE_MOS_4CH_B,
    };

    /* Cache last-rendered (state, flags) per chip so we only repaint on
     * actual change. Without this the 200 ms espnow_master heartbeat causes
     * every chip to invalidate at 5 Hz, which the eye reads as cycle-flash
     * and wastes display bandwidth. */
    uint8_t *last_state = config_chip_last_state;
    uint8_t *last_flags = config_chip_last_flags;

    bool any_in_ota = false;

    for (int i = 0; i < CONFIG_NODE_COUNT; i++) {
        if (!config_nodes[i].status_label) continue;

        uint8_t flags = node_health_get_status_flags(node_map[i]);
        bool in_ota   = (flags & OPENDASH_STATUS_FLAG_BLE_OTA) != 0;
        node_health_state_t state = node_health_get_state(node_map[i]);

        if (in_ota) any_in_ota = true;

        /* Skip repaint when nothing changed for this chip. */
        if ((uint8_t)state == last_state[i] && flags == last_flags[i]) continue;
        last_state[i] = (uint8_t)state;
        last_flags[i] = flags;

        if (in_ota) {
            lv_label_set_text(config_nodes[i].status_label, "BT-OTA");
            lv_obj_set_style_text_color(config_nodes[i].status_label,
                lv_color_hex(config_ota_blink_visible ? 0x00CCFF : 0x004466), 0);
            if (config_nodes[i].box) {
                lv_obj_set_style_border_width(config_nodes[i].box, 3, 0);
                lv_obj_set_style_border_color(config_nodes[i].box,
                    lv_color_hex(config_ota_blink_visible ? 0x00CCFF : 0x003355), 0);
            }
            continue;
        }

        /* Not in OTA — restore default chip border */
        if (config_nodes[i].box) {
            lv_obj_set_style_border_width(config_nodes[i].box, 1, 0);
            lv_obj_set_style_border_color(config_nodes[i].box, lv_color_hex(0xFFFFFF), 0);
        }

        switch (state) {
            case NODE_STATE_ONLINE:
                lv_label_set_text(config_nodes[i].status_label, "ONLINE");
                lv_obj_set_style_text_color(config_nodes[i].status_label,
                                             lv_color_hex(0x4ECCA3), 0);
                break;

            case NODE_STATE_DEGRADED:
                lv_label_set_text(config_nodes[i].status_label, "DEGRADED");
                lv_obj_set_style_text_color(config_nodes[i].status_label,
                                             lv_color_hex(0xFFAA00), 0);
                break;

            case NODE_STATE_AWAITING:
                lv_label_set_text(config_nodes[i].status_label, "AWAITING");
                lv_obj_set_style_text_color(config_nodes[i].status_label,
                                             lv_color_hex(0x6688FF), 0);
                break;

            case NODE_STATE_OFFLINE:
                lv_label_set_text(config_nodes[i].status_label, "OFFLINE");
                lv_obj_set_style_text_color(config_nodes[i].status_label,
                                             lv_color_hex(0xFF4444), 0);
                break;

            case NODE_STATE_UNKNOWN:
            default:
                lv_label_set_text(config_nodes[i].status_label, "---");
                lv_obj_set_style_text_color(config_nodes[i].status_label,
                                             lv_color_hex(0x666666), 0);
                break;
        }
    }

    /* Re-evaluate any_in_ota across ALL chips even if we skipped them
     * above — the blink-timer lifecycle is global, not per-chip. */
    if (!any_in_ota) {
        for (int i = 0; i < CONFIG_NODE_COUNT; i++) {
            if ((node_health_get_status_flags(node_map[i]) &
                 OPENDASH_STATUS_FLAG_BLE_OTA) != 0) {
                any_in_ota = true;
                break;
            }
        }
    }

    /* Manage the blink timer lifecycle based on whether any node is in OTA. */
    if (any_in_ota && !config_ota_blink_timer) {
        config_ota_blink_timer = lv_timer_create(config_ota_blink_timer_cb, 400, NULL);
    } else if (!any_in_ota && config_ota_blink_timer) {
        lv_timer_delete(config_ota_blink_timer);
        config_ota_blink_timer = NULL;
        config_ota_blink_visible = true;
    }
}

/* ── MIL Indicator Public API ─────────────────────────────────────────── */

static void mil_blink_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!screen_layout.status_bar || !mil_cel_label) return;
    mil_blink_visible = !mil_blink_visible;
    if (mil_blink_visible) {
        /* Orange border + CEL label visible */
        lv_obj_set_style_border_color(screen_layout.status_bar, lv_color_hex(0xFF6600), 0);
        lv_obj_clear_flag(mil_cel_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        /* Dim border + CEL hidden (blink off phase) */
        lv_obj_set_style_border_color(screen_layout.status_bar, lv_color_hex(0x663300), 0);
        lv_obj_add_flag(mil_cel_label, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_manager_mil_indicator_update(bool mil_on)
{
    if (!screen_layout.status_bar || !mil_cel_label) return;

    const obd_config_t *cfg = obd_config_get();
    bool show = mil_on && cfg->mil_indicator_enabled;

    if (show && !mil_currently_active) {
        /* Activate: orange border + CEL label + blink timer */
        mil_currently_active = true;
        lv_obj_set_style_border_color(screen_layout.status_bar, lv_color_hex(0xFF6600), 0);
        lv_obj_clear_flag(mil_cel_label, LV_OBJ_FLAG_HIDDEN);
        mil_blink_visible = true;
        if (!mil_blink_timer) {
            mil_blink_timer = lv_timer_create(mil_blink_timer_cb, 500, NULL);
        }
        ESP_LOGW(TAG, "MIL indicator activated — CHECK ENGINE (status bar orange)");
    } else if (!show && mil_currently_active) {
        /* Deactivate: restore white border, hide CEL label, stop blink */
        mil_currently_active = false;
        if (mil_blink_timer) {
            lv_timer_delete(mil_blink_timer);
            mil_blink_timer = NULL;
        }
        lv_obj_set_style_border_color(screen_layout.status_bar, lv_color_hex(0xFFFFFF), 0);
        lv_obj_add_flag(mil_cel_label, LV_OBJ_FLAG_HIDDEN);
        mil_blink_visible = true;
        ESP_LOGI(TAG, "MIL indicator cleared (status bar white)");
    }
}

/* ── BLE-OTA Indicator Public API ─────────────────────────────────────── */

/* Legacy status-bar OTA blink timer callback — retained but unused after the
 * banner was retired in favour of the per-chip CONFIG-screen blink. Kept so
 * any stray timer hook still resolves; marked unused to silence -Werror. */
__attribute__((unused))
static void ota_blink_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!screen_layout.status_bar || !ota_badge_label) return;
    ota_blink_visible = !ota_blink_visible;
    if (ota_blink_visible) {
        lv_obj_clear_flag(ota_badge_label, LV_OBJ_FLAG_HIDDEN);
        /* Don't fight MIL for the border colour — only own it when MIL is idle. */
        if (!mil_currently_active) {
            lv_obj_set_style_border_color(screen_layout.status_bar, lv_color_hex(0x00CCFF), 0);
        }
    } else {
        lv_obj_add_flag(ota_badge_label, LV_OBJ_FLAG_HIDDEN);
        if (!mil_currently_active) {
            lv_obj_set_style_border_color(screen_layout.status_bar, lv_color_hex(0x006688), 0);
        }
    }
}

void ui_manager_refresh_ota_banner(void)
{
    /* Legacy status-bar BT-OTA badge has been retired in favour of the
     * per-node blue blink on the CONFIG-screen device chip. Keep the symbol
     * so existing timer hooks still link, but force the badge hidden and the
     * status bar back to its neutral border colour whenever called. */
    if (ota_badge_label) {
        lv_obj_add_flag(ota_badge_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (ota_blink_timer) {
        lv_timer_delete(ota_blink_timer);
        ota_blink_timer = NULL;
    }
    if (ota_currently_active) {
        ota_currently_active = false;
        if (screen_layout.status_bar && !mil_currently_active) {
            lv_obj_set_style_border_color(screen_layout.status_bar, lv_color_hex(0xFFFFFF), 0);
        }
        ESP_LOGI(TAG, "BLE-OTA badge retired (chip blink handles indication)");
    }
}

bool ui_manager_is_obd_enabled(void)
{
    return obd_config_get()->obd_enabled;
}

void ui_manager_update_dtc_list(const char codes[][6], uint8_t count)
{
    if (!obd_dtc_list_label) return;

    char dtc_buf[256];
    if (count > 0) {
        int off = snprintf(dtc_buf, sizeof(dtc_buf), "DTCs (%d): ", count);
        for (int i = 0; i < count && i < 16; i++) {
            off += snprintf(dtc_buf + off, sizeof(dtc_buf) - off,
                           "%s%s", codes[i],
                           (i < count - 1) ? ", " : "");
            if (off >= (int)sizeof(dtc_buf) - 10) break;
        }
    } else {
        snprintf(dtc_buf, sizeof(dtc_buf), "DTCs: None detected");
    }
    lv_label_set_text(obd_dtc_list_label, dtc_buf);
}
