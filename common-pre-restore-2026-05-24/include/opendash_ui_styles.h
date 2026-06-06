/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_ui_styles.h
 * @brief OpenDash UI Styling Configuration
 *
 * Centralized styling definitions for consistent look across all displays.
 * Includes colors, font outlines, positioning, and warning states.
 * 
 * TEXT OUTLINE IMPLEMENTATION:
 * LVGL doesn't support native text stroke/outline. We implement it by creating
 * multiple shadow labels at offset positions behind the main text, creating
 * the visual effect of an outline hugging each letter.
 */

#ifndef OPENDASH_UI_STYLES_H
#define OPENDASH_UI_STYLES_H

#include "lvgl.h"
#include "opendash_display_config.h"  /* For opendash_temp_unit_t, opendash_speed_unit_t */
#include "opendash_fonts.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ────────────────────────────────────────────────────────────────────────────
 * Color Definitions
 * ──────────────────────────────────────────────────────────────────────────── */

/* Primary font colors */
#define OPENDASH_COLOR_TEXT_PRIMARY     0xFFFFFF    /**< White - main text */
#define OPENDASH_COLOR_TEXT_SECONDARY   0xAAAAAA    /**< Light gray - labels */
#define OPENDASH_COLOR_TEXT_OUTLINE     0x000000    /**< Black - text outline */

/* Warning/Alert colors */
#define OPENDASH_COLOR_WARNING_TEXT     0xFF0000    /**< Red - warning text */
#define OPENDASH_COLOR_WARNING_OUTLINE  0xFFFFFF    /**< White - warning outline */
#define OPENDASH_COLOR_OK               0x00FF00    /**< Green - system ok */
#define OPENDASH_COLOR_CAUTION          0xFFAA00    /**< Orange - caution state */

/* Warning Box colors (hard solid, no bleed-through) */
#define OPENDASH_COLOR_WARNING_BOX_RED      0xFF0000    /**< Bright red - critical warning */
#define OPENDASH_COLOR_WARNING_BOX_ORANGE   0xFF6600    /**< Orange - caution warning */
#define OPENDASH_COLOR_WARNING_BOX_BG       0x000000    /**< Black background for boxes */

/* Background colors */
#define OPENDASH_COLOR_BG_DARK          0x000000    /**< Black background */
#define OPENDASH_COLOR_BG_SECTION       0x101010    /**< Dark gray - section bg */
#define OPENDASH_COLOR_BG_STATUSBAR     0x181818    /**< Status bar background */
#define OPENDASH_COLOR_BORDER           0xFFFFFF    /**< Section border - white */
#define OPENDASH_COLOR_BORDER_STATUS    0xFFFFFF    /**< Status bar border - white */

/* RPM Arc colors */
#define OPENDASH_COLOR_RPM_BG           0x660000    /**< Dark red - RPM arc background */
#define OPENDASH_COLOR_RPM_INDICATOR    0xFFFFFF    /**< White - RPM sweep indicator */
#define OPENDASH_COLOR_RPM_BORDER       0x000000    /**< Black - RPM arc outline */
#define OPENDASH_COLOR_RPM_LOW          0x00FF00    /**< Green - low RPM (unused) */
#define OPENDASH_COLOR_RPM_MID          0xFFFF00    /**< Yellow - mid RPM (unused) */
#define OPENDASH_COLOR_RPM_HIGH         0xFF0000    /**< Red - high RPM/redline (unused) */
#define OPENDASH_COLOR_RPM_TEXT         0x000000    /**< Black - RPM text fill */
#define OPENDASH_COLOR_RPM_OUTLINE      0xFFFFFF    /**< White - RPM text outline */

/* ────────────────────────────────────────────────────────────────────────────
 * Layout Position Constants
 * ──────────────────────────────────────────────────────────────────────────── */

/* Data section label positioning (centered, hugging top) */
#define OPENDASH_LABEL_OFFSET_X         0     /**< Centered horizontally */
#define OPENDASH_LABEL_OFFSET_Y         -2    /**< Hug top edge (compensate for outline) */

/* Data section value positioning (centered, hugging bottom) */
#define OPENDASH_VALUE_OFFSET_X         0     /**< Centered horizontally */
#define OPENDASH_VALUE_OFFSET_Y         2     /**< Hug bottom edge (compensate for outline) */

/* ────────────────────────────────────────────────────────────────────────────
 * Outlined Text System
 * 
 * Creates text with a colored outline that hugs each letter.
 * Use the create_outlined_label() function in ui_manager.c for implementation.
 * 
 * The outline is created by rendering the text 8 times at offset positions
 * (in the outline color) with the main text on top.
 * ──────────────────────────────────────────────────────────────────────────── */

/* Note: Outlined text implementation is in each ui_manager.c because it
 * creates multiple LVGL objects. See create_outlined_label() for usage. */

/* ────────────────────────────────────────────────────────────────────────────
 * Simple Text Styling (for non-outlined text)
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Apply white text styling (used for box labels)
 */
static inline void opendash_style_text_label(lv_obj_t *label)
{
    lv_obj_set_style_text_color(label, lv_color_hex(OPENDASH_COLOR_TEXT_SECONDARY), 0);
}

/**
 * @brief Apply simple white text (no outline, just color)
 */
