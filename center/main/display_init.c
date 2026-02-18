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
 * ============================================================================
 * CRITICAL DISPLAY CONFIGURATION - READ BEFORE MODIFYING
 * ============================================================================
 *
 * These settings were carefully tuned to eliminate visual artifacts (noise,
 * "humming", stripe patterns) and ensure crisp font rendering. Incorrect
 * values can cause eye strain and headaches!
 *
 * KEY SETTINGS:
 *
 * 1. PIXEL CLOCK (LCD_PIXEL_CLOCK_HZ):
 *    - Official Waveshare library uses: 16 MHz (conservative, stable)
 *    - ST7262 datasheet allows: 23-25-27 MHz (min-typ-max)
 *    - Higher values = faster refresh but may cause artifacts with PSRAM
 *    - RECOMMENDED: 16 MHz for stability
 *
 * 2. BOUNCE BUFFER (LCD_BOUNCE_BUFFER_SIZE):
 *    - Required when framebuffer is in PSRAM (fb_in_psram = true)
 *    - Copies PSRAM data to internal SRAM before DMA transfer
 *    - Prevents visual noise/artifacts from PSRAM bandwidth contention
 *    - ESP-IDF recommends: 10-20 lines (e.g., 10 * LCD_H_RES)
 *    - RECOMMENDED: 20 lines for best stability
 *
 * 3. TIMING VALUES (hsync/vsync porches):
 *    - Values from ST7262 datasheet (Page 52)
 *    - Using TYPICAL values (4/8/8/4/8/8) for stable, centered display
 *    - DO NOT CHANGE unless you understand RGB LCD timing
 *
 * TROUBLESHOOTING:
 *
 * - Stripe artifacts / visual noise:
 *   → Increase bounce buffer size (try 30 * LCD_H_RES)
 *   → Decrease pixel clock (try 14 MHz)
 *
 * - Display drift / misalignment on reset:
 *   → Check timing values match ST7262 datasheet
 *   → Verify pclk_active_neg = true
 *
 * - Blurry fonts:
 *   → Usually a font BPP issue, not display timing
 *   → Use 4-bit BPP for fonts (see common/fonts/font_config.json)
 *   → 8-bit BPP can cause blurring on RGB565 displays
 *
 * @see ST7262 Datasheet (Page 52 - RGB Timing Table):
 *      https://files.waveshare.com/wiki/common/ST7262.pdf
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
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "lvgl.h"
#include "ui_manager.h"

static const char *TAG = "display_init";

/* Display dimensions */
#define LCD_H_RES   800
#define LCD_V_RES   480

/* LCD RGB Interface GPIO pins for Waveshare ESP32-S3-Touch-LCD-4.3 */
/* ST7262 Datasheet (Page 52): DCLK Frequency = 23-25-27 MHz */
/* Waveshare official library uses 16 MHz for stability */
#define LCD_PIXEL_CLOCK_HZ      (16 * 1000 * 1000)  /* 16 MHz (Waveshare official) */

/* Bounce buffer for PSRAM stability - prevents visual noise/artifacts */
/* ESP-IDF recommends 10-20 lines when framebuffer is in PSRAM */
#define LCD_BOUNCE_BUFFER_SIZE  (20 * LCD_H_RES)  /* 16000 pixels = 20 lines */
#define LCD_BK_LIGHT_GPIO       GPIO_NUM_2
#define LCD_BK_LIGHT_ON_LEVEL   1

#define LCD_PIN_NUM_HSYNC       GPIO_NUM_46
#define LCD_PIN_NUM_VSYNC       GPIO_NUM_3
#define LCD_PIN_NUM_DE          GPIO_NUM_5
#define LCD_PIN_NUM_PCLK        GPIO_NUM_7
#define LCD_PIN_NUM_DISP_EN     (-1)  /* Not used */

