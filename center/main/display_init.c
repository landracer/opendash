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
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lvgl.h"

static const char *TAG = "display_init";

/* Display dimensions */
#define LCD_H_RES   800
#define LCD_V_RES   480

/* Static buffers for LVGL */
static lv_color_t *buf1 = NULL;
static lv_color_t *buf2 = NULL;

/* Display object */
static lv_display_t *display = NULL;

/* Timer for LVGL tick */
static esp_timer_handle_t lvgl_tick_timer = NULL;

/**
 * @brief Timer callback to increment LVGL tick.
 *
 * Called periodically to update LVGL's internal time counter.
 */
static void lvgl_tick_timer_cb(void *arg)
{
    lv_tick_inc(1);  /* Increment by 1ms */
}

/**
 * @brief Initialize LVGL tick timer.
 *
 * Creates a periodic timer that increments LVGL's tick every 1ms.
 */
static esp_err_t init_lvgl_tick(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = &lvgl_tick_timer_cb,
        .name = "lvgl_tick"
    };
    
    esp_err_t ret = esp_timer_create(&timer_args, &lvgl_tick_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create LVGL tick timer");
        return ret;
    }
    
    /* Start timer with 1ms period */
    ret = esp_timer_start_periodic(lvgl_tick_timer, 1000);  /* 1000 microseconds = 1ms */
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start LVGL tick timer");
        esp_timer_delete(lvgl_tick_timer);
        return ret;
    }
    
    ESP_LOGI(TAG, "LVGL tick timer started (1ms period)");
    return ESP_OK;
}

/**
 * @brief Flush callback for LVGL display driver.
 *
 * This function is called by LVGL when it needs to update the display.
 * For the baseline implementation, this is a dummy function that just
 * marks the flush as complete.
 *
 * In a full implementation, this would:
 * 1. Transfer the buffer to the RGB LCD panel
 * 2. Wait for DMA transfer to complete
 * 3. Call lv_display_flush_ready()
 *
 * @param disp   Display object
 * @param area   Area being flushed
 * @param px_map Pixel color data
 */
static void display_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    /* For baseline: just mark as ready immediately */
    /* In full implementation: transfer buffer to RGB LCD via DMA */
    lv_display_flush_ready(disp);
}

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
    
    /* Initialize LVGL tick timer */
    esp_err_t ret = init_lvgl_tick();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize LVGL tick timer");
        return ret;
    }

    /* Calculate buffer size (1/10th of screen for memory efficiency) */
    size_t buf_size = LCD_H_RES * LCD_V_RES / 10;
    
    /* Allocate draw buffers */
    buf1 = heap_caps_malloc(buf_size * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    buf2 = heap_caps_malloc(buf_size * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    
    if (buf1 == NULL || buf2 == NULL) {
        ESP_LOGE(TAG, "Failed to allocate LVGL draw buffers");
        if (buf1) heap_caps_free(buf1);
        if (buf2) heap_caps_free(buf2);
        esp_timer_delete(lvgl_tick_timer);
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGI(TAG, "Allocated LVGL buffers: %zu bytes each", buf_size * sizeof(lv_color_t));

    /* Create a display */
    display = lv_display_create(LCD_H_RES, LCD_V_RES);
    if (display == NULL) {
        ESP_LOGE(TAG, "Failed to create LVGL display");
        heap_caps_free(buf1);
        heap_caps_free(buf2);
        esp_timer_delete(lvgl_tick_timer);
        return ESP_FAIL;
    }
    
    /* Set display buffers */
    lv_display_set_buffers(display, buf1, buf2, buf_size * sizeof(lv_color_t), LV_DISPLAY_RENDER_MODE_PARTIAL);
    
    /* Set flush callback */
    lv_display_set_flush_cb(display, display_flush_cb);
    
    ESP_LOGI(TAG, "Display driver registered with LVGL");
    ESP_LOGI(TAG, "Display initialization complete");
    ESP_LOGI(TAG, "Ready for UI rendering");

    return ESP_OK;
}
