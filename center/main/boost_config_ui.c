/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file boost_config_ui.c
 * @brief LVGL System Config screen — Boost editor.
 *
 *  ┌──────────────────────────────────────────────────────────────────────┐
 *  │ BOOST CONFIG                                  [Close]                │
 *  │ Units: [kPa][BAR][PSI]  Target: MOS_4CH_A [Switch][Re-pull]          │
 *  │ Mode:  [OFF][NORMAL][RACE]                                            │
 *  │ ─── UNSTABLE LINK / LIMP MODE ───   (red banner, only when stale)    │
 *  │ Live: setpoint 0.45 BAR   actual 0.32 BAR   duty 168/255             │
 *  │ Edit slot: [NORMAL][RACE]   Gear: [1][2][3][4][5][6]                 │
 *  │ ┌─────┬────┬────┬────┬────┬────┬────┬────┬────┬────┬────┬────┬────┬─┐│
 *  │ │ RPM │ ...│    │    │    │    │    │    │    │    │    │    │    │ ││
 *  │ │ DUTY│ btn│ btn│ btn│ btn│ btn│ btn│ btn│ btn│ btn│ btn│ btn│ btn│ ││
 *  │ │ SETP│ btn│ btn│ btn│ btn│ btn│ btn│ btn│ btn│ btn│ btn│ btn│ btn│ ││
 *  │ └─────┴────┴────┴────┴────┴────┴────┴────┴────┴────┴────┴────┴────┴─┘│
 *  └──────────────────────────────────────────────────────────────────────┘
 *
 * Cell tap → numeric keypad popup (same lv_keyboard pattern used by the
 * layout editor) pre-loaded with the cell's current value. OK commits and
 * pushes the row to the slave. Cancel discards.
 *
 * Pressure-related cells (setpoint, live readout) honour the user's unit
 * preference (kPa / BAR / PSI). Internally everything is still centi-bar
 * on the wire (1 cBar ≡ 1 kPa).
 */

#include "boost_config_ui.h"
#include "boost_client.h"
#include "espnow_master.h"
#include "system_config.h"
#include "opendash_boost.h"
#include "opendash_common.h"
#include "opendash_ui_styles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"

static const char *TAG = "boost_ui";

/* ============================================================================
 *  Editor state
 * ==========================================================================*/

static lv_obj_t *s_root        = NULL;
static lv_obj_t *s_lbl_actual  = NULL;
static lv_obj_t *s_lbl_setp    = NULL;
static lv_obj_t *s_lbl_duty    = NULL;
static lv_obj_t *s_lbl_flags   = NULL;
static lv_obj_t *s_lbl_target  = NULL;
static lv_obj_t *s_lbl_link    = NULL;   /* "UNSTABLE LINK" banner */
static lv_obj_t *s_btn_unit[3] = {0};
static lv_obj_t *s_btn_mode[3] = {0};    /* OFF / NORMAL / RACE */
static lv_obj_t *s_btn_chan[4] = {0};    /* CH1..CH4 boost output channels */
static lv_obj_t *s_btn_slot[2] = {0};    /* NORMAL / RACE       */
static lv_obj_t *s_btn_gear[OPENDASH_BOOST_GEARS] = {0};
static lv_obj_t *s_btn_page[OPENDASH_BOOST_UI_PAGES] = {0};

static lv_obj_t *s_cell_duty[OPENDASH_BOOST_UI_PAGE_SIZE];
static lv_obj_t *s_cell_setp[OPENDASH_BOOST_UI_PAGE_SIZE];
static lv_obj_t *s_cell_rpm [OPENDASH_BOOST_UI_PAGE_SIZE];

static lv_timer_t *s_tick_timer = NULL;

static void (*s_close_cb)(void) = NULL;

static uint8_t s_edit_slot = OPENDASH_BOOST_MODE_NORMAL;
static uint8_t s_edit_gear = 0;
static uint8_t s_page      = 0;   /* 0 = 0..7.5k RPM, 1 = 8..15.5k RPM */
static opendash_boost_mode_t s_active_mode = OPENDASH_BOOST_MODE_OFF;

static uint8_t  s_buf_duty[OPENDASH_BOOST_MAP_POINTS];
static uint16_t s_buf_setp[OPENDASH_BOOST_MAP_POINTS];

/* Keypad popup. Created lazily on first cell tap, reused thereafter. */
typedef enum { KP_KIND_DUTY = 0, KP_KIND_SETP = 1 } keypad_kind_t;
static lv_obj_t *s_kp_root = NULL;
static lv_obj_t *s_kp_ta   = NULL;
static lv_obj_t *s_kp_kbd  = NULL;
static lv_obj_t *s_kp_title= NULL;
static keypad_kind_t s_kp_kind = KP_KIND_DUTY;
static int8_t        s_kp_idx  = -1;

/* ============================================================================
 *  Helpers
 * ==========================================================================*/

static const char *node_label(opendash_node_t n)
{
    switch (n) {
        case OPENDASH_NODE_MOS_4CH_A: return "MOS_4CH_A";
        case OPENDASH_NODE_MOS_4CH_B: return "MOS_4CH_B";
        default:                       return "(disabled)";
    }
}

