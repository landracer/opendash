/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file layout_editor.h
 * @brief OpenDash Center — Per-Mode Screen Layout Editor (LVGL)
 *
 * Lightweight on-device editor for choosing which data points appear on
 * the center display in each `display_mode_t`. The editor lets the user:
 *
 *   1. Pick a target display mode (ENGINE / GPS / MD / ...).
 *   2. Pick the arc data point and its min/max range.
 *   3. Pick the data point bound to each of the 6 section slots.
 *   4. Save & Apply — writes through `ui_manager_apply_layout()`, which
 *      persists to NVS and propagates to the active screen.
 *
 * The editor is built as a fullscreen overlay on the active screen and
 * is destroyed when the user taps "← BACK". It does not touch the
 * underlying gauge UI in any way other than the apply step.
 *
 * This module owns no global state besides one container pointer; calling
 * `layout_editor_open()` while it is already open is a no-op.
 */

#ifndef LAYOUT_EDITOR_H
#define LAYOUT_EDITOR_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Open the layout editor as a fullscreen overlay on `parent`.
 *
 * Parent is typically `lv_scr_act()` or the active screen container so
 * the editor draws on top of whatever is currently shown.
 *
 * @param[in] parent  LVGL parent (usually the active screen).
 */
void layout_editor_open(lv_obj_t *parent);

/**
 * @brief Close the editor and free its widgets.
 *
 * Safe to call when the editor is not open (no-op).
 */
void layout_editor_close(void);

#ifdef __cplusplus
}
#endif

#endif /* LAYOUT_EDITOR_H */
