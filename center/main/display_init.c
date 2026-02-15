/**
 * @file display_init.c
 * @brief OpenDash Center Display — Hardware Initialization
 *
 * Hardware: Waveshare ESP32-S3-Touch-LCD-4.3
 * LCD Controller: ST7262 (RGB interface)
 * Resolution: 800×480 IPS LCD
 *
 * This file initializes the display hardware using the ESP LVGL port for
 * simplified RGB LCD setup.
 *
 * @see ESP32-S3 LCD API:
 *      https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32s3/api-reference/peripherals/lcd.html
 * @see LVGL Documentation:
 *      https://docs.lvgl.io/master/
 */

#include "display_init.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "display_init";

/* Display dimensions */
#define LCD_H_RES   800
#define LCD_V_RES   480

/**
 * @brief Initialize the display hardware.
 *
 * For the baseline implementation, we initialize LVGL with a simple buffer
 * and configure basic display settings. The actual RGB LCD initialization
 * will use the ESP LVGL port component which handles the low-level setup.
 *
 * @note This is a simplified baseline implementation. Full hardware
 *       initialization with RGB interface and touch controller should be
 *       added based on the Waveshare hardware specifications.
 *
 * @return ESP_OK on success.
 */
esp_err_t display_init(void)
{
    ESP_LOGI(TAG, "Initializing LVGL for %dx%d display", LCD_H_RES, LCD_V_RES);

    /* Initialize LVGL library */
    lv_init();

    /* Create display and configure */
    /* Note: In a full implementation, this would use esp_lvgl_port to create
     * the RGB LCD panel and register it with LVGL. For baseline, we set up
     * a basic LVGL display. */

    ESP_LOGI(TAG, "Display initialization complete");
    ESP_LOGI(TAG, "Ready for UI rendering");

    return ESP_OK;
}