static const char *mode_label(opendash_boost_mode_t m)
{
    switch (m) {
        case OPENDASH_BOOST_MODE_NORMAL: return "NORMAL";
        case OPENDASH_BOOST_MODE_RACE:   return "RACE";
        default:                         return "OFF";
    }
}

/* Centi-bar → display unit. cBar and kPa are 1:1 (1 bar = 100 kPa). */
static inline float cbar_to_unit(int cbar, opendash_pressure_unit_t u)
{
    return opendash_convert_pressure((float)cbar, u);
}

/* Display unit → centi-bar (rounded to nearest, clamped ≥0). */
static int unit_to_cbar(float v, opendash_pressure_unit_t u)
{
    float kpa;
    switch (u) {
        case OPENDASH_PRESSURE_BAR: kpa = v * 100.0f;       break;
        case OPENDASH_PRESSURE_PSI: kpa = v / 0.1450377f;   break;
        default:                    kpa = v;                break;   /* kPa */
    }
    if (kpa < 0) kpa = 0;
    int cbar = (int)(kpa + 0.5f);   /* cBar == kPa numerically */
    if (cbar > 65535) cbar = 65535;
    return cbar;
}

static const char *unit_suffix(opendash_pressure_unit_t u)
{
    return opendash_get_pressure_suffix(u);
}

static int unit_decimals(opendash_pressure_unit_t u)
{
    return (u == OPENDASH_PRESSURE_BAR) ? 2 : (u == OPENDASH_PRESSURE_KPA ? 0 : 1);
}

static void load_buffers_from_client(void)
{
    if (!boost_client_peek_duty_row(s_edit_slot, s_edit_gear, s_buf_duty)) {
        memset(s_buf_duty, 0, sizeof(s_buf_duty));
    }
    if (!boost_client_peek_setpoint_row(s_edit_slot, s_edit_gear, s_buf_setp)) {
        memset(s_buf_setp, 0, sizeof(s_buf_setp));
    }
}

static void style_toggle(lv_obj_t *btn, bool on)
{
    if (!btn) return;
    /* Avoid lv_obj_add_state(CHECKED) — it walks the style-transition list and
     * crashes on theme transition_normal dsc. Toggle bg color manually. */
    lv_color_t c = on ? lv_palette_main(LV_PALETTE_BLUE)
                      : lv_palette_darken(LV_PALETTE_GREY, 3);
    lv_obj_set_style_bg_color(btn, c, LV_PART_MAIN);
}

static void refresh_unit_buttons(void)
{
    for (int i = 0; i < 3; ++i) style_toggle(s_btn_unit[i], i == (int)g_boost_pressure_unit);
}

static void refresh_mode_buttons(void)
{
    style_toggle(s_btn_mode[0], s_active_mode == OPENDASH_BOOST_MODE_OFF);
    style_toggle(s_btn_mode[1], s_active_mode == OPENDASH_BOOST_MODE_NORMAL);
    style_toggle(s_btn_mode[2], s_active_mode == OPENDASH_BOOST_MODE_RACE);
}

/* ── Output channel selection + cross-module conflict resolution ──────────
 * Boost owns one or more MOS power channels (bit0=CH1..bit3=CH4) on the target
 * node. Channels already committed to the safety-deployment system on the same
 * MOS are greyed out and unselectable, so the two subsystems never fight over a
 * FET. To reclaim such a channel the user must free it on the Deploy panel. */

static uint8_t boost_current_mask(void)
{
    opendash_boost_params_t p;
    if (boost_client_peek_params(&p)) return p.output_mask & 0x0Fu;
    return 0x08;   /* default: CH4 (legacy N75 wiring) until the slave echoes */
}

static uint8_t boost_foreign_mask(void)
{
    return espnow_master_parachute_reserved_mask(g_boost_target_node);
}

