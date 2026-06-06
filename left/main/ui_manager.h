/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file ui_manager.h
 * @brief OpenDash Left/Right/Pod Gauges — UI Manager
 *
 * Full-featured gauge display manager with:
 * - Multi-page gauge cycling (boot button or I2C command)
 *   Up to GAUGE_PAGE_MAX pages, each with independent primary/secondary
 *   data point assignments, arc range, and min/max tracking.
 * - Warning boxes with flashing red/orange alerts
 * - Centered text labels with outlined rendering
 * - Thick arc gauge with symmetric white outline
 * - Unit-aware value display (°C/°F, kPa/BAR/PSI, km/mi, km/h/MPH)
 * - Session min/max tracking per gauge page
 * - Shift-light blink (RPM-triggered flash)
 * - Odometer & trip display (final screen)
 *
 * This header is shared by /left, /right, and /pod# projects.
 * The only difference between them is the node type passed at init.
 *
 * @see center/main/ui_manager.h for the center display equivalent.
 */

#ifndef UI_MANAGER_H
#define UI_MANAGER_H

#include "esp_err.h"
#include "opendash_display_config.h"
#include "opendash_odometer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Warning System ──────────────────────────────────────────────────────── */

typedef enum {
    OPENDASH_WARNING_NONE     = 0,  /**< No warning */
    OPENDASH_WARNING_CAUTION  = 1,  /**< Caution (orange flash) */
    OPENDASH_WARNING_CRITICAL = 2,  /**< Critical (red flash) */
} opendash_warning_level_t;

/* ── Gauge Page Configuration ────────────────────────────────────────────
 *
 * Each gauge page defines what the primary (arc) and secondary (box)
 * displays show.  The pod cycles through pages on boot-button press.
 * The last screen is always the Odometer/Trip page.
 *
 * Example page set (4 total):
 *   Page 0: Oil Pressure (arc) + Boost (box)
 *   Page 1: Water Temp   (arc) + Wheel MPH (box)
 *   Page 2: RPM          (arc) + AFR (box)        ← shift-light enabled
 *   Page 3: Odometer / Trip A / Trip B
 * ──────────────────────────────────────────────────────────────────────── */

#define GAUGE_PAGE_MAX  8   /**< Max configurable gauge pages (compile-time) */

/**
 * @brief Configuration for one gauge page.
 *
 * Defines what data appears on the arc (primary) and in the bottom
 * box (secondary), plus the arc's numeric range for percentage mapping.
 */
typedef struct {
    const char *primary_label;      /**< Arc value label (e.g. "OIL PRESS") */
    uint16_t    primary_dp;         /**< Data point ID for primary arc value */
    float       arc_min;            /**< Arc 0% value (in internal units) */
    float       arc_max;            /**< Arc 100% value (in internal units) */
    bool        is_primary_temp;    /**< true if primary uses temp conversion */
    bool        is_primary_pressure;/**< true if primary uses pressure conversion */

    const char *secondary_label;    /**< Box label (e.g. "BOOST") */
    uint16_t    secondary_dp;       /**< Data point ID for secondary box */
    bool        is_secondary_temp;  /**< true if secondary uses temp conversion */
    bool        is_secondary_pressure; /**< true if secondary uses pressure conversion */
    bool        is_secondary_speed; /**< true if secondary uses speed conversion */

    bool        shift_light;        /**< true = blink screen when arc > 90% */
} gauge_page_t;

/* ── Initialization ──────────────────────────────────────────────────────── */

esp_err_t ui_manager_init(const opendash_display_layout_t *layout);
esp_err_t ui_manager_start(void);

/**
 * @brief Suspend the LVGL UI task.
 *
 * Used by the BLE OTA entry path to stop LVGL refresh activity so the
 * NimBLE host task and OTA flash writes do not compete with LVGL for CPU
 * and SPI bus time. No matching resume — caller reboots after OTA.
 */
void ui_manager_suspend(void);

/* ── Data Updates ────────────────────────────────────────────────────────── */

void ui_manager_update_value(uint16_t data_point_id, float value);
void ui_manager_update_odometer(const opendash_odometer_t *odo);

/* ── Warning Boxes ───────────────────────────────────────────────────────── */

esp_err_t ui_manager_warning_trigger(opendash_warning_level_t level,
                                      const char *message,
                                      uint32_t flash_ms);
esp_err_t ui_manager_warning_clear(void);

/* ── Screen Navigation ───────────────────────────────────────────────────── */

esp_err_t ui_manager_next_screen(void);
uint8_t   ui_manager_get_current_screen(void);
uint8_t   ui_manager_get_screen_count(void);

/* ── Status Line (small text above primary label) ────────────────────────── */

void ui_manager_set_status_text(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* UI_MANAGER_H */
