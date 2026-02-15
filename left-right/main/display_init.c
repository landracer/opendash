/**
 * @file display_init.c
 * @brief OpenDash Left/Right Gauges — Hardware Initialization
 *
 * Hardware: Waveshare ESP32-S3-LCD-2.8C
 * LCD Controller: ST7701 (RGB interface)
 * Resolution: 480×480 Round IPS LCD
 *
 * This file initializes the display hardware using the ESP LCD RGB panel driver
 * and LVGL for rendering.
 *
 * @see ESP32-S3 LCD API:
 *      https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32s3/api-reference/peripherals/lcd.html
 * @see LVGL Documentation:
 *      https://docs.lvgl.io/master/
 */

#include "display_init.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lvgl.h"

static const char *TAG = "display_init";

/* Display dimensions */
#define LCD_H_RES   480
#define LCD_V_RES   480

/* LCD RGB Interface GPIO pins for Waveshare ESP32-S3-LCD-2.8C */
#define LCD_PIXEL_CLOCK_HZ      (16 * 1000 * 1000)

/* I2C Configuration for backlight control via CH422G */
#define I2C_MASTER_NUM              0
#define I2C_MASTER_SCL_IO           GPIO_NUM_9
#define I2C_MASTER_SDA_IO           GPIO_NUM_8
#define I2C_MASTER_FREQ_HZ          400000
#define I2C_MASTER_TIMEOUT_MS       1000
#define CH422G_I2C_ADDRESS          0x24
#define CH422G_BACKLIGHT_ADDRESS    0x38
#define CH422G_OUTPUT_MODE_CMD      0x01
#define CH422G_BACKLIGHT_ON_CMD     0x1E
#define CH422G_BACKLIGHT_OFF_CMD    0x1A

#define LCD_PIN_NUM_HSYNC       GPIO_NUM_46
#define LCD_PIN_NUM_VSYNC       GPIO_NUM_3
#define LCD_PIN_NUM_DE          GPIO_NUM_5
#define LCD_PIN_NUM_PCLK        GPIO_NUM_7
#define LCD_PIN_NUM_DISP_EN     -1  // Not used

/* RGB Data pins - Same pin mapping as center display (4.3") */
#define LCD_PIN_NUM_DATA0       GPIO_NUM_14  // B3
#define LCD_PIN_NUM_DATA1       GPIO_NUM_38  // B4
#define LCD_PIN_NUM_DATA2       GPIO_NUM_18  // B5
#define LCD_PIN_NUM_DATA3       GPIO_NUM_17  // B6
#define LCD_PIN_NUM_DATA4       GPIO_NUM_10  // B7
#define LCD_PIN_NUM_DATA5       GPIO_NUM_39  // G2
#define LCD_PIN_NUM_DATA6       GPIO_NUM_0   // G3
#define LCD_PIN_NUM_DATA7       GPIO_NUM_45  // G4
#define LCD_PIN_NUM_DATA8       GPIO_NUM_48  // G5
#define LCD_PIN_NUM_DATA9       GPIO_NUM_47  // G6
#define LCD_PIN_NUM_DATA10      GPIO_NUM_21  // G7
#define LCD_PIN_NUM_DATA11      GPIO_NUM_1   // R3
#define LCD_PIN_NUM_DATA12      GPIO_NUM_2   // R4
#define LCD_PIN_NUM_DATA13      GPIO_NUM_42  // R5
#define LCD_PIN_NUM_DATA14      GPIO_NUM_41  // R6
#define LCD_PIN_NUM_DATA15      GPIO_NUM_40  // R7

/* LVGL Configuration */
#define LVGL_TICK_PERIOD_MS     2
#define LVGL_BUFFER_HEIGHT      100  // Height of LVGL draw buffer

/* Static handles */
static esp_lcd_panel_handle_t panel_handle = NULL;
static SemaphoreHandle_t lvgl_mux = NULL;
static lv_disp_t *lvgl_disp = NULL;
static i2c_port_t i2c_port = I2C_MASTER_NUM;

/**
 * @brief LVGL tick timer callback
 */
static void lvgl_tick_timer_cb(void *arg)
{
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

/**
 * @brief LVGL flush callback
 */
static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)drv->user_data;
    const int offsetx1 = area->x1;
    const int offsetx2 = area->x2;
    const int offsety1 = area->y1;
    const int offsety2 = area->y2;
    
    /* Copy rendered data to the RGB frame buffer */
    esp_lcd_panel_draw_bitmap(panel, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);
    
    lv_disp_flush_ready(drv);
}

/**
 * @brief Initialize I2C master for backlight control
 */
static esp_err_t i2c_master_init(void)
{
    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    
    ESP_ERROR_CHECK(i2c_param_config(i2c_port, &i2c_conf));
    ESP_ERROR_CHECK(i2c_driver_install(i2c_port, i2c_conf.mode, 0, 0, 0));
    ESP_LOGI(TAG, "I2C master initialized");
    
    return ESP_OK;
}

/**
 * @brief Turn on backlight via CH422G I2C GPIO expander
 */
static esp_err_t lcd_backlight_on(void)
{
    uint8_t write_buf;
    
    /* Configure CH422G to output mode */
    write_buf = CH422G_OUTPUT_MODE_CMD;
    esp_err_t ret = i2c_master_write_to_device(i2c_port, CH422G_I2C_ADDRESS, &write_buf, 1, 
                                                I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to configure CH422G: %s", esp_err_to_name(ret));
    }
    
    /* Turn on backlight by pulling backlight pin high */
    write_buf = CH422G_BACKLIGHT_ON_CMD;
    ret = i2c_master_write_to_device(i2c_port, CH422G_BACKLIGHT_ADDRESS, &write_buf, 1, 
                                     I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to turn on backlight: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Backlight ON");
    }
    
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
            .flags.pclk_active_neg = 1,
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 1,  // Single frame buffer
        .bounce_buffer_size_px = 0,  // No bounce buffer
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
        .flags.fb_in_psram = 1,  // Allocate frame buffer in PSRAM
    };
    
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_config, &panel_handle));
    ESP_LOGI(TAG, "RGB LCD panel created");
    
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_LOGI(TAG, "RGB LCD panel initialized");
    
    return ESP_OK;
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
    disp_drv.user_data = panel_handle;
    
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
    
    ESP_LOGI(TAG, "LVGL initialized successfully");
    
    return ESP_OK;
}

/**
 * @brief Initialize the display hardware.
 *
 * This function performs the following:
 * 1. Initialize I2C master for backlight control
 * 2. Initialize the RGB LCD panel
 * 3. Turn on the backlight
 * 4. Initialize LVGL and register the display driver
 *
 * @return ESP_OK on success.
 */
esp_err_t display_init(void)
{
    ESP_LOGI(TAG, "Initializing LVGL for %dx%d round display", LCD_H_RES, LCD_V_RES);
    
    /* Step 1: Initialize I2C for backlight control */
    ESP_ERROR_CHECK(i2c_master_init());
    
    /* Step 2: Initialize RGB LCD panel */
    ESP_ERROR_CHECK(lcd_panel_init());
    
    /* Step 3: Turn on backlight */
    ESP_ERROR_CHECK(lcd_backlight_on());
    
    /* Small delay to let display stabilize */
    vTaskDelay(pdMS_TO_TICKS(100));
    
    /* Step 4: Initialize LVGL */
    ESP_ERROR_CHECK(lvgl_init());
    
    ESP_LOGI(TAG, "Display initialization complete");
    ESP_LOGI(TAG, "Ready for UI rendering");
    
    return ESP_OK;
}
