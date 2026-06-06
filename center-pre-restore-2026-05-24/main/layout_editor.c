/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file layout_editor.c
 * @brief Per-mode screen layout editor (LVGL overlay) for the Center.
 *
 * Lightweight rewrite using `lv_dropdown` for every DP picker. Each
 * dropdown is a single LVGL widget — LVGL renders the option list
 * internally as a flat text blob, which is dramatically cheaper than
 * spawning 100+ button widgets per picker. This eliminates the IDLE1
 * starvation that triggered ui_task watchdog when opening the editor.
 *
 *   ┌────────────────────────────────────────────────────────────────┐
 *   │  LAYOUT EDITOR — <MODE NAME>                       [← BACK]   │
 *   ├────────────────────────────────────────────────────────────────┤
 *   │  Mode:  [ENGINE] [ GPS ] [  MD  ] [RELAY] [BMS] [OBD] [CFG]   │
 *   │                                                                │
 *   │  ARC:   [ DP_RPM            ▼]   min: [ 0  ]  max: [8000]     │
 *   │                                                                │
 *   │  Slot 0: [ DP_…  ▼]   Slot 3: [ DP_…  ▼]                      │
 *   │  Slot 1: [ DP_…  ▼]   Slot 4: [ DP_…  ▼]                      │
 *   │  Slot 2: [ DP_…  ▼]   Slot 5: [ DP_…  ▼]                      │
 *   │                                                                │
 *   │            [ RESET DRAFT ]    [ SAVE & APPLY ]                 │
 *   └────────────────────────────────────────────────────────────────┘
 */

#include "layout_editor.h"
#include "ui_manager.h"
#include "opendash_dp_catalog.h"
#include "opendash_layout.h"
#include "opendash_fonts.h"
#include "opendash_ui_styles.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LCD_W 800
#define LCD_H 480

static const char *TAG = "layout_editor";

/* ── Module state ───────────────────────────────────────────────────── */
static lv_obj_t *s_root         = NULL;   /* fullscreen overlay container  */

static screen_layout_v1_t s_edit_layout;
static uint8_t            s_edit_mode = 0;

/* Picker buttons (replace the old broken dropdowns). Each button shows
 * the *currently selected* DP name; tapping opens a fullscreen lv_list
 * picker. We track the embedded label separately so we can update its
 * text without re-finding the child. Slot index -1 means "arc target". */
static lv_obj_t *s_arc_btn       = NULL;
static lv_obj_t *s_arc_btn_lbl   = NULL;
static lv_obj_t *s_slot_btn[6]   = {0};
static lv_obj_t *s_slot_btn_lbl[6] = {0};

static lv_obj_t *s_arc_min_ta    = NULL;
static lv_obj_t *s_arc_max_ta    = NULL;
static lv_obj_t *s_status_label  = NULL;
static lv_obj_t *s_title_label   = NULL;
/* On-screen numeric keyboard, shown when a textarea is tapped.
 * Built lazily on first focus, hidden otherwise so it doesn't eat
 * the editor screen. */
static lv_obj_t *s_kb            = NULL;

/* DP picker modal — created on demand, destroyed on close. */
static lv_obj_t *s_picker_modal  = NULL;
static int8_t    s_picker_target = -2;  /* -1=arc, 0..5=slot, -2=invalid */
static uint16_t  s_picker_page   = 0;   /* 0-based page index */
#define PICKER_PAGE_SIZE 8              /* entries per page; 8 × 44px fits without scroll */

/* Parallel index→dp_id table built once per editor open. Replaces the
 * old s_dd_options string (which fed the broken lv_dropdown overlay). */
static uint16_t *s_dd_dp_ids     = NULL;   /* index 0 = 0 (none) */
static size_t    s_dd_count      = 0;       /* total entries incl. "(none)" */

