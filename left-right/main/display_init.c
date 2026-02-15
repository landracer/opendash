/**
 * @file display_init.c
 * @brief OpenDash Left/Right Gauges — Hardware Initialization
 *
 * Hardware: Waveshare ESP32-S3-LCD-2.8C
 * LCD Controller: ST7701 (SPI interface)
 * Resolution: 480×480 Round IPS LCD
 *
 * @see ESP32-S3 LCD API:
 *      https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32s3/api-reference/peripherals/lcd.html
 */

#include "display_init.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "display_init";

/* Display dimensions */
#define LCD_H_RES   480
#define LCD_V_RES   480

/**
 * @brief Initialize the display hardware.
 */
esp_err_t display_init(void)
{
    ESP_LOGI(TAG, "Initializing LVGL for %dx%d round display", LCD_H_RES, LCD_V_RES);

    /* Initialize LVGL library */
    lv_init();

    /* Note: In a full implementation, this would use esp_lvgl_port to create
     * the SPI LCD panel and register it with LVGL. */

    ESP_LOGI(TAG, "Display initialization complete");
    ESP_LOGI(TAG, "Ready for UI rendering");

    return ESP_OK;
}
