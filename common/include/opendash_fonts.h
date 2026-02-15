/**
 * @file opendash_fonts.h
 * @brief OpenDash Font Management
 * 
 * This header provides utilities for using custom fonts in OpenDash.
 * Fonts are automatically converted from TrueType format during build.
 * 
 * Usage:
 * 1. Add your .ttf files to common/fonts/ttf/
 * 2. Configure fonts in common/fonts/font_config.json
 * 3. Build the project - fonts are automatically converted
 * 4. Use the fonts:
 *    LV_FONT_DECLARE(your_font_name_size);
 *    lv_obj_set_style_text_font(obj, &your_font_name_size, 0);
 */

#ifndef OPENDASH_FONTS_H
#define OPENDASH_FONTS_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Declare converted Montserrat fonts */
LV_FONT_DECLARE(montserrat_14);
LV_FONT_DECLARE(montserrat_18);
LV_FONT_DECLARE(montserrat_32);

/**
 * @brief Font size enumeration for easier font selection
 */
typedef enum {
    OPENDASH_FONT_SIZE_SMALL = 0,   ///< Small font (typically 14-16px)
    OPENDASH_FONT_SIZE_MEDIUM = 1,  ///< Medium font (typically 18-24px)
    OPENDASH_FONT_SIZE_LARGE = 2,   ///< Large font (typically 32-48px)
} opendash_font_size_t;

/**
 * @brief Get LVGL font pointer based on size category
 * 
 * This function maps abstract font sizes to actual LVGL fonts.
 * Uses converted Montserrat font as the default OpenDash font.
 * 
 * @param size Font size category
 * @return Pointer to lv_font_t, or NULL if invalid size
 */
static inline const lv_font_t* opendash_get_font(opendash_font_size_t size)
{
    switch (size) {
        case OPENDASH_FONT_SIZE_SMALL:
            return &montserrat_14;
        case OPENDASH_FONT_SIZE_MEDIUM:
            return &montserrat_18;
        case OPENDASH_FONT_SIZE_LARGE:
            return &montserrat_32;
        default:
            return &montserrat_14;
    }
}

/**
 * @brief Set font on an LVGL object using size category
 * 
 * Convenience function to set a font on an object.
 * 
 * @param obj LVGL object to set font on
 * @param size Font size category
 */
static inline void opendash_set_font(lv_obj_t* obj, opendash_font_size_t size)
{
    const lv_font_t* font = opendash_get_font(size);
    if (font != NULL) {
        lv_obj_set_style_text_font(obj, font, 0);
    }
}

#ifdef __cplusplus
}
#endif

#endif /* OPENDASH_FONTS_H */