/* Forward decls */
static void rebuild_editor_for_mode(uint8_t mode);
static void refresh_picker_labels(void);
static void build_dp_id_table(void);
static void free_dp_id_table(void);
static const char *dp_display_name(uint16_t dp_id);
static void open_picker_modal(int8_t target);
static void close_picker_modal(void);
static void open_picker_modal_async(void *user_data);
/* ── Helpers ────────────────────────────────────────────────────────── */
static const char *mode_name(uint8_t mode)
{
    static const char *names[] = {
        "ENGINE", "GPS", "MD", "RELAY", "BMS", "OBD", "CONFIG",
    };
    return (mode < (sizeof(names) / sizeof(names[0]))) ? names[mode] : "?";
}

static void set_status(const char *msg, uint32_t color)
{
    if (s_status_label) {
        lv_label_set_text(s_status_label, msg);
        lv_obj_set_style_text_color(s_status_label, lv_color_hex(color), 0);
    }
}

/* ── DP id table ───────────────────────────────────────────────────── *
 *
 * REWRITE C (no more lv_dropdown):
 *
 * The previous implementation built a 4 KB newline-separated options
 * blob and fed it to seven lv_dropdown widgets. Tapping any dropdown
 * triggered LVGL's overlay-list draw path, which for a 136-entry list
 * with the LARGE-font config crashed inside lv_draw_sw_fill →
 * lv_memset (StoreProhibited on Core 1) — verified with addr2line on
 * the captured backtrace. The dropdown widget's flat-text overlay is
 * fundamentally not viable for a list this large on this hardware.
 *
 * The new picker uses lv_list (one button per visible row) inside a
 * fullscreen modal. lv_list renders only the buttons currently in the
 * scroll viewport, sidesteps the broken dropdown draw path entirely,
 * and avoids the giant intermediate text blob. Memory use drops from
 * ~4 KB blob + 7×overlay-state to a single transient modal that is
 * created on tap and destroyed on selection.
 */
static void free_dp_id_table(void)
{
    if (s_dd_dp_ids)  { free(s_dd_dp_ids);  s_dd_dp_ids  = NULL; }
    s_dd_count = 0;
}

static void build_dp_id_table(void)
{
    free_dp_id_table();
    s_dd_count = opendash_dp_catalog_count + 1;  /* +1 for "(none)" */

    s_dd_dp_ids  = (uint16_t *)malloc(s_dd_count * sizeof(uint16_t));
    if (!s_dd_dp_ids) {
        ESP_LOGE(TAG, "OOM building dp id table");
        free_dp_id_table();
        return;
    }

    /* Index 0 reserved for "(none)" (dp_id 0). Catalog entries follow. */
    s_dd_dp_ids[0] = 0;
    for (size_t i = 0; i < opendash_dp_catalog_count; i++) {
        s_dd_dp_ids[i + 1] = opendash_dp_catalog[i].dp_id;
    }
}

/**
 * @brief Display name for a dp_id, formatted as "SHORTNAME (units)".
 *
 * Returned pointer is to a per-call static buffer; not thread-safe and
 * caller must copy if persistence needed. ui_task is the only caller.
 */
static const char *dp_display_name(uint16_t dp_id)
{
    static char buf[40];
    if (dp_id == 0) return "(none)";
    for (size_t i = 0; i < opendash_dp_catalog_count; i++) {
        const opendash_dp_info_t *e = &opendash_dp_catalog[i];
        if (e->dp_id == dp_id) {
            const char *u = (e->units && e->units[0]) ? e->units : "-";
            snprintf(buf, sizeof(buf), "%s (%s)", e->short_name, u);
            return buf;
        }
    }
    snprintf(buf, sizeof(buf), "DP_0x%04X", dp_id);
    return buf;
}

/* ── Mode selector ──────────────────────────────────────────────────── */
static void mode_chip_cb(lv_event_t *e)
{
    uint8_t mode = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    rebuild_editor_for_mode(mode);
}

