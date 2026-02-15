/**
 * @file display_init.c
 * @brief OpenDash Center Display — Hardware Initialization
 *
 * Hardware: Waveshare ESP32-S3-Touch-LCD-4.3
 * LCD Controller: ST7262 (RGB interface)
 * Resolution: 800×480 IPS LCD
 *
 * This file initializes the display hardware using the ESP LCD RGB panel driver
 * and LVGL 9.x for rendering.
 *
 * @see ESP32-S3 LCD API:
 *      https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/lcd.html
 * @see LVGL 9.x Documentation:
 *      https://docs.lvgl.io/9.2/
 */

#include "display_init.h"
#include <stdbool.h>
#include <string.h>
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lvgl.h"

static const char *TAG = "display_init";

/* Display dimensions */
#define LCD_H_RES   800
#define LCD_V_RES   480

/* LCD RGB Interface GPIO pins for Waveshare ESP32-S3-Touch-LCD-4.3 */
#define LCD_PIXEL_CLOCK_HZ      (16 * 1000 * 1000)
#define LCD_BK_LIGHT_GPIO       GPIO_NUM_2
#define LCD_BK_LIGHT_ON_LEVEL   1

#define LCD_PIN_NUM_HSYNC       GPIO_NUM_46
#define LCD_PIN_NUM_VSYNC       GPIO_NUM_3
#define LCD_PIN_NUM_DE          GPIO_NUM_5
#define LCD_PIN_NUM_PCLK        GPIO_NUM_7
#define LCD_PIN_NUM_DISP_EN     (-1)  /* Not used */

/* RGB Data pins (active bits for RGB565: 5R, 6G, 5B = 16 bits) */
#define LCD_PIN_NUM_DATA0       GPIO_NUM_14  /* B3 */
#define LCD_PIN_NUM_DATA1       GPIO_NUM_38  /* B4 */
#define LCD_PIN_NUM_DATA2       GPIO_NUM_18  /* B5 */
#define LCD_PIN_NUM_DATA3       GPIO_NUM_17  /* B6 */
#define LCD_PIN_NUM_DATA4       GPIO_NUM_10  /* B7 */
#define LCD_PIN_NUM_DATA5       GPIO_NUM_39  /* G2 */
#define LCD_PIN_NUM_DATA6       GPIO_NUM_0   /* G3 */
#define LCD_PIN_NUM_DATA7       GPIO_NUM_45  /* G4 */
#define LCD_PIN_NUM_DATA8       GPIO_NUM_48  /* G5 */
#define LCD_PIN_NUM_DATA9       GPIO_NUM_47  /* G6 */
#define LCD_PIN_NUM_DATA10      GPIO_NUM_21  /* G7 */
#define LCD_PIN_NUM_DATA11      GPIO_NUM_1   /* R3 */
#define LCD_PIN_NUM_DATA12      GPIO_NUM_42  /* R4 */
#define LCD_PIN_NUM_DATA13      GPIO_NUM_41  /* R5 */
#define LCD_PIN_NUM_DATA14      GPIO_NUM_40  /* R6 */
#define LCD_PIN_NUM_DATA15      GPIO_NUM_9   /* R7 */

/* LVGL Configuration */
#define LVGL_TICK_PERIOD_MS     2
#define LVGL_BUFFER_HEIGHT      50   /* Height of LVGL draw buffer (partial buffer) */
#define LVGL_DRAW_BUF_LINES     LVGL_BUFFER_HEIGHT

/* Static handles */
static esp_lcd_panel_handle_t panel_handle = NULL;
static SemaphoreHandle_t lvgl_mux = NULL;
static lv_display_t *lvgl_disp = NULL;

/**
 * @brief LVGL tick timer callback
 *
 * This callback is called by the ESP timer to increment the LVGL tick counter.
 */
static void lvgl_tick_timer_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