/* ──────────────────────────────────────────────────────────────────────────
 * GT911 Capacitive Touch Controller Configuration
 * ──────────────────────────────────────────────────────────────────────────
 * Hardware: GT911 I2C Touch Controller (Waveshare ESP32-S3-Touch-LCD-4.3)
 * Address: 0x5D (default) or 0x14 (alternate, depending on INT pin state)
 * Resolution: 800×480 matching the display
 *
 * @see GT911 Datasheet: https://files.waveshare.com/upload/3/3e/GT911_Datasheet.pdf
 */
#define TOUCH_I2C_NUM           I2C_NUM_0
#define TOUCH_I2C_SCL_PIN       GPIO_NUM_20   /* SCL on GPIO20 */
#define TOUCH_I2C_SDA_PIN       GPIO_NUM_19   /* SDA on GPIO19 */
#define TOUCH_I2C_FREQ_HZ       400000        /* 400 kHz I2C clock */
#define TOUCH_GT911_ADDR        0x5D          /* GT911 I2C address */
#define TOUCH_GT911_INT_PIN     GPIO_NUM_4    /* Interrupt pin (input) */
#define TOUCH_GT911_RST_PIN     GPIO_NUM_8    /* Reset pin (output) */

/* GT911 useful register addresses */
#define GT911_REG_STATUS        0x814E        /* Touch status register */
#define GT911_REG_NUM_TOUCHES   0x814E        /* Number of touches in bits 3:0 */
#define GT911_REG_TOUCHES_BASE  0x8150        /* Base address for touch points */

/* ──────────────────────────────────────────────────────────────────────────
 * Boot Button Configuration
 * ──────────────────────────────────────────────────────────────────────────
 * The ESP32-S3 boot button (GPIO0) can be used for screen navigation.
 */
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define BOOT_BUTTON_DEBOUNCE_MS 50

/* LVGL Input Device Configuration */
#define LVGL_INPUT_QUEUE_SIZE   20

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

/* Backlight PWM Configuration */
#define LCD_BK_LEDC_TIMER       LEDC_TIMER_0
#define LCD_BK_LEDC_CHANNEL     LEDC_CHANNEL_0
#define LCD_BK_LEDC_SPEED_MODE  LEDC_LOW_SPEED_MODE
#define LCD_BK_LEDC_FREQ_HZ     5000   /* 5 kHz PWM frequency */
#define LCD_BK_LEDC_RESOLUTION  LEDC_TIMER_10_BIT  /* 0-1023 duty range */

/* Static handles */
static esp_lcd_panel_handle_t panel_handle = NULL;
static SemaphoreHandle_t lvgl_mux = NULL;
static lv_display_t *lvgl_disp = NULL;
static uint8_t current_brightness = 100;  /* Default to full brightness */
static lv_indev_t *touch_indev = NULL;    /* LVGL touch input device */
static lv_indev_t *button_indev = NULL;   /* LVGL button input device */
static QueueHandle_t touch_queue = NULL;  /* Queue for touch events */
static TaskHandle_t touch_task_handle = NULL;  /* Touch reading task */
static TaskHandle_t button_task_handle = NULL; /* Button reading task */
static bool touched = false;              /* Current touch state */
static uint16_t touch_x = 0, touch_y = 0; /* Current touch coordinates */

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
 * @brief Initialize backlight with PWM for brightness control
 *
 * Uses LEDC peripheral for smooth PWM-based brightness adjustment.
 */
static esp_err_t lcd_backlight_init(void)
{
    /* Configure LEDC timer */
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LCD_BK_LEDC_SPEED_MODE,
        .duty_resolution  = LCD_BK_LEDC_RESOLUTION,
        .timer_num        = LCD_BK_LEDC_TIMER,
        .freq_hz          = LCD_BK_LEDC_FREQ_HZ,
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    /* Configure LEDC channel */
    ledc_channel_config_t ledc_channel = {
        .speed_mode     = LCD_BK_LEDC_SPEED_MODE,
        .channel        = LCD_BK_LEDC_CHANNEL,
        .timer_sel      = LCD_BK_LEDC_TIMER,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = LCD_BK_LIGHT_GPIO,
        .duty           = 1023,  /* Start at full brightness (100%) */
        .hpoint         = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));

    current_brightness = 100;
    ESP_LOGI(TAG, "Backlight PWM initialized (GPIO %d, 100%% brightness)", LCD_BK_LIGHT_GPIO);
    return ESP_OK;
}