/* ── Save / Reset / Back ────────────────────────────────────────────── */
static void save_btn_cb(lv_event_t *e)
{
    (void)e;
    /* Slot/arc selections live directly in s_edit_layout (updated by
     * picker_row_cb when the user taps a row). Only the textareas need
     * to be pulled at save time. */
    if (s_arc_min_ta) s_edit_layout.arc_min = strtof(lv_textarea_get_text(s_arc_min_ta), NULL);
    if (s_arc_max_ta) s_edit_layout.arc_max = strtof(lv_textarea_get_text(s_arc_max_ta), NULL);
    s_edit_layout.version    = OPENDASH_LAYOUT_VERSION;
    s_edit_layout.mode       = s_edit_mode;
    s_edit_layout.slot_count = 6;

    esp_err_t err = ui_manager_apply_layout(s_edit_mode, &s_edit_layout);
    if (err == ESP_OK) {
        set_status("Saved & applied.", 0x4ECCA3);
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "Save failed: 0x%x", err);
        set_status(buf, 0xFF4444);
    }
}

static void reset_btn_cb(lv_event_t *e)
{
    (void)e;
    rebuild_editor_for_mode(s_edit_mode);
    set_status("Editor reset to current bindings.", 0xFFCC00);
}

static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    layout_editor_close();
}

/* ── Build / rebuild ────────────────────────────────────────────────── */
static void refresh_picker_labels(void)
{
    if (s_arc_btn_lbl) {
        lv_label_set_text(s_arc_btn_lbl, dp_display_name(s_edit_layout.arc_dp_id));
    }
    for (int i = 0; i < 6; i++) {
        if (s_slot_btn_lbl[i]) {
            lv_label_set_text(s_slot_btn_lbl[i], dp_display_name(s_edit_layout.slot_dp_ids[i]));
        }
    }
}

static void rebuild_editor_for_mode(uint8_t mode)
{
    s_edit_mode = mode;
    if (ui_manager_get_layout(mode, &s_edit_layout) != ESP_OK) {
        memset(&s_edit_layout, 0, sizeof(s_edit_layout));
    }
    if (s_title_label) {
        char tbuf[48];
        snprintf(tbuf, sizeof(tbuf), "LAYOUT EDITOR — %s", mode_name(mode));
        lv_label_set_text(s_title_label, tbuf);
    }
    if (s_arc_min_ta) {
        char b[16]; snprintf(b, sizeof(b), "%.0f", s_edit_layout.arc_min);
        lv_textarea_set_text(s_arc_min_ta, b);
    }
    if (s_arc_max_ta) {
        char b[16]; snprintf(b, sizeof(b), "%.0f", s_edit_layout.arc_max);
        lv_textarea_set_text(s_arc_max_ta, b);
    }
    refresh_picker_labels();
}

/* ── Dropdown construction helper ───────────────────────────────────── */
/* ── Picker primitives (lv_list-based, NOT lv_dropdown) ─────────────── */

/* lv_async_call requires a (void*)->void thunk; carry the int8_t target
 * through the user_data pointer (cast via intptr_t to satisfy strict
 * function-pointer typing under -Werror=cast-function-type). */
static void open_picker_modal_async(void *user_data)
{
    int8_t target = (int8_t)(intptr_t)user_data;
    open_picker_modal(target);
}

/**
 * @brief Tap callback for a picker button. Defers modal construction
 *        via lv_async_call so the touch event returns immediately
 *        (otherwise building the modal synchronously inside the touch
 *        event re-trips the ui_task watchdog — verified by addr2line
 *        on the prior crash backtrace pointing at lv_list_add_button).
 */
static void picker_btn_cb(lv_event_t *e)
{
    int8_t target = (int8_t)(intptr_t)lv_event_get_user_data(e);
    s_picker_target = target;
    s_picker_page   = 0;
    lv_async_call(open_picker_modal_async, (void *)(intptr_t)target);
}

/**
 * @brief Build a picker button — a simple lv_btn that shows the current
 *        DP selection text and opens the modal picker on tap.
 *
 * @param parent       Editor root.
 * @param x,y,w        Position and width on the editor surface.
 * @param target       -1 for the arc target, 0..5 for slot index.
 * @param label_out    Receives the inner label pointer (so the caller
 *                     can refresh the displayed name without re-finding it).
 * @return The button object, also kept as s_arc_btn / s_slot_btn[i].
 */