/**
 * @brief LVGL flush callback for LVGL 9.x
 *
 * This function is called by LVGL when it has rendered a portion of the screen
 * and needs to transfer it to the display.
 *
 * @param disp      Pointer to the display driver
 * @param area      Area that was rendered
 * @param px_map    Pointer to the rendered pixel data
 */
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)lv_display_get_user_data(disp);
    
    const int offsetx1 = area->x1;
    const int offsetx2 = area->x2;
    const int offsety1 = area->y1;
    const int offsety2 = area->y2;
    
    /* Copy rendered data to the RGB frame buffer */
    esp_lcd_panel_draw_bitmap(panel, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, px_map);
    
    /* Inform LVGL that flushing is done */
    lv_display_flush_ready(disp);
}

/**
 * @brief Turn on backlight
 */
static esp_err_t lcd_backlight_on(void)
{
    gpio_config_t bk_gpio_config = {
        .pin_bit_mask = (1ULL << LCD_BK_LIGHT_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));
    ESP_ERROR_CHECK(gpio_set_level(LCD_BK_LIGHT_GPIO, LCD_BK_LIGHT_ON_LEVEL));
    ESP_LOGI(TAG, "Backlight ON (GPIO %d)", LCD_BK_LIGHT_GPIO);
    return ESP_OK;
}

/**
 * @brief Initialize the RGB LCD panel
 */
static esp_err_t lcd_panel_init(void)
{
    ESP_LOGI(TAG, "Initializing RGB LCD panel driver");
    
    esp_lcd_rgb_panel_config_t panel_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = LCD_PIXEL_CLOCK_HZ,
            .h_res = LCD_H_RES,
            .v_res = LCD_V_RES,
            .hsync_pulse_width = 4,
            .hsync_back_porch = 8,
            .hsync_front_porch = 8,
            .vsync_pulse_width = 4,
            .vsync_back_porch = 8,
            .vsync_front_porch = 8,
        },
        .data_width = 16,  /* RGB565 = 16 bits */
        .num_fbs = 1,
        .bounce_buffer_size_px = 0,
        .hsync_gpio_num = LCD_PIN_NUM_HSYNC,
        .vsync_gpio_num = LCD_PIN_NUM_VSYNC,
        .de_gpio_num = LCD_PIN_NUM_DE,
        .pclk_gpio_num = LCD_PIN_NUM_PCLK,
        .disp_gpio_num = LCD_PIN_NUM_DISP_EN,
        .data_gpio_nums = {
            LCD_PIN_NUM_DATA0,
            LCD_PIN_NUM_DATA1,
            LCD_PIN_NUM_DATA2,
            LCD_PIN_NUM_DATA3,
            LCD_PIN_NUM_DATA4,
            LCD_PIN_NUM_DATA5,
            LCD_PIN_NUM_DATA6,
            LCD_PIN_NUM_DATA7,
            LCD_PIN_NUM_DATA8,
            LCD_PIN_NUM_DATA9,
            LCD_PIN_NUM_DATA10,
            LCD_PIN_NUM_DATA11,
            LCD_PIN_NUM_DATA12,
            LCD_PIN_NUM_DATA13,
            LCD_PIN_NUM_DATA14,
            LCD_PIN_NUM_DATA15,
        },
    };
    
    /* Set flags after struct init - C99 designated initializer limitation workaround */
    panel_config.timings.flags.pclk_active_neg = true;
    panel_config.flags.fb_in_psram = true;
    
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_config, &panel_handle));
    ESP_LOGI(TAG, "RGB LCD panel created");
    
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_LOGI(TAG, "RGB LCD panel initialized");
    
    return ESP_OK;
}

/**
 * @brief Initialize LVGL library and display driver using LVGL 9.x API
 */