/**
 * @brief Initialize I2C for GT911 touch controller communication
 */
static esp_err_t touch_i2c_init(void)
{
    /* I2C will be initialized on demand if needed for GT911 communication
     * For now, just log that touch initialization is deferred */
    ESP_LOGI(TAG, "I2C touch controller disabled (will implement full GT911 driver in future)");
    return ESP_OK;
}

/**
 * @brief Initialize GT911 capacitive touch controller
 *
 * Performs initial configuration including reset and register setup.
 */
static esp_err_t touch_gt911_init(void)
{
    ESP_LOGI(TAG, "Initializing GT911 touch controller...");
    
    /* Configure reset pin (output) */
    gpio_config_t rst_config = {
        .pin_bit_mask = (1ULL << TOUCH_GT911_RST_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&rst_config));
    
    /* Configure interrupt pin (input) */
    gpio_config_t int_config = {
        .pin_bit_mask = (1ULL << TOUCH_GT911_INT_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&int_config));
    
    /* Perform reset sequence: pull low then high */
    gpio_set_level(TOUCH_GT911_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(TOUCH_GT911_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    
    ESP_LOGI(TAG, "GT911 touch controller initialized (Addr=0x%02X, Reset=%d, INT=%d)",
             TOUCH_GT911_ADDR, TOUCH_GT911_RST_PIN, TOUCH_GT911_INT_PIN);
    return ESP_OK;
}

/**
 * @brief Read touch point from GT911 using new I2C master API
 *
 * Performs polled I2C reads from GT911 registers for touch coordinates.
 * Note: This is a simplified implementation that polls the touch controller.
 */
static esp_err_t touch_gt911_read_point(uint16_t *x, uint16_t *y, bool *touched)
{
    uint8_t buffer[1] = {0};
    
    /* For now, just report no touch - full GT911 driver is a future enhancement */
    *touched = false;
    *x = 0;
    *y = 0;
    
    ESP_LOGD(TAG, "Touch read: no touch detected (simplified implementation)");
    return ESP_OK;
}

/**
 * @brief Boot button initialization
 */
static esp_err_t boot_button_init(void)
{
    gpio_config_t button_config = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button_config));
    
    ESP_LOGI(TAG, "Boot button initialized (GPIO %d)", BOOT_BUTTON_GPIO);
    return ESP_OK;
}

/**
 * @brief Button reading task
 *
 * Polls the boot button GPIO periodically. When a press is detected,
 * directly calls ui_manager_next_screen() to switch screens.
 */
static void button_read_task(void *pvParameters)
{
    (void)pvParameters;
    static int last_button_state = 1;  /* Initially not pressed (active low) */
    static uint32_t last_press_time = 0;
    const uint32_t debounce_ms = BOOT_BUTTON_DEBOUNCE_MS;
    
    ESP_LOGI(TAG, "Button reading task started - press boot button to switch screens");
    
    while (1) {
        int button_level = gpio_get_level(BOOT_BUTTON_GPIO);
        uint32_t now = esp_log_timestamp();
        
        /* Detect state change with debouncing */
        if (button_level != last_button_state && (now - last_press_time) > debounce_ms) {
            last_button_state = button_level;
            last_press_time = now;
            
            if (button_level == 0) {  /* Button pressed (active low) */
                ESP_LOGI(TAG, "Boot button pressed - switching screen");
                
                /* Directly call the screen switching function */
                if (display_lvgl_lock(10)) {
                    ui_manager_next_screen();
                    display_lvgl_unlock();
                }
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));  /* 100 Hz polling for responsive button */
    }
}

/**
 * @brief Start button reading task
 */