static lv_obj_t *make_picker_btn(lv_obj_t *parent, int x, int y, int w,
                                  int8_t target, lv_obj_t **label_out)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, 38);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1E2A38), 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xFF6F00), 0);
    lv_obj_set_style_radius(btn, 4, 0);
    lv_obj_set_style_pad_left(btn, 10, 0);
    lv_obj_set_style_pad_right(btn, 10, 0);
    lv_obj_add_event_cb(btn, picker_btn_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)target);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "(none)");
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    opendash_set_font(lbl, OPENDASH_FONT_SIZE_MEDIUM);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_width(lbl, w - 20);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
    if (label_out) *label_out = lbl;

    return btn;
}

/**
 * @brief Picker row tap → write selection to draft layout, refresh the
 *        opener button's label, close the modal.
 *
 * The selected dp_id is encoded into the row button's user_data at row
 * creation time. The current target (arc vs which slot) is held in
 * s_picker_target while the modal is open.
 */
static void picker_row_cb(lv_event_t *e)
{
    uint16_t dp_id = (uint16_t)(uintptr_t)lv_event_get_user_data(e);

    if (s_picker_target == -1) {
        s_edit_layout.arc_dp_id = dp_id;
        /* Auto-load sensible default min/max from the catalog whenever
         * the user changes the ARC DP. The user can still override by
         * tapping the textarea and editing — this just removes the
         * "RPM 0..8000 stuck on a coolant gauge" footgun. The catalog
         * provides default_min/default_max for every entry. */
        const opendash_dp_info_t *info = opendash_dp_lookup(dp_id);
        if (info) {
            s_edit_layout.arc_min = info->default_min;
            s_edit_layout.arc_max = info->default_max;
            if (s_arc_min_ta) {
                char b[16]; snprintf(b, sizeof(b), "%.0f", info->default_min);
                lv_textarea_set_text(s_arc_min_ta, b);
            }
            if (s_arc_max_ta) {
                char b[16]; snprintf(b, sizeof(b), "%.0f", info->default_max);
                lv_textarea_set_text(s_arc_max_ta, b);
            }
        }
    } else if (s_picker_target >= 0 && s_picker_target < 6) {
        s_edit_layout.slot_dp_ids[s_picker_target] = dp_id;
    }

    refresh_picker_labels();
    close_picker_modal();
}

static void picker_close_cb(lv_event_t *e)
{
    (void)e;
    close_picker_modal();
}

/**
 * @brief Page-nav callback. user_data: +1 = next page, -1 = prev page.
 *        Rebuilds the modal with the new page slice.
 */
static void picker_page_cb(lv_event_t *e)
{
    int delta = (int)(intptr_t)lv_event_get_user_data(e);
    int8_t target = s_picker_target;
    uint16_t total_pages = (uint16_t)((s_dd_count + PICKER_PAGE_SIZE - 1) / PICKER_PAGE_SIZE);
    if (total_pages == 0) return;
    int new_page = (int)s_picker_page + delta;
    if (new_page < 0) new_page = 0;
    if (new_page >= total_pages) new_page = total_pages - 1;
    s_picker_page = (uint16_t)new_page;
    /* Tear down current modal and rebuild on next tick — same async
     * defer pattern that prevents ui_task watchdog trips. */
    close_picker_modal();
    s_picker_target = target;
    lv_async_call(open_picker_modal_async, (void *)(intptr_t)target);
}

/**
 * @brief Open the fullscreen DP picker modal.
 *
 * Replaces the broken lv_dropdown overlay with a paginated lv_list.
 * Each page holds at most PICKER_PAGE_SIZE rows so the synchronous
 * widget build stays well under one ui_task tick. Prev/Next buttons
 * page through the catalog without rebuilding the editor underneath.
 */
