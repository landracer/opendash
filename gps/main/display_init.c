/**
 * @file display_init.c
 * @brief OpenDash GPS / Telemetry Unit — Hardware Initialization
 *
 * Hardware: Waveshare ESP32-S3-Touch-AMOLED-1.75
 * Display Controller: CO5300 (QSPI interface)
 * Resolution: 466×466 Round AMOLED
 *
 * This file initializes the display hardware. For the AMOLED display with QSPI
 * interface, this is a baseline implementation that sets up LVGL with a simulated
 * display to prevent watchdog timeouts. Full QSPI driver initialization requires
 * the Waveshare BSP component.
 *
 * @note To fully enable this display, add the Waveshare BSP:
 *       idf.py add-dependency "waveshare/esp32_s3_touch_amoled_1_75^2.0.6"
 *
 * @see ESP32-S3 LCD API:
 *      https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32s3/api-reference/peripherals/lcd.html
 * @see LVGL Documentation:
 *      https://docs.lvgl.io/master/
 */

#include "display_init.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lvgl.h"

static const char *TAG = "display_init";

/* Display dimensions */
#define LCD_H_RES   466
#define LCD_V_RES   466

/* LVGL Configuration */
#define LVGL_TICK_PERIOD_MS     2
#define LVGL_BUFFER_HEIGHT      100  // Height of LVGL draw buffer

/* Static handles */
static SemaphoreHandle_t lvgl_mux = NULL;
static lv_disp_t *lvgl_disp = NULL;

/**
 * @brief LVGL tick timer callback
 */
static void lvgl_tick_timer_cb(void *arg)
{
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

/**
 * @brief LVGL flush callback (simulated for baseline)
 *
 * This is a stub flush callback that simply marks the flush as ready.
 * In a full implementation, this would send the pixel data to the QSPI
 * display controller.
 */
static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    /* Simulate display flush - in real implementation, this would:
     * 1. Set the display window (area->x1, y1, x2, y2)
     * 2. Send pixel data via QSPI to CO5300 controller
     * 3. Wait for transfer complete
     */
    
    /* For now, just mark as ready to prevent blocking */
    lv_disp_flush_ready(drv);
}

/**
 * @brief Initialize LVGL library and display driver
 */
static esp_err_t lvgl_init(void)
{
    ESP_LOGI(TAG, "Initializing LVGL");
    
    /* Initialize LVGL */
    lv_init();
    
    /* Create mutex for LVGL */
    lvgl_mux = xSemaphoreCreateMutex();
    if (lvgl_mux == NULL) {
        ESP_LOGE(TAG, "Failed to create LVGL mutex");
        return ESP_FAIL;
    }
    
    /* Allocate LVGL draw buffers */
    size_t buffer_size = LCD_H_RES * LVGL_BUFFER_HEIGHT * sizeof(lv_color_t);
    void *buf1 = heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM);
    if (buf1 == NULL) {
        ESP_LOGE(TAG, "Failed to allocate LVGL draw buffer");
        return ESP_FAIL;
    }
    
    /* Initialize LVGL display driver */
    static lv_disp_draw_buf_t disp_buf;
    lv_disp_draw_buf_init(&disp_buf, buf1, NULL, LCD_H_RES * LVGL_BUFFER_HEIGHT);
    
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_H_RES;
    disp_drv.ver_res = LCD_V_RES;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = NULL;  // No hardware handle in baseline
    
    lvgl_disp = lv_disp_drv_register(&disp_drv);
    if (lvgl_disp == NULL) {
        ESP_LOGE(TAG, "Failed to register LVGL display driver");
        return ESP_FAIL;
    }
    
    /* Create and start LVGL tick timer */
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &lvgl_tick_timer_cb,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, LVGL_TICK_PERIOD_MS * 1000));
    
    ESP_LOGI(TAG, "LVGL initialized successfully (baseline mode)");
    
    return ESP_OK;
}

/**
 * @brief Initialize the display hardware.
 *
 * This is a baseline implementation that initializes LVGL with a simulated
 * display to prevent watchdog timeouts. The UI will render in memory but
 * won't be displayed on the actual hardware without the QSPI driver.
 *
 * To fully enable this display:
 * 1. Add Waveshare BSP component to idf_component.yml
 * 2. Replace this implementation with BSP initialization calls
 * 3. Configure QSPI pins and CO5300 controller
 *
 * @return ESP_OK on success.
 */
esp_err_t display_init(void)
{
    ESP_LOGI(TAG, "Initializing LVGL for %dx%d AMOLED display", LCD_H_RES, LCD_V_RES);
    ESP_LOGW(TAG, "Running in baseline mode - display output not connected");
    ESP_LOGW(TAG, "To enable AMOLED display, add Waveshare BSP component");
    
    /* Initialize LVGL in baseline mode */
    ESP_ERROR_CHECK(lvgl_init());
    
    ESP_LOGI(TAG, "Display initialization complete (baseline mode)");
    ESP_LOGI(TAG, "Ready for UI rendering (output to memory only)");
    
    return ESP_OK;
}
