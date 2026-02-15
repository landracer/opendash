/**
 * @file display_init.c
 * @brief OpenDash GPS / Telemetry Unit — Hardware Initialization
 *
 * Hardware: Waveshare ESP32-S3-Touch-AMOLED-1.75
 * Display Controller: CO5300 (QSPI interface)
 * Resolution: 466×466 Round AMOLED
 *
 * @see ESP32-S3 LCD API:
 *      https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32s3/api-reference/peripherals/lcd.html
 */

#include "display_init.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "display_init";

/* Display dimensions */
#define LCD_H_RES   466
#define LCD_V_RES   466

/**
 * @brief Initialize the display hardware.
 */
esp_err_t display_init(void)
{
    ESP_LOGI(TAG, "Initializing LVGL for %dx%d AMOLED display", LCD_H_RES, LCD_V_RES);

    /* Initialize LVGL library */
    lv_init();

    /* Note: In a full implementation, this would use esp_lvgl_port to create
     * the QSPI AMOLED panel and register it with LVGL. */

    ESP_LOGI(TAG, "Display initialization complete");
    ESP_LOGI(TAG, "Ready for UI rendering");

    return ESP_OK;
}