static void open_picker_modal(int8_t target)
{
    if (s_picker_modal) return;          /* already open */
    if (s_dd_count == 0) return;         /* no catalog */

    s_picker_target = target;

    /* Modal root: full screen, opaque dark, blocks input to editor below. */
    s_picker_modal = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_picker_modal);
    lv_obj_set_size(s_picker_modal, LCD_W, LCD_H);
    lv_obj_align(s_picker_modal, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_picker_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_picker_modal, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_picker_modal, LV_OBJ_FLAG_SCROLLABLE);

    /* Compute page bounds. */
    uint16_t total_pages = (uint16_t)((s_dd_count + PICKER_PAGE_SIZE - 1) / PICKER_PAGE_SIZE);
    if (s_picker_page >= total_pages) s_picker_page = 0;
    size_t start = (size_t)s_picker_page * PICKER_PAGE_SIZE;
    size_t end   = start + PICKER_PAGE_SIZE;
    if (end > s_dd_count) end = s_dd_count;

    /* Title — includes page indicator. */
    lv_obj_t *title = lv_label_create(s_picker_modal);
    char tbuf[64];
    if (target == -1) snprintf(tbuf, sizeof(tbuf), "SELECT ARC DP   (page %u/%u)",
                               (unsigned)(s_picker_page + 1), (unsigned)total_pages);
    else              snprintf(tbuf, sizeof(tbuf), "SELECT SLOT %d DP   (page %u/%u)",
                               target, (unsigned)(s_picker_page + 1), (unsigned)total_pages);
    lv_label_set_text(title, tbuf);
    opendash_set_font(title, OPENDASH_FONT_SIZE_MEDIUM);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFF6F00), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 15, 8);

    /* Cancel button (top right) */
    lv_obj_t *close_btn = lv_btn_create(s_picker_modal);
    lv_obj_set_size(close_btn, 110, 40);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, -10, 5);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0x664444), 0);
    lv_obj_add_event_cb(close_btn, picker_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(close_btn);
    lv_label_set_text(cl, "CANCEL");
    opendash_set_font(cl, OPENDASH_FONT_SIZE_MEDIUM);
    lv_obj_set_style_text_color(cl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(cl);

    /* Prev page (bottom left) */
    lv_obj_t *prev_btn = lv_btn_create(s_picker_modal);
    lv_obj_set_size(prev_btn, 130, 44);
    lv_obj_align(prev_btn, LV_ALIGN_BOTTOM_LEFT, 15, -10);
    lv_obj_set_style_bg_color(prev_btn, lv_color_hex(0x224466), 0);
    lv_obj_add_event_cb(prev_btn, picker_page_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    lv_obj_t *pl = lv_label_create(prev_btn);
    lv_label_set_text(pl, "\xe2\x97\x80 PREV");
    opendash_set_font(pl, OPENDASH_FONT_SIZE_MEDIUM);
    lv_obj_set_style_text_color(pl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(pl);

    /* Next page (bottom right) */
    lv_obj_t *next_btn = lv_btn_create(s_picker_modal);
    lv_obj_set_size(next_btn, 130, 44);
    lv_obj_align(next_btn, LV_ALIGN_BOTTOM_RIGHT, -15, -10);
    lv_obj_set_style_bg_color(next_btn, lv_color_hex(0x224466), 0);
    lv_obj_add_event_cb(next_btn, picker_page_cb, LV_EVENT_CLICKED, (void *)(intptr_t)+1);
    lv_obj_t *nl = lv_label_create(next_btn);
    lv_label_set_text(nl, "NEXT \xe2\x96\xb6");
    opendash_set_font(nl, OPENDASH_FONT_SIZE_MEDIUM);
    lv_obj_set_style_text_color(nl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(nl);

    /* Build this page's rows directly on the modal at fixed Y offsets.
     *
     * NOTE(addr2line-confirmed): lv_list_add_button calls
     * lv_label_set_long_mode(LV_LABEL_LONG_DOT) on its child label,
     * which under FONT_FMT_TXT_LARGE drops into an lv_anim_start path
     * inside lv_label_refr_text that hangs ui_task indefinitely.
     * We sidestep by building plain buttons + labels with LONG_CLIP
     * (no animation timer ever installed). 8 rows × 44px = 352px
     * which fits in the 480-110=370px vertical band, so no scroll
     * container is needed (which avoids the NULL-child-on-flex bug). */
    int row_w = LCD_W - 40;
    int row_h = 40;
    int row_top_y = 50;       /* below title */
    int row_gap = 4;
    int idx = 0;
    for (size_t i = start; i < end; i++) {
        uint16_t dp_id = s_dd_dp_ids[i];
        lv_obj_t *row = lv_btn_create(s_picker_modal);
        lv_obj_set_size(row, row_w, row_h);
        lv_obj_align(row, LV_ALIGN_TOP_LEFT, 20, row_top_y + idx * (row_h + row_gap));
        lv_obj_set_style_bg_color(row, lv_color_hex(0x1E2A38), 0);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_set_style_pad_all(row, 4, 0);
        lv_obj_add_event_cb(row, picker_row_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)dp_id);

        lv_obj_t *lbl = lv_label_create(row);
        /* CRITICAL: set long-mode BEFORE text. LONG_CLIP installs no
         * animation; LONG_DOT (the lv_list default) does and hangs. */
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
        lv_label_set_text(lbl, dp_display_name(dp_id));
        opendash_set_font(lbl, OPENDASH_FONT_SIZE_MEDIUM);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xF0F0F0), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);
        idx++;
    }
}

static void close_picker_modal(void)
{
    if (s_picker_modal) {
        lv_obj_del(s_picker_modal);
        s_picker_modal  = NULL;
        s_picker_target = -2;
    }
}

/* ── On-screen numeric keyboard ─────────────────────────────────────── */
/**
 * @brief Lazily build a numeric keyboard at the bottom of the screen.
 *
 * LVGL's lv_keyboard widget handles all key events and applies them to
 * its assigned textarea automatically. We use LV_KEYBOARD_MODE_NUMBER
 * because all editor textareas hold floats. The keyboard is hidden by
 * default and shown when a textarea gains focus.
 */
static void kb_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    /* Hide on Cancel/Apply so the user gets the editor back. */
    if (code == LV_EVENT_CANCEL || code == LV_EVENT_READY) {
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief Show the numeric keyboard bound to the focused textarea.
 *
 * Attached as LV_EVENT_FOCUSED + LV_EVENT_CLICKED to each textarea so
 * a tap brings up the keyboard even without focus management. Also
 * attached as LV_EVENT_DEFOCUSED to hide when focus moves away (e.g.
 * the user taps Save).
 */
static void ta_focus_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = lv_event_get_target_obj(e);
    if (!s_kb) return;
    if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
        lv_keyboard_set_textarea(s_kb, ta);
        lv_obj_clear_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
        /* Bring keyboard to front in case other widgets were added on top. */
        lv_obj_move_foreground(s_kb);
    }
}

