/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file boost_config_ui.h
 * @brief LVGL System Config screen — boost editor section.
 */

#ifndef OPENDASH_CENTER_BOOST_CONFIG_UI_H
#define OPENDASH_CENTER_BOOST_CONFIG_UI_H

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Build the System Config screen as a child of @p parent.
 *
 * Returns the root container so callers can switch to it / hide it.
 * Calling this multiple times rebuilds the screen.
 */
lv_obj_t *boost_config_ui_create(lv_obj_t *parent);

/**
 * @brief Show the System Config screen as a modal over the current screen.
 *
 * Convenience helper: pass NULL → uses lv_scr_act().
 */
void boost_config_ui_show(void);

/** @brief Tick — call from a periodic timer so live readouts refresh. */
void boost_config_ui_tick(void);

/**
 * @brief True while the editor is visible (modal).
 *
 * The screen-level gesture handler uses this to disable swipe-to-cycle so
 * the user can interact with cells/keyboard without flinging away the page
 * (which previously crashed on the deferred delete of editor children).
 */
bool boost_config_ui_is_active(void);

/** @brief Close the editor if it is open (no-op otherwise). */
void boost_config_ui_close(void);

/**
 * @brief Register a callback invoked after the editor closes.
 *
 * Lets the owning screen (System Config) un-hide whatever it hid when
 * opening the modal. Pass NULL to clear. Safe to call before or after
 * `boost_config_ui_create()`.
 */
void boost_config_ui_set_close_cb(void (*cb)(void));

#ifdef __cplusplus
}
#endif

#endif /* OPENDASH_CENTER_BOOST_CONFIG_UI_H */