static esp_err_t start_button_task(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(
        button_read_task,
        "button_task",
        4096,
        NULL,
        4,  /* Same priority as touch task */
        &button_task_handle,
        0   /* Core 0 */
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create button reading task");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Button reading task started on core 0");
    return ESP_OK;
}

/**
 * @brief LVGL touch input device callback
 *
 * Called by LVGL to read the current touch state.
 */
static void touch_input_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    data->point.x = touch_x;
    data->point.y = touch_y;
    data->state = touched ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/**
 * @brief LVGL button input device callback
 *
 * Called by LVGL to read the boot button state.
 */
static void button_input_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    /* GPIO0 (boot button) is active low - 0 means pressed */
    int button_level = gpio_get_level(BOOT_BUTTON_GPIO);
    data->state = (button_level == 0) ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->btn_id = 0;
}

/**
 * @brief Touch reading task
 *
 * Periodically reads the GT911 touch controller and updates the input state.
 */
static void touch_read_task(void *pvParameters)
{
    (void)pvParameters;
    uint16_t x, y;
    bool pressed;
    
    ESP_LOGI(TAG, "Touch reading task started");
    
    while (1) {
        if (touch_gt911_read_point(&x, &y, &pressed) == ESP_OK) {
            if (display_lvgl_lock(10)) {
                touch_x = x;
                touch_y = y;
                touched = pressed;
                display_lvgl_unlock();
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(20));  /* 50 Hz polling rate */
    }
}

/**
 * @brief Register LVGL input devices (touch and button)
 */
static esp_err_t register_lvgl_input_devices(void)
{
    /* Register touch device */
    touch_indev = lv_indev_create();
    if (touch_indev == NULL) {
        ESP_LOGE(TAG, "Failed to create LVGL touch input device");
        return ESP_FAIL;
    }
    lv_indev_set_type(touch_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch_indev, touch_input_cb);
    
    /* Register button device */
    button_indev = lv_indev_create();
    if (button_indev == NULL) {
        ESP_LOGE(TAG, "Failed to create LVGL button input device");
        return ESP_FAIL;
    }
    lv_indev_set_type(button_indev, LV_INDEV_TYPE_BUTTON);
    lv_indev_set_read_cb(button_indev, button_input_cb);
    
    ESP_LOGI(TAG, "LVGL input devices registered (touch + button)");
    return ESP_OK;
}

/**
 * @brief Start touch reading task
 */
static esp_err_t start_touch_task(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(
        touch_read_task,
        "touch_task",
        4096,
        NULL,
        4,  /* Above UI task priority */
        &touch_task_handle,
        0   /* Core 0 */
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create touch reading task");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Touch reading task started on core 0");
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
            /* ST7262 Timing Values from Datasheet (Page 52)
             * Source: ST7262.pdf - Parallel 24-bit RGB Interface Timing Table
             *
             * Horizontal Timing (units: PCLK cycles):
             *   - Pulse Width:  Min=2, Typ=4, Max=8
             *   - Back Porch:   Min=4, Typ=8, Max=48
             *   - Front Porch:  Min=4, Typ=8, Max=48
             *   - Total H:      Min=808, Typ=816, Max=896 (800 + porches + pulse)
             *
             * Vertical Timing (units: HSYNC lines):
             *   - Pulse Width:  Min=2, Typ=4, Max=8
             *   - Back Porch:   Min=4, Typ=8, Max=12
             *   - Front Porch:  Min=4, Typ=8, Max=12
             *   - Total V:      Min=488, Typ=496, Max=504 (480 + porches + pulse)
             *
             * Using TYPICAL values for stable, centered display.
             */
            .hsync_pulse_width = 4,   /* Datasheet: 2-4-8, using typical */
            .hsync_back_porch = 8,    /* Datasheet: 4-8-48, using typical */
            .hsync_front_porch = 8,   /* Datasheet: 4-8-48, using typical */
            .vsync_pulse_width = 4,   /* Datasheet: 2-4-8, using typical */
            .vsync_back_porch = 8,    /* Datasheet: 4-8-12, using typical */
            .vsync_front_porch = 8,   /* Datasheet: 4-8-12, using typical */
        },
        .data_width = 16,  /* RGB565 = 16 bits */
        .num_fbs = 1,
        /* Bounce buffer: copies PSRAM->SRAM in chunks before DMA, prevents visual noise */
        .bounce_buffer_size_px = LCD_BOUNCE_BUFFER_SIZE,
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
    
    /* Set color format to RGB565 to match the hardware panel.
     * LVGL 9.x internal lv_color_t is RGB888 (3 bytes), but the
     * RGB LCD panel uses 16-bit RGB565.  Setting this ensures LVGL
     * renders into the draw buffers in the correct format. */
    lv_display_set_color_format(lvgl_disp, LV_COLOR_FORMAT_RGB565);
    
    /* Set the flush callback */
    lv_display_set_flush_cb(lvgl_disp, lvgl_flush_cb);
    
    /* Store panel handle as user data for the flush callback */
    lv_display_set_user_data(lvgl_disp, panel_handle);
    
    /* Allocate LVGL draw buffers in PSRAM — sized for RGB565 (2 bytes/pixel) */
    size_t buffer_size = LCD_H_RES * LVGL_DRAW_BUF_LINES * sizeof(uint16_t);
    void *buf1 = heap_caps_aligned_alloc(64, buffer_size, MALLOC_CAP_SPIRAM);
    if (buf1 == NULL) {
        ESP_LOGE(TAG, "Failed to allocate LVGL draw buffer 1 (%zu bytes)", buffer_size);
        return ESP_FAIL;
    }
    
    void *buf2 = heap_caps_aligned_alloc(64, buffer_size, MALLOC_CAP_SPIRAM);
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
    
    /* Initialize backlight with PWM */
    ret = lcd_backlight_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Backlight init failed");
        return ret;
    }
    
    /* Initialize I2C for touch controller */
    ret = touch_i2c_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Touch I2C init failed (touch will not work)");
        /* Don't return; allow system to boot without touch */
    }
    
    /* Initialize GT911 touch controller */
    ret = touch_gt911_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "GT911 init failed (touch will not work)");
        /* Don't return; allow system to boot without touch */
    }
    
    /* Initialize boot button */
    ret = boot_button_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Boot button init failed");
        /* Don't return; allow system to boot without button */
    }
    
    /* Initialize LVGL */
    ret = lvgl_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LVGL init failed");
        return ret;
    }
    
    /* Register LVGL input devices (touch + button) */
    ret = register_lvgl_input_devices();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register LVGL input devices");
        /* Don't return; allow system to boot without input */
    }
    
    /* Start touch reading task */
    ret = start_touch_task();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start touch reading task");
        /* Don't return; allow system to boot without touch */
    }
    
    /* Start button reading task */
    ret = start_button_task();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start button reading task");
        /* Don't return; allow system to boot without button */
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

/**
 * @brief Set display backlight brightness
 *
 * @param brightness Brightness level 0-100 (0 = off, 100 = max)
 * @return ESP_OK on success, error code on failure.
 */
esp_err_t display_set_brightness(uint8_t brightness)
{
    if (brightness > 100) {
        brightness = 100;
    }
    
    /* Convert 0-100% to 0-1023 duty cycle (10-bit resolution) */
    uint32_t duty = (brightness * 1023) / 100;
    
    esp_err_t ret = ledc_set_duty(LCD_BK_LEDC_SPEED_MODE, LCD_BK_LEDC_CHANNEL, duty);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set backlight duty");
        return ret;
    }
    
    ret = ledc_update_duty(LCD_BK_LEDC_SPEED_MODE, LCD_BK_LEDC_CHANNEL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to update backlight duty");
        return ret;
    }
    
    current_brightness = brightness;
    ESP_LOGI(TAG, "Backlight brightness set to %d%%", brightness);
    return ESP_OK;
}

/**
 * @brief Get current backlight brightness
 *
 * @return Current brightness level 0-100
 */
uint8_t display_get_brightness(void)
{
    return current_brightness;
}