static void refresh_chan_buttons(void)
{
    uint8_t mask    = boost_current_mask();
    uint8_t foreign = boost_foreign_mask();
    for (int c = 0; c < 4; ++c) {
        if (!s_btn_chan[c]) continue;
        bool reserved = (foreign >> c) & 1u;
        bool on       = (mask    >> c) & 1u;
        if (reserved) {
            /* Owned by the safety system — dark, disabled, not selectable. */
            lv_obj_add_state(s_btn_chan[c], LV_STATE_DISABLED);
            lv_obj_set_style_bg_color(s_btn_chan[c], lv_color_hex(0x2A2A2A), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(s_btn_chan[c], LV_OPA_70, LV_PART_MAIN);
        } else {
            lv_obj_remove_state(s_btn_chan[c], LV_STATE_DISABLED);
            lv_obj_set_style_bg_opa(s_btn_chan[c], LV_OPA_COVER, LV_PART_MAIN);
            style_toggle(s_btn_chan[c], on);
        }
    }
}

static void on_chan_click(lv_event_t *e)
{
    int c = (int)(intptr_t)lv_event_get_user_data(e);
    if (c < 0 || c > 3) return;

    uint8_t foreign = boost_foreign_mask();
    if (foreign & (1u << c)) {
        ESP_LOGW(TAG, "CH%d reserved by safety deployment on %s — free it there first",
                 c + 1, node_label(g_boost_target_node));
        refresh_chan_buttons();
        return;
    }

    opendash_boost_params_t p;
    if (!boost_client_peek_params(&p)) opendash_boost_default_params(&p);
    p.version     = OPENDASH_BOOST_PARAMS_VERSION;
    p.output_mask = (uint8_t)((p.output_mask ^ (1u << c)) & 0x0Fu);
    p.output_mask &= (uint8_t)~foreign;   /* belt-and-suspenders: never overlap safety */
    boost_client_set_params(&p);
    ESP_LOGI(TAG, "Boost output mask → 0x%X on %s", p.output_mask,
             node_label(g_boost_target_node));
    refresh_chan_buttons();
}

static void refresh_slot_buttons(void)
{
    style_toggle(s_btn_slot[0], s_edit_slot == OPENDASH_BOOST_MODE_NORMAL);
    style_toggle(s_btn_slot[1], s_edit_slot == OPENDASH_BOOST_MODE_RACE);
}

static void refresh_gear_buttons(void)
{
    for (int g = 0; g < OPENDASH_BOOST_GEARS; ++g) style_toggle(s_btn_gear[g], g == s_edit_gear);
}

static void refresh_page_buttons(void)
{
    for (int p = 0; p < OPENDASH_BOOST_UI_PAGES; ++p) style_toggle(s_btn_page[p], p == s_page);
}

/* Update RPM header labels for the currently-visible page. */
static void refresh_rpm_header(void)
{
    const float bin = (float)OPENDASH_BOOST_RPM_MAX / (OPENDASH_BOOST_MAP_POINTS - 1);
    char tmp[8];
    int base = (int)s_page * OPENDASH_BOOST_UI_PAGE_SIZE;
    for (int i = 0; i < OPENDASH_BOOST_UI_PAGE_SIZE; ++i) {
        int rpm = (int)((base + i) * bin + 0.5f);
        snprintf(tmp, sizeof(tmp), "%d", rpm);
        if (s_cell_rpm[i]) lv_label_set_text(s_cell_rpm[i], tmp);
    }
}

static void refresh_cells(void)
{
    char tmp[16];
    int dec = unit_decimals(g_boost_pressure_unit);
    int base = (int)s_page * OPENDASH_BOOST_UI_PAGE_SIZE;
    for (int i = 0; i < OPENDASH_BOOST_UI_PAGE_SIZE; ++i) {
        int idx = base + i;
        if (idx >= OPENDASH_BOOST_MAP_POINTS) break;
        if (s_cell_duty[i]) {
            snprintf(tmp, sizeof(tmp), "%u", s_buf_duty[idx]);
            lv_label_set_text(s_cell_duty[i], tmp);
        }
        if (s_cell_setp[i]) {
            float v = cbar_to_unit(s_buf_setp[idx], g_boost_pressure_unit);
            snprintf(tmp, sizeof(tmp), "%.*f", dec, v);
            lv_label_set_text(s_cell_setp[i], tmp);
        }
    }
}

/* ============================================================================
 *  Keypad popup
 * ==========================================================================*/

static void keypad_close(void)
{
    if (s_kp_root) {
        lv_obj_del(s_kp_root);
        s_kp_root  = NULL;
        s_kp_ta    = NULL;
        s_kp_kbd   = NULL;
        s_kp_title = NULL;
    }
    s_kp_idx = -1;
}

static void on_kp_cancel(lv_event_t *e)
{
    (void)e;
    keypad_close();
}

static void on_kp_ok(lv_event_t *e)
{
    (void)e;
    if (!s_kp_ta || s_kp_idx < 0 || s_kp_idx >= OPENDASH_BOOST_MAP_POINTS) {
        keypad_close();
        return;
    }
    const char *txt = lv_textarea_get_text(s_kp_ta);
    if (!txt || !*txt) { keypad_close(); return; }

    if (s_kp_kind == KP_KIND_DUTY) {
        int v = atoi(txt);
        if (v < 0)   v = 0;
        if (v > 255) v = 255;
        s_buf_duty[s_kp_idx] = (uint8_t)v;
        boost_client_set_duty_row(s_edit_slot, s_edit_gear, s_buf_duty);
    } else {
        float f = strtof(txt, NULL);
        int cbar = unit_to_cbar(f, g_boost_pressure_unit);
        if (cbar > 65535) cbar = 65535;
        s_buf_setp[s_kp_idx] = (uint16_t)cbar;
        boost_client_set_setpoint_row(s_edit_slot, s_edit_gear, s_buf_setp);
    }
    refresh_cells();
    keypad_close();
}

static void on_kbd_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY)  on_kp_ok(e);
    if (code == LV_EVENT_CANCEL) on_kp_cancel(e);
}