static inline void opendash_style_text_white(lv_obj_t *label)
{
    lv_obj_set_style_text_color(label, lv_color_hex(OPENDASH_COLOR_TEXT_PRIMARY), 0);
}

/* ────────────────────────────────────────────────────────────────────────────
 * Legacy inline outline functions (using shadow - less accurate but simpler)
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Apply text style with shadow-based outline effect
 * @note For true letter-hugging outline, use opendash_create_outlined_text()
 */
static inline void opendash_apply_text_outline(lv_obj_t *label, 
                                                lv_color_t text_color,
                                                lv_color_t outline_color,
                                                uint8_t outline_width)
{
    lv_obj_set_style_text_color(label, text_color, 0);
}

static inline void opendash_style_text_normal(lv_obj_t *label)
{
    lv_obj_set_style_text_color(label, lv_color_hex(OPENDASH_COLOR_TEXT_PRIMARY), 0);
}

static inline void opendash_style_text_warning(lv_obj_t *label)
{
    lv_obj_set_style_text_color(label, lv_color_hex(OPENDASH_COLOR_WARNING_TEXT), 0);
}

static inline void opendash_style_text_rpm(lv_obj_t *label)
{
    lv_obj_set_style_text_color(label, lv_color_hex(OPENDASH_COLOR_RPM_TEXT), 0);
}

/* ────────────────────────────────────────────────────────────────────────────
 * Unit Conversion Utilities
 *
 * All internal values use SI/metric base units.
 * These functions convert from internal units to the user's display preference.
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Convert Celsius to Fahrenheit
 */
static inline float opendash_celsius_to_fahrenheit(float celsius)
{
    return (celsius * 9.0f / 5.0f) + 32.0f;
}

/**
 * @brief Convert temperature based on unit preference
 * @param celsius  Temperature in Celsius (internal storage format)
 * @param unit     User preference (CELSIUS or FAHRENHEIT)
 * @return Temperature in the requested unit
 */
static inline float opendash_convert_temp(float celsius, opendash_temp_unit_t unit)
{
    return (unit == OPENDASH_TEMP_FAHRENHEIT) ? opendash_celsius_to_fahrenheit(celsius) : celsius;
}

/**
 * @brief Convert km/h to MPH
 */
static inline float opendash_kmh_to_mph(float kmh)
{
    return kmh * 0.621371f;
}

/**
 * @brief Convert speed based on unit preference
 * @param kmh   Speed in km/h (internal storage format)
 * @param unit  User preference (KMH or MPH)
 * @return Speed in the requested unit
 */
static inline float opendash_convert_speed(float kmh, opendash_speed_unit_t unit)
{
    return (unit == OPENDASH_SPEED_MPH) ? opendash_kmh_to_mph(kmh) : kmh;
}

/**
 * @brief Convert kPa to BAR
 */
static inline float opendash_kpa_to_bar(float kpa)
{
    return kpa / 100.0f;
}

/**
 * @brief Convert kPa to PSI
 */
static inline float opendash_kpa_to_psi(float kpa)
{
    return kpa * 0.145038f;
}

/**
 * @brief Convert pressure based on unit preference
 * @param kpa   Pressure in kPa (internal storage format)
 * @param unit  User preference (KPA, BAR, or PSI)
 * @return Pressure in the requested unit
 */
static inline float opendash_convert_pressure(float kpa, opendash_pressure_unit_t unit)
{
    switch (unit) {
        case OPENDASH_PRESSURE_BAR: return opendash_kpa_to_bar(kpa);
        case OPENDASH_PRESSURE_PSI: return opendash_kpa_to_psi(kpa);
        default:                    return kpa;
    }
}

/**
 * @brief Convert km to miles
 */
static inline float opendash_km_to_miles(float km)
{
    return km * 0.621371f;
}

/**
 * @brief Convert distance based on unit preference
 * @param km    Distance in kilometers (internal storage format)
 * @param unit  User preference (KM or MI)
 * @return Distance in the requested unit
 */
static inline float opendash_convert_distance(float km, opendash_distance_unit_t unit)
{
    return (unit == OPENDASH_DISTANCE_MI) ? opendash_km_to_miles(km) : km;
}

/**
 * @brief Get temperature unit suffix string
 */
static inline const char* opendash_get_temp_suffix(opendash_temp_unit_t unit)
{
    return (unit == OPENDASH_TEMP_FAHRENHEIT) ? "°F" : "°C";
}

/**
 * @brief Get speed unit suffix string
 */
static inline const char* opendash_get_speed_suffix(opendash_speed_unit_t unit)
{
    return (unit == OPENDASH_SPEED_MPH) ? "MPH" : "km/h";
}

/**
 * @brief Get pressure unit suffix string
 */
static inline const char* opendash_get_pressure_suffix(opendash_pressure_unit_t unit)
{
    switch (unit) {
        case OPENDASH_PRESSURE_BAR: return "BAR";
        case OPENDASH_PRESSURE_PSI: return "PSI";
        default:                    return "kPa";
    }
}

/**
 * @brief Get distance unit suffix string
 */
static inline const char* opendash_get_distance_suffix(opendash_distance_unit_t unit)
{
    return (unit == OPENDASH_DISTANCE_MI) ? "mi" : "km";
}

#ifdef __cplusplus
}
#endif

#endif /* OPENDASH_UI_STYLES_H */