static esp_err_t lvgl_init(void)
{
    ESP_LOGI(TAG, "Initializing LVGL 9.x");
    
    /* Initialize LVGL */
    lv_init();
    
    /* Create mutex for LVGL thread safety */
    lvgl_mux = xSemaphoreCreateMutex();
    if (lvgl_mux == NULL) {
        ESP_LOGE(TAG, "Failed to create LVGL mutex");
        return ESP_FAIL;
    }
    
    /* Create LVGL display (LVGL 9.x API) */
    lvgl_disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    if (lvgl_disp == NULL) {
        ESP_LOGE(TAG, "Failed to create LVGL display");
        return ESP_FAIL;
    }
    
    /* Set the flush callback */
    lv_display_set_flush_cb(lvgl_disp, lvgl_flush_cb);
    
    /* Store panel handle as user data for the flush callback */
    lv_display_set_user_data(lvgl_disp, panel_handle);
    
    /* Allocate LVGL draw buffers in PSRAM */
    size_t buffer_size = LCD_H_RES * LVGL_DRAW_BUF_LINES * sizeof(lv_color_t);
    void *buf1 = heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM);
    if (buf1 == NULL) {
        ESP_LOGE(TAG, "Failed to allocate LVGL draw buffer 1 (%zu bytes)", buffer_size);
        return ESP_FAIL;
    }
    
    void *buf2 = heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM);
    if (buf2 == NULL) {
        ESP_LOGW(TAG, "Failed to allocate LVGL draw buffer 2, using single buffer mode");
        /* Single buffer mode is fine, just less efficient */
    }
    
    /* Set the draw buffers (LVGL 9.x API) */
    lv_display_set_buffers(lvgl_disp, buf1, buf2, buffer_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    
    /* Create and start LVGL tick timer */
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &lvgl_tick_timer_cb,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, LVGL_TICK_PERIOD_MS * 1000));
    
    ESP_LOGI(TAG, "LVGL 9.x initialized successfully");
    ESP_LOGI(TAG, "  Display: %dx%d", LCD_H_RES, LCD_V_RES);
    ESP_LOGI(TAG, "  Draw buffer: %d lines (%zu bytes)", LVGL_DRAW_BUF_LINES, buffer_size);
    ESP_LOGI(TAG, "  Double buffer: %s", buf2 != NULL ? "yes" : "no");
    
    return ESP_OK;
}

/**
 * @brief Initialize the display hardware.
 *
 * This function performs the following:
 * 1. Initialize the RGB LCD panel
 * 2. Turn on the backlight
 * 3. Initialize LVGL and register the display driver
 *
 * @return ESP_OK on success.
 */
esp_err_t display_init(void)
{
    ESP_LOGI(TAG, "===== OpenDash Center Display Init =====");
    ESP_LOGI(TAG, "Hardware: Waveshare ESP32-S3-Touch-LCD-4.3");
    ESP_LOGI(TAG, "Resolution: %dx%d RGB565", LCD_H_RES, LCD_V_RES);
    
    esp_err_t ret;
    
    /* Initialize LCD panel hardware */
    ret = lcd_panel_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LCD panel init failed");
        return ret;
    }
    
    /* Turn on backlight */
    ret = lcd_backlight_on();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Backlight init failed");
        return ret;
    }
    
    /* Initialize LVGL */
    ret = lvgl_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LVGL init failed");
        return ret;
    }
    
    ESP_LOGI(TAG, "===== Display initialization complete =====");
    
    return ESP_OK;
}

/**
 * @brief Lock LVGL mutex for thread-safe access
 *
 * Call this before accessing LVGL functions from multiple tasks.
 *
 * @param timeout_ms Timeout in milliseconds
 * @return true if mutex was acquired, false on timeout
 */
bool display_lvgl_lock(uint32_t timeout_ms)
{
    if (lvgl_mux == NULL) {
        return false;
    }
    return xSemaphoreTake(lvgl_mux, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

/**
 * @brief Unlock LVGL mutex
 */
void display_lvgl_unlock(void)
{
    if (lvgl_mux != NULL) {
        xSemaphoreGive(lvgl_mux);
    }
}

/**
 * @brief Get the LVGL display handle
 *
 * @return Pointer to the LVGL display, or NULL if not initialized
 */
lv_display_t *display_get_lvgl_disp(void)
{
    return lvgl_disp;
}