static void keypad_open(keypad_kind_t kind, int idx)
{
    if (idx < 0 || idx >= OPENDASH_BOOST_MAP_POINTS) return;
    keypad_close();
    s_kp_kind = kind;
    s_kp_idx  = (int8_t)idx;

    s_kp_root = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_kp_root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_kp_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_kp_root, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(s_kp_root, 8, 0);
    lv_obj_set_layout(s_kp_root, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_kp_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_kp_root, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(s_kp_root, LV_OBJ_FLAG_SCROLLABLE);
    /* Soak up gestures so they cannot escape to the screen-level handler. */
    lv_obj_add_flag(s_kp_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_kp_root, LV_OBJ_FLAG_GESTURE_BUBBLE);

    s_kp_title = lv_label_create(s_kp_root);
    if (kind == KP_KIND_DUTY) {
        lv_label_set_text_fmt(s_kp_title,
                              "DUTY cell %d  (slot %s, gear %d)  range 0-255",
                              idx + 1, mode_label((opendash_boost_mode_t)s_edit_slot),
                              s_edit_gear + 1);
    } else {
        lv_label_set_text_fmt(s_kp_title,
                              "SETPOINT cell %d  (slot %s, gear %d)  unit %s",
                              idx + 1, mode_label((opendash_boost_mode_t)s_edit_slot),
                              s_edit_gear + 1, unit_suffix(g_boost_pressure_unit));
    }

    s_kp_ta = lv_textarea_create(s_kp_root);
    lv_obj_set_width(s_kp_ta, 240);
    lv_textarea_set_one_line(s_kp_ta, true);
    lv_textarea_set_accepted_chars(s_kp_ta, "0123456789.-");

    char preload[16];
    if (kind == KP_KIND_DUTY) {
        snprintf(preload, sizeof(preload), "%u", s_buf_duty[idx]);
    } else {
        snprintf(preload, sizeof(preload), "%.*f",
                 unit_decimals(g_boost_pressure_unit),
                 cbar_to_unit(s_buf_setp[idx], g_boost_pressure_unit));
    }
    lv_textarea_set_text(s_kp_ta, preload);

    /* OK / Cancel row */
    lv_obj_t *btn_row = lv_obj_create(s_kp_root);
    lv_obj_set_size(btn_row, 260, LV_SIZE_CONTENT);
    lv_obj_set_layout(btn_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *btn_ok = lv_button_create(btn_row);
    lv_obj_add_event_cb(btn_ok, on_kp_ok, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_ok = lv_label_create(btn_ok); lv_label_set_text(lbl_ok, "OK");
    lv_obj_center(lbl_ok);

    lv_obj_t *btn_cancel = lv_button_create(btn_row);
    lv_obj_add_event_cb(btn_cancel, on_kp_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_cancel = lv_label_create(btn_cancel); lv_label_set_text(lbl_cancel, "Cancel");
    lv_obj_center(lbl_cancel);

    /* The numeric keyboard. */
    s_kp_kbd = lv_keyboard_create(s_kp_root);
    lv_keyboard_set_mode(s_kp_kbd, LV_KEYBOARD_MODE_NUMBER);
    lv_keyboard_set_textarea(s_kp_kbd, s_kp_ta);
    lv_obj_set_width(s_kp_kbd, lv_pct(95));
    lv_obj_set_height(s_kp_kbd, 220);
    lv_obj_add_event_cb(s_kp_kbd, on_kbd_event, LV_EVENT_READY,  NULL);
    lv_obj_add_event_cb(s_kp_kbd, on_kbd_event, LV_EVENT_CANCEL, NULL);
}

/* ============================================================================
 *  Event handlers
 * ==========================================================================*/

static void on_close_click(lv_event_t *e)
{
    (void)e;
    boost_config_ui_close();
}

static void on_unit_click(lv_event_t *e)
{
    uintptr_t u = (uintptr_t)lv_event_get_user_data(e);
    if (u > OPENDASH_PRESSURE_PSI) return;
    g_boost_pressure_unit = (opendash_pressure_unit_t)u;
    system_config_save_pressure_unit();
    refresh_unit_buttons();
    refresh_cells();
}

static void on_mode_click(lv_event_t *e)
{
    uintptr_t mode = (uintptr_t)lv_event_get_user_data(e);
    s_active_mode = (opendash_boost_mode_t)mode;
    boost_client_set_mode(s_active_mode);
    refresh_mode_buttons();
    ESP_LOGI(TAG, "Mode → %s", mode_label(s_active_mode));
}

static void on_slot_click(lv_event_t *e)
{
    s_edit_slot = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    load_buffers_from_client();
    refresh_slot_buttons();
    refresh_cells();
}

static void on_gear_click(lv_event_t *e)
{
    s_edit_gear = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    load_buffers_from_client();
    refresh_gear_buttons();
    refresh_cells();
}

static void on_duty_cell_click(lv_event_t *e)
{
    uintptr_t ui_idx = (uintptr_t)lv_event_get_user_data(e);
    int idx = (int)s_page * OPENDASH_BOOST_UI_PAGE_SIZE + (int)ui_idx;
    if (idx >= OPENDASH_BOOST_MAP_POINTS) return;
    keypad_open(KP_KIND_DUTY, idx);
}

static void on_setp_cell_click(lv_event_t *e)
{
    uintptr_t ui_idx = (uintptr_t)lv_event_get_user_data(e);
    int idx = (int)s_page * OPENDASH_BOOST_UI_PAGE_SIZE + (int)ui_idx;
    if (idx >= OPENDASH_BOOST_MAP_POINTS) return;
    keypad_open(KP_KIND_SETP, idx);
}

static void on_page_click(lv_event_t *e)
{
    uint8_t p = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    if (p >= OPENDASH_BOOST_UI_PAGES) return;
    s_page = p;
    refresh_page_buttons();
    refresh_rpm_header();
    refresh_cells();
}

static void on_pull_click(lv_event_t *e)
{
    (void)e;
    boost_client_request_pull_all();
}

static void on_target_click(lv_event_t *e)
{
    (void)e;
    g_boost_target_node = (g_boost_target_node == OPENDASH_NODE_MOS_4CH_A)
                         ? OPENDASH_NODE_MOS_4CH_B : OPENDASH_NODE_MOS_4CH_A;
    system_config_save_boost_target();
    if (s_lbl_target) lv_label_set_text_fmt(s_lbl_target, "Target: %s", node_label(g_boost_target_node));
    boost_client_request_pull_all();
}

/* ============================================================================
 *  UI assembly helpers
 * ==========================================================================*/

static void tick_timer_cb(lv_timer_t *t)
{
    (void)t;
    boost_config_ui_tick();
}

static lv_obj_t *make_row(lv_obj_t *parent, const char *title)
{
    esp_task_wdt_reset();
    ESP_LOGI(TAG, "make_row: title=%s parent=%p", title ? title : "(none)", parent);
    lv_obj_t *row = lv_obj_create(parent);
    ESP_LOGI(TAG, "make_row: row=%p created", row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(row, 3, 0);
    lv_obj_set_style_pad_column(row, 4, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    if (title && *title) {
        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, title);
        lv_obj_set_width(lbl, 96);
    }
    return row;
}

static int s_btn_counter = 0;
static lv_obj_t *add_button(lv_obj_t *parent, const char *txt, lv_event_cb_t cb, void *ud)
{
    esp_task_wdt_reset();
    int n = ++s_btn_counter;
    ESP_LOGI(TAG, "add_button #%d txt=%s parent=%p free=%u int=%u", n, txt, parent,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    lv_obj_t *btn = lv_button_create(parent);
    ESP_LOGI(TAG, "add_button #%d btn=%p", n, btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, txt);
    lv_obj_center(lbl);
    ESP_LOGI(TAG, "add_button #%d done", n);
    return btn;
}

/* Build one row of cells (rpm header / duty / setp). */
static lv_obj_t *make_grid_row(lv_obj_t *parent, const char *title,
                                bool clickable, lv_event_cb_t cb,
                                lv_obj_t **out_cells)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 38);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(row, 1, 0);
    lv_obj_set_style_pad_column(row, 2, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_label_create(row);
    lv_label_set_text(hdr, title);
    lv_obj_set_width(hdr, 56);

    for (int i = 0; i < OPENDASH_BOOST_UI_PAGE_SIZE; ++i) {
        /* Feed the watchdog on every cell — creating many widgets in one tick
         * with theme styling can easily blow the 5 s WDT on a populated tree. */
        esp_task_wdt_reset();
        /* DIAG/WORKAROUND: All rows now use plain labels (no button wrap).
         * Button widgets exhaust the 218 KB internal-RAM pool well before the
         * 4+ MB PSRAM heap shows any pressure. Cells will be made tappable via
         * a single row-level event with hit-testing once the screen renders. */
        (void)clickable;
        (void)cb;
        lv_obj_t *lbl = lv_label_create(row);
        if (!lbl) {
            ESP_LOGE(TAG, "make_grid_row: label create failed (i=%d) int=%u", i,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            out_cells[i] = NULL;
            continue;
        }
        lv_obj_set_size(lbl, 42, 34);
        lv_obj_set_style_pad_all(lbl, 0, 0);
        lv_obj_set_style_bg_opa(lbl, LV_OPA_20, 0);
        lv_obj_set_style_border_width(lbl, 1, 0);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(lbl, "-");
        out_cells[i] = lbl;
    }
    return row;
}

/* ============================================================================
 *  Build / show / hide
 * ==========================================================================*/

lv_obj_t *boost_config_ui_create(lv_obj_t *parent)
{
    if (!parent) parent = lv_scr_act();
    if (s_root) {
        lv_obj_del(s_root);
        s_root = NULL;
    }
    /* Reset every cached child pointer NOW, before any new widget is built,
     * so a stray tick or callback can't see stale handles to freed widgets. */
    keypad_close();
    if (s_tick_timer) {
        lv_timer_del(s_tick_timer);
        s_tick_timer = NULL;
    }
    s_lbl_actual = s_lbl_setp = s_lbl_duty = s_lbl_flags = NULL;
    s_lbl_target = s_lbl_link = NULL;
    for (int i = 0; i < 3; ++i) { s_btn_unit[i] = s_btn_mode[i] = NULL; }
    for (int i = 0; i < 4; ++i) s_btn_chan[i] = NULL;
    for (int i = 0; i < 2; ++i) s_btn_slot[i] = NULL;
    for (int i = 0; i < OPENDASH_BOOST_GEARS; ++i) s_btn_gear[i] = NULL;
    for (int i = 0; i < OPENDASH_BOOST_UI_PAGE_SIZE; ++i) {
        s_cell_rpm[i] = s_cell_duty[i] = s_cell_setp[i] = NULL;
    }

    s_btn_counter = 0;
    size_t heap_before = esp_get_free_heap_size();
    ESP_LOGI(TAG, "boost_config_ui_create: free heap=%u parent=%p act_scr=%p", (unsigned)heap_before, parent, lv_scr_act());

    s_root = lv_obj_create(parent);
    ESP_LOGI(TAG, "boost_config_ui_create: s_root=%p", s_root);
    lv_obj_set_size(s_root, lv_pct(100), lv_pct(100));
    lv_obj_set_layout(s_root, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_root, 4, 0);
    lv_obj_set_style_pad_row(s_root, 2, 0);
    /* Don't let gestures from inside the editor reach the screen handler. */
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_GESTURE_BUBBLE);
    /* NOTE: scroll/clip styling deliberately deferred to end-of-build —
     * applying scrollbar mode + clip-corner on s_root before children are
     * added has been observed to interact badly with flex layout + many
     * descendants and wedge the UI task. */
    esp_task_wdt_reset();

    /* ── Header: title + close ── */
    {
        lv_obj_t *row = make_row(s_root, NULL);
        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, "BOOST CONFIG");
        lv_obj_set_flex_grow(lbl, 1);
        add_button(row, "Close", on_close_click, NULL);
    }

    /* ── Units + target node ── */
    {
        lv_obj_t *row = make_row(s_root, "Units:");
        s_btn_unit[0] = add_button(row, "kPa", on_unit_click, (void *)(uintptr_t)OPENDASH_PRESSURE_KPA);
        s_btn_unit[1] = add_button(row, "BAR", on_unit_click, (void *)(uintptr_t)OPENDASH_PRESSURE_BAR);
        s_btn_unit[2] = add_button(row, "PSI", on_unit_click, (void *)(uintptr_t)OPENDASH_PRESSURE_PSI);

        lv_obj_t *spacer = lv_obj_create(row);
        lv_obj_set_size(spacer, 20, 1);
        lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(spacer, 0, 0);
        lv_obj_remove_flag(spacer, LV_OBJ_FLAG_SCROLLABLE);

        s_lbl_target = lv_label_create(row);
        lv_label_set_text_fmt(s_lbl_target, "Target: %s", node_label(g_boost_target_node));
        lv_obj_set_flex_grow(s_lbl_target, 1);
        add_button(row, "Switch",  on_target_click, NULL);
        add_button(row, "Re-pull", on_pull_click,   NULL);
    }

    /* ── Mode picker ── */
    {
        lv_obj_t *row = make_row(s_root, "Mode:");
        s_btn_mode[0] = add_button(row, "OFF",    on_mode_click, (void *)(uintptr_t)OPENDASH_BOOST_MODE_OFF);
        s_btn_mode[1] = add_button(row, "NORMAL", on_mode_click, (void *)(uintptr_t)OPENDASH_BOOST_MODE_NORMAL);
        s_btn_mode[2] = add_button(row, "RACE",   on_mode_click, (void *)(uintptr_t)OPENDASH_BOOST_MODE_RACE);
    }

    /* ── Output channel picker (CH1..CH4 on the target MOS) ── */
    {
        lv_obj_t *row = make_row(s_root, "Chan:");
        s_btn_chan[0] = add_button(row, "CH1", on_chan_click, (void *)(intptr_t)0);
        s_btn_chan[1] = add_button(row, "CH2", on_chan_click, (void *)(intptr_t)1);
        s_btn_chan[2] = add_button(row, "CH3", on_chan_click, (void *)(intptr_t)2);
        s_btn_chan[3] = add_button(row, "CH4", on_chan_click, (void *)(intptr_t)3);
    }

    /* ── Link / limp banner (hidden until stale) ── */
    {
        s_lbl_link = lv_label_create(s_root);
        lv_label_set_text(s_lbl_link, "UNSTABLE LINK — LIMP MODE (slave data stale)");
        lv_obj_set_style_text_color(s_lbl_link, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_color(s_lbl_link, lv_color_hex(0xD32F2F), 0);
        lv_obj_set_style_bg_opa(s_lbl_link, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(s_lbl_link, 4, 0);
        lv_obj_set_width(s_lbl_link, lv_pct(100));
        lv_obj_add_flag(s_lbl_link, LV_OBJ_FLAG_HIDDEN);
    }

    /* ── Live readout ── */
    {
        lv_obj_t *row = make_row(s_root, "Live:");
        s_lbl_setp   = lv_label_create(row); lv_label_set_text(s_lbl_setp,   "set --");
        s_lbl_actual = lv_label_create(row); lv_label_set_text(s_lbl_actual, "act --");
        s_lbl_duty   = lv_label_create(row); lv_label_set_text(s_lbl_duty,   "duty --");
        s_lbl_flags  = lv_label_create(row); lv_label_set_text(s_lbl_flags,  "flags --");
    }

    /* ── Slot + gear pickers (combined row) ── */
    {
        lv_obj_t *row = make_row(s_root, "Edit:");
        s_btn_slot[0] = add_button(row, "NORMAL", on_slot_click, (void *)(uintptr_t)OPENDASH_BOOST_MODE_NORMAL);
        s_btn_slot[1] = add_button(row, "RACE",   on_slot_click, (void *)(uintptr_t)OPENDASH_BOOST_MODE_RACE);

        lv_obj_t *spacer = lv_obj_create(row);
        lv_obj_set_size(spacer, 12, 1);
        lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(spacer, 0, 0);
        lv_obj_remove_flag(spacer, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *gear_lbl = lv_label_create(row);
        lv_label_set_text(gear_lbl, "Gear:");

        for (int g = 0; g < OPENDASH_BOOST_GEARS; ++g) {
            char txt[4]; snprintf(txt, sizeof(txt), "%d", g + 1);
            s_btn_gear[g] = add_button(row, txt, on_gear_click, (void *)(uintptr_t)g);
        }
    }

    /* ── Page picker (visible only when MAP_POINTS doesn't fit in one row) ── */
    {
        lv_obj_t *row = make_row(s_root, "Page:");
        for (int p = 0; p < OPENDASH_BOOST_UI_PAGES; ++p) {
            char txt[12]; snprintf(txt, sizeof(txt), "%d (%dk-%dk)", p + 1,
                                   (p * OPENDASH_BOOST_UI_PAGE_SIZE * (OPENDASH_BOOST_RPM_MAX / (OPENDASH_BOOST_MAP_POINTS - 1))) / 1000,
                                   (((p + 1) * OPENDASH_BOOST_UI_PAGE_SIZE - 1) * (OPENDASH_BOOST_RPM_MAX / (OPENDASH_BOOST_MAP_POINTS - 1))) / 1000);
            s_btn_page[p] = add_button(row, txt, on_page_click, (void *)(uintptr_t)p);
        }
    }

    /* ── Cell grid: RPM / DUTY / SETP rows ── */
    {
        lv_obj_t *grid = lv_obj_create(s_root);
        lv_obj_set_width(grid, lv_pct(100));
        lv_obj_set_height(grid, LV_SIZE_CONTENT);
        lv_obj_set_layout(grid, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(grid, 2, 0);
        lv_obj_set_style_pad_row(grid, 2, 0);
        lv_obj_set_style_border_width(grid, 1, 0);
        lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

        ESP_LOGI(TAG, "heap check BEFORE grid: %s", heap_caps_check_integrity_all(true) ? "OK" : "CORRUPT");
        make_grid_row(grid, "RPM",  false, NULL,                s_cell_rpm);
        ESP_LOGI(TAG, "heap check after RPM row: %s", heap_caps_check_integrity_all(true) ? "OK" : "CORRUPT");
        make_grid_row(grid, "DUTY", true,  on_duty_cell_click,  s_cell_duty);
        ESP_LOGI(TAG, "heap check after DUTY row: %s", heap_caps_check_integrity_all(true) ? "OK" : "CORRUPT");
        make_grid_row(grid, "SETP", true,  on_setp_cell_click,  s_cell_setp);
        ESP_LOGI(TAG, "heap check after SETP row: %s", heap_caps_check_integrity_all(true) ? "OK" : "CORRUPT");
    }

    /* ── Footer ── Pull-from-slave button. */
    {
        ESP_LOGI(TAG, "section: FOOTER start, free=%u", (unsigned)esp_get_free_heap_size());
        lv_obj_t *row = make_row(s_root, NULL);
        add_button(row, "Pull from slave", on_pull_click, NULL);
        ESP_LOGI(TAG, "section: FOOTER done, free=%u", (unsigned)esp_get_free_heap_size());
    }

    /* RPM header values for the visible page are filled by refresh_rpm_header. */
    refresh_rpm_header();

    refresh_unit_buttons();
    refresh_mode_buttons();
    refresh_chan_buttons();
    refresh_slot_buttons();
    refresh_gear_buttons();
    refresh_page_buttons();
    load_buffers_from_client();
    refresh_cells();

    /* Apply scroll/clip props AFTER all children are built, so a layout
     * pass doesn't fight the still-growing flex tree. */
    lv_obj_set_style_clip_corner(s_root, true, 0);
    lv_obj_set_scroll_dir(s_root, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_root, LV_SCROLLBAR_MODE_AUTO);
    esp_task_wdt_reset();

    /* Self-ticking timer (200 ms) for live readout + limp banner. */
    if (!s_tick_timer) {
        s_tick_timer = lv_timer_create(tick_timer_cb, 200, NULL);
    }

    return s_root;
}

void boost_config_ui_show(void)
{
    if (!s_root) boost_config_ui_create(NULL);
    if (s_root) lv_obj_clear_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    boost_client_request_pull_all();
}

void boost_config_ui_close(void)
{
    keypad_close();
    if (s_tick_timer) {
        lv_timer_del(s_tick_timer);
        s_tick_timer = NULL;
    }
    if (s_root) {
        lv_obj_del(s_root);
        s_root = NULL;
    }
    /* Reset cached object pointers so we never dereference a stale handle. */
    s_lbl_actual = s_lbl_setp = s_lbl_duty = s_lbl_flags = NULL;
    s_lbl_target = s_lbl_link = NULL;
    for (int i = 0; i < 3; ++i) { s_btn_unit[i] = s_btn_mode[i] = NULL; }
    for (int i = 0; i < 4; ++i) s_btn_chan[i] = NULL;
    for (int i = 0; i < 2; ++i) s_btn_slot[i] = NULL;
    for (int i = 0; i < OPENDASH_BOOST_GEARS; ++i) s_btn_gear[i] = NULL;
    for (int i = 0; i < OPENDASH_BOOST_UI_PAGES; ++i) s_btn_page[i] = NULL;
    for (int i = 0; i < OPENDASH_BOOST_UI_PAGE_SIZE; ++i) {
        s_cell_rpm[i] = s_cell_duty[i] = s_cell_setp[i] = NULL;
    }

    if (s_close_cb) {
        s_close_cb();
    }
}

void boost_config_ui_set_close_cb(void (*cb)(void))
{
    s_close_cb = cb;
}

bool boost_config_ui_is_active(void)
{
    return s_root != NULL && !lv_obj_has_flag(s_root, LV_OBJ_FLAG_HIDDEN);
}

void boost_config_ui_tick(void)
{
    if (!boost_config_ui_is_active()) return;

    opendash_boost_telemetry_t t;
    memset(&t, 0, sizeof(t));
    boost_client_get_telemetry(&t);

    int dec = unit_decimals(g_boost_pressure_unit);
    const char *suf = unit_suffix(g_boost_pressure_unit);
    if (!suf) suf = "?";

    /* Use libc snprintf (newlib) + lv_label_set_text to bypass LVGL's
     * builtin vsnprintf entirely. LVGL's _strnlen_s crashes on NULL %s,
     * and we'd rather format defensively in our own stack buffer. */
    char buf[48];

    if (s_lbl_setp) {
        float v = cbar_to_unit((int)t.setpoint_cbar, g_boost_pressure_unit);
        snprintf(buf, sizeof(buf), "set %.*f %s", dec, (double)v, suf);
        lv_label_set_text(s_lbl_setp, buf);
    }
    if (s_lbl_actual) {
        float v = cbar_to_unit((int)t.boost_cbar, g_boost_pressure_unit);
        snprintf(buf, sizeof(buf), "act %.*f %s", dec, (double)v, suf);
        lv_label_set_text(s_lbl_actual, buf);
    }
    if (s_lbl_duty) {
        snprintf(buf, sizeof(buf), "duty %u/255", (unsigned)t.duty);
        lv_label_set_text(s_lbl_duty, buf);
    }
    if (s_lbl_flags) {
        snprintf(buf, sizeof(buf), "flags 0x%02X", (unsigned)t.safety_flags);
        lv_label_set_text(s_lbl_flags, buf);
    }

    /* Limp / unstable-link banner. We treat any of the following as "limp":
     *   - slave has explicitly raised DATA_STALE
     *   - we haven't received telemetry in > 800 ms (link blackout)
     *   - we have never received telemetry (slave offline)
     *
     * Hysteresis: a transient ESP-NOW hiccup must NOT flash the scary banner.
     * The fault has to persist ~1.2 s before we surface it; it clears the
     * instant the link recovers. The hard safety cut on the slave (600 ms) is
     * unaffected — this only governs what the end-user sees. */
    uint32_t age = boost_client_telemetry_age_ms();
    bool stale = (t.safety_flags & OPENDASH_BOOST_SAFE_DATA_STALE) != 0;
    bool link_dead = (age == UINT32_MAX) || (age > 800);
    bool fault = stale || link_dead;

    static uint32_t s_limp_since_ms = 0;   /* lv_tick when fault first seen, 0 = healthy */
    const uint32_t LIMP_SUSTAIN_MS = 1200;
    uint32_t now_ms = lv_tick_get();
    if (fault) {
        if (s_limp_since_ms == 0) s_limp_since_ms = now_ms ? now_ms : 1;
    } else {
        s_limp_since_ms = 0;   /* recovered — drop the banner immediately */
    }
    bool show_banner = (s_limp_since_ms != 0) &&
                       (lv_tick_elaps(s_limp_since_ms) >= LIMP_SUSTAIN_MS);

    if (s_lbl_link) {
        if (show_banner) {
            const char *why = link_dead
                ? (age == UINT32_MAX ? "no telemetry from slave"
                                      : "telemetry RX gap")
                : "slave reports stale engine data";
            lv_label_set_text_fmt(s_lbl_link,
                                  "UNSTABLE LINK — LIMP MODE (%s)", why);
            lv_obj_clear_flag(s_lbl_link, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_lbl_link, LV_OBJ_FLAG_HIDDEN);
        }
    }

    refresh_chan_buttons();   /* reflect live boost mask + safety reservations */
}
