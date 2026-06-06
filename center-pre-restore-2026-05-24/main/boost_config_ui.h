/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file boost_config_ui.h
 * @brief LVGL System Config screen — boost editor section.
 */

#ifndef OPENDASH_CENTER_BOOST_CONFIG_UI_H
#define OPENDASH_CENTER_BOOST_CONFIG_UI_H

#include "lvgl.h"

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

#ifdef __cplusplus
}
#endif

#endif /* OPENDASH_CENTER_BOOST_CONFIG_UI_H */