/* ── Public API ─────────────────────────────────────────────────────── */
void layout_editor_open(lv_obj_t *parent)
{
    if (s_root) return;  /* already open */
    if (parent == NULL) parent = lv_scr_act();

    ESP_LOGI(TAG, "layout_editor_open: free heap=%u, largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));

    /* Build the DP id table (cheap; just an array of u16). */
    build_dp_id_table();

    s_root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, LCD_W, LCD_H);
    lv_obj_align(s_root, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    /* Title */
    s_title_label = lv_label_create(s_root);
    lv_label_set_text(s_title_label, "LAYOUT EDITOR");
    opendash_set_font(s_title_label, OPENDASH_FONT_SIZE_LARGE);
    lv_obj_set_style_text_color(s_title_label, lv_color_hex(0xFF6F00), 0);
    lv_obj_align(s_title_label, LV_ALIGN_TOP_LEFT, 15, 8);

    /* BACK */
    lv_obj_t *btn_back = lv_btn_create(s_root);
    lv_obj_set_size(btn_back, 110, 40);
    lv_obj_align(btn_back, LV_ALIGN_TOP_RIGHT, -10, 5);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x444444), 0);
    lv_obj_add_event_cb(btn_back, back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bk = lv_label_create(btn_back);
    lv_label_set_text(bk, "\xe2\x86\x90 BACK");
    opendash_set_font(bk, OPENDASH_FONT_SIZE_MEDIUM);
    lv_obj_set_style_text_color(bk, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(bk);

    vTaskDelay(1);  /* yield so IDLE1 can run */

    /* Mode chips */
    static const uint8_t mode_chips[] = {0, 1, 2, 3, 4, 5, 6};
    int chip_x = 15;
    int chip_y = 55;
    for (size_t i = 0; i < sizeof(mode_chips); i++) {
        lv_obj_t *chip = lv_btn_create(s_root);
        lv_obj_set_size(chip, 95, 36);
        lv_obj_set_pos(chip, chip_x, chip_y);
        lv_obj_set_style_bg_color(chip, lv_color_hex(0x222244), 0);
        lv_obj_set_style_radius(chip, 6, 0);
        lv_obj_add_event_cb(chip, mode_chip_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)mode_chips[i]);
        lv_obj_t *cl = lv_label_create(chip);
        lv_label_set_text(cl, mode_name(mode_chips[i]));
        opendash_set_font(cl, OPENDASH_FONT_SIZE_MEDIUM);
        lv_obj_set_style_text_color(cl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(cl);
        chip_x += 105;
    }
    vTaskDelay(1);

    /* Arc row */
    int arc_y = 105;
    lv_obj_t *arc_lbl = lv_label_create(s_root);
    lv_label_set_text(arc_lbl, "ARC:");
    opendash_set_font(arc_lbl, OPENDASH_FONT_SIZE_MEDIUM);
    lv_obj_set_style_text_color(arc_lbl, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_pos(arc_lbl, 15, arc_y + 8);

    s_arc_btn = make_picker_btn(s_root, 80, arc_y, 200, -1, &s_arc_btn_lbl);

    /* min text area */
    lv_obj_t *min_lbl = lv_label_create(s_root);
    lv_label_set_text(min_lbl, "min:");
    opendash_set_font(min_lbl, OPENDASH_FONT_SIZE_MEDIUM);
    lv_obj_set_style_text_color(min_lbl, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_pos(min_lbl, 295, arc_y + 8);

    s_arc_min_ta = lv_textarea_create(s_root);
    lv_textarea_set_one_line(s_arc_min_ta, true);
    lv_textarea_set_text(s_arc_min_ta, "0");
    lv_obj_set_size(s_arc_min_ta, 90, 36);
    lv_obj_set_pos(s_arc_min_ta, 345, arc_y);
    opendash_set_font(s_arc_min_ta, OPENDASH_FONT_SIZE_MEDIUM);

    lv_obj_t *max_lbl = lv_label_create(s_root);
    lv_label_set_text(max_lbl, "max:");
    opendash_set_font(max_lbl, OPENDASH_FONT_SIZE_MEDIUM);
    lv_obj_set_style_text_color(max_lbl, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_pos(max_lbl, 450, arc_y + 8);

    s_arc_max_ta = lv_textarea_create(s_root);
    lv_textarea_set_one_line(s_arc_max_ta, true);
    lv_textarea_set_text(s_arc_max_ta, "8000");
    lv_obj_set_size(s_arc_max_ta, 90, 36);
    lv_obj_set_pos(s_arc_max_ta, 500, arc_y);
    opendash_set_font(s_arc_max_ta, OPENDASH_FONT_SIZE_MEDIUM);

    /* Numeric on-screen keyboard. Parented on lv_scr_act() so it sits
     * above s_root and the editor's other widgets. Hidden until a
     * textarea is focused; ta_focus_cb shows it and binds the active
     * textarea. The keyboard's own CANCEL/APPLY emit kb_event_cb which
     * hides it again. */
    s_kb = lv_keyboard_create(lv_scr_act());
    lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_NUMBER);
    lv_obj_set_size(s_kb, LCD_W, 200);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_kb, kb_event_cb, LV_EVENT_ALL, NULL);

    /* Hook each textarea to bring up the keyboard on tap. Both FOCUSED
     * and CLICKED are wired so a tap works regardless of LVGL's input
     * focus mode (no group registered → only CLICKED fires). */
    lv_obj_add_event_cb(s_arc_min_ta, ta_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_arc_min_ta, ta_focus_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_arc_max_ta, ta_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_arc_max_ta, ta_focus_cb, LV_EVENT_CLICKED, NULL);

    vTaskDelay(1);

    /* Slot picker buttons — 2 columns × 3 rows. Each button opens the
     * fullscreen picker modal on tap (see open_picker_modal). */
    int slot_y0 = 160;
    for (int i = 0; i < 6; i++) {
        int col = i / 3;
        int row = i % 3;
        int x   = (col == 0) ? 80 : 470;
        int lx  = (col == 0) ? 15 : 405;
        int y   = slot_y0 + row * 50;

        char buf[16]; snprintf(buf, sizeof(buf), "Slot %d:", i);
        lv_obj_t *lbl = lv_label_create(s_root);
        lv_label_set_text(lbl, buf);
        opendash_set_font(lbl, OPENDASH_FONT_SIZE_MEDIUM);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xCCCCCC), 0);
        lv_obj_set_pos(lbl, lx, y + 8);

        s_slot_btn[i] = make_picker_btn(s_root, x, y, 280,
                                        (int8_t)i, &s_slot_btn_lbl[i]);
    }

    /* Action row */
    int action_y = 320;
    lv_obj_t *btn_reset = lv_btn_create(s_root);
    lv_obj_set_size(btn_reset, 200, 50);
    lv_obj_set_pos(btn_reset, 130, action_y);
    lv_obj_set_style_bg_color(btn_reset, lv_color_hex(0x664400), 0);
    lv_obj_add_event_cb(btn_reset, reset_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = lv_label_create(btn_reset);
    lv_label_set_text(rl, "RESET DRAFT");
    opendash_set_font(rl, OPENDASH_FONT_SIZE_MEDIUM);
    lv_obj_set_style_text_color(rl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(rl);

    ESP_LOGI(TAG, "before btn_save: free=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    lv_obj_t *btn_save = lv_btn_create(s_root);
    if (!btn_save) {
        ESP_LOGE(TAG, "btn_save alloc FAILED — bailing");
        return;
    }
    lv_obj_set_size(btn_save, 240, 50);
    lv_obj_set_pos(btn_save, 430, action_y);
    lv_obj_set_style_bg_color(btn_save, lv_color_hex(0x006633), 0);
    lv_obj_add_event_cb(btn_save, save_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(btn_save);
    lv_label_set_text(sl, "SAVE & APPLY");
    opendash_set_font(sl, OPENDASH_FONT_SIZE_MEDIUM);
    lv_obj_set_style_text_color(sl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(sl);

    /* Status line */
    s_status_label = lv_label_create(s_root);
    lv_label_set_text(s_status_label, "Ready.");
    opendash_set_font(s_status_label, OPENDASH_FONT_SIZE_MEDIUM);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_LEFT, 15, -15);

    /* Yield once more before rebuild_editor_for_mode — that call
     * just walks the DP id table to update label text on 7 buttons,
     * cheap, but the yield keeps IDLE1 happy for any slow path inside
     * ui_manager_get_layout. */
    vTaskDelay(pdMS_TO_TICKS(5));

    /* Default to ENGINE (or whatever is mode 0) */
    rebuild_editor_for_mode(0);
    ESP_LOGI(TAG, "Layout editor opened (mode=%u, %u DPs in catalog)",
             s_edit_mode, (unsigned)s_dd_count);
}

void layout_editor_close(void)
{
    /* If a picker modal is still up (rare — user tapped BACK without
     * closing the picker first), tear it down before the editor root
     * so its widgets don't outlive their parent. */
    close_picker_modal();

    if (s_root) {
        lv_obj_del(s_root);
        s_root = NULL;
    }
    /* Keyboard is parented on lv_scr_act, NOT s_root, so it survives
     * the editor root deletion above and we must clean it up explicitly. */
    if (s_kb) {
        lv_obj_del(s_kb);
        s_kb = NULL;
    }
    s_arc_btn        = NULL;
    s_arc_btn_lbl    = NULL;
    s_arc_min_ta    = NULL;
    s_arc_max_ta    = NULL;
    s_status_label  = NULL;
    s_title_label   = NULL;
    for (int i = 0; i < 6; i++) {
        s_slot_btn[i]     = NULL;
        s_slot_btn_lbl[i] = NULL;
    }

    /* Free the dropdown options string AFTER the widgets are deleted —
     * lv_dropdown_set_options_static() only stored a pointer to it. */
    free_dp_id_table();
    ESP_LOGI(TAG, "Layout editor closed");
}
