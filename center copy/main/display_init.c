/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
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
#define TOUCH_I2C_SCL_PIN       GPIO_NUM_9    /* SCL on GPIO9 (per Waveshare demo) */
#define TOUCH_I2C_SDA_PIN       GPIO_NUM_8    /* SDA on GPIO8 */
#define TOUCH_I2C_FREQ_HZ       400000        /* 400 kHz (per Waveshare demo) */
#define TOUCH_GT911_ADDR        0x5D          /* GT911 I2C address (alt: 0x14) */
/* Note: Waveshare ESP32-S3-Touch-LCD-4.3 uses CH422G IO expander (0x24/0x38)
 * for GT911 reset. GPIO4 is a control GPIO (not I2C). INT/RST not directly
 * connected to ESP32 GPIO pins. */

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
#define LCD_PIN_NUM_DATA12      GPIO_NUM_2   /* R4 (per Waveshare demo) */
#define LCD_PIN_NUM_DATA13      GPIO_NUM_42  /* R5 */
#define LCD_PIN_NUM_DATA14      GPIO_NUM_41  /* R6 */
#define LCD_PIN_NUM_DATA15      GPIO_NUM_40  /* R7 — frees GPIO9 for I2C SCL */

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
static SemaphoreHandle_t s_vsync_sem = NULL;  /* Vsync synchronization for tear-free rendering */
static lv_display_t *lvgl_disp = NULL;
static uint8_t current_brightness = 100;  /* Default to full brightness */
static lv_indev_t *touch_indev = NULL;    /* LVGL touch input device */
static lv_indev_t *button_indev = NULL;   /* LVGL button input device */
static TaskHandle_t touch_task_handle = NULL;  /* Touch reading task */
static TaskHandle_t button_task_handle = NULL; /* Button reading task */
static bool touched = false;              /* Current touch state */
static uint16_t touch_x = 0, touch_y = 0; /* Current touch coordinates */
static i2c_master_bus_handle_t touch_i2c_bus = NULL;    /* I2C bus for GT911 */
static i2c_master_dev_handle_t gt911_dev     = NULL;    /* GT911 device handle */

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
 * @brief VSYNC event callback — fires in ISR context when the LCD has finished
 *        displaying a complete frame.  Unblocks the flush callback so LVGL only
 *        writes to the "back" framebuffer while the "front" one is being scanned.
 */
static IRAM_ATTR bool on_vsync_event(esp_lcd_panel_handle_t panel,
                                      const esp_lcd_rgb_panel_event_data_t *edata,
                                      void *user_ctx)
{
    BaseType_t high_task_wakeup = pdFALSE;
    if (s_vsync_sem) {
        xSemaphoreGiveFromISR(s_vsync_sem, &high_task_wakeup);
    }
    return high_task_wakeup == pdTRUE;
}

/**
 * @brief LVGL flush callback — double-framebuffer + vsync synchronization.
 *
 * In DIRECT render mode with 2 framebuffers, px_map IS one of the HW frame
 * buffers (allocated by esp_lcd_new_rgb_panel).  draw_bitmap recognises its
 * own pointer and performs a zero-copy DMA source swap on the next vsync.
 * We block here until the swap completes so that LVGL never writes into the
 * buffer currently being scanned to the panel.
 */
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)lv_display_get_user_data(disp);

    /* Swap the active framebuffer (takes effect on next vsync) */
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, px_map);

    /* Wait for vsync before returning — prevents writing to the displayed buffer */
    xSemaphoreTake(s_vsync_sem, pdMS_TO_TICKS(100));

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
 * @brief Initialize I2C bus 0 for GT911 touch controller
 */
static esp_err_t touch_i2c_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port   = TOUCH_I2C_NUM,
        .sda_io_num = TOUCH_I2C_SDA_PIN,
        .scl_io_num = TOUCH_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &touch_i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create touch I2C bus: %s", esp_err_to_name(ret));
        return ret;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = TOUCH_GT911_ADDR,
        .scl_speed_hz    = TOUCH_I2C_FREQ_HZ,
    };
    ret = i2c_master_bus_add_device(touch_i2c_bus, &dev_cfg, &gt911_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add GT911 device: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Touch I2C bus initialized (SDA=%d, SCL=%d, addr=0x%02X)",
             TOUCH_I2C_SDA_PIN, TOUCH_I2C_SCL_PIN, TOUCH_GT911_ADDR);
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
    
    /* ── CH422G IO Expander — reset GT911 via hardware reset line ──
     * The Waveshare ESP32-S3-Touch-LCD-4.3 board uses a CH422G IO expander
     * (I2C addrs 0x24 / 0x38) to control the GT911 reset pin.
     * Sequence from Waveshare demo:
     *   1. Write 0x01 to 0x24 → set CH422G to output mode
     *   2. Write 0x2C to 0x38 → assert reset (touch reset LOW)
     *   3. Delay 100ms
     *   4. Write 0x2E to 0x38 → de-assert reset (touch reset HIGH)
     *   5. Delay 200ms → GT911 boot
     */
    {
        /* GPIO4 LOW during reset selects I2C address 0x5D */
        gpio_config_t io4_cfg = {
            .pin_bit_mask = (1ULL << GPIO_NUM_4),
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&io4_cfg);
        gpio_set_level(GPIO_NUM_4, 0);
        
        /* Create temporary device handles for CH422G IO expander */
        i2c_master_dev_handle_t ch422g_mode_dev = NULL;
        i2c_master_dev_handle_t ch422g_port_dev = NULL;
        i2c_device_config_t ch422g_mode_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = 0x24,
            .scl_speed_hz    = TOUCH_I2C_FREQ_HZ,
        };
        i2c_device_config_t ch422g_port_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = 0x38,
            .scl_speed_hz    = TOUCH_I2C_FREQ_HZ,
        };
        esp_err_t ret = i2c_master_bus_add_device(touch_i2c_bus, &ch422g_mode_cfg, &ch422g_mode_dev);
        esp_err_t ret2 = i2c_master_bus_add_device(touch_i2c_bus, &ch422g_port_cfg, &ch422g_port_dev);
        
        if (ret == ESP_OK && ret2 == ESP_OK) {
            /* Set CH422G to output mode */
            uint8_t ch422g_mode = 0x01;
            ret = i2c_master_transmit(ch422g_mode_dev, &ch422g_mode, 1, 100);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "CH422G mode set (0x24 → 0x01)");
                
                /* Assert touch reset via CH422G */
                uint8_t reset_assert = 0x2C;
                i2c_master_transmit(ch422g_port_dev, &reset_assert, 1, 100);
                vTaskDelay(pdMS_TO_TICKS(100));
                
                /* De-assert touch reset */
                uint8_t reset_deassert = 0x2E;
                i2c_master_transmit(ch422g_port_dev, &reset_deassert, 1, 100);
                ESP_LOGI(TAG, "GT911 reset via CH422G complete");
            } else {
                ESP_LOGW(TAG, "CH422G not found at 0x24 (ret=%s) — skipping HW reset",
                         esp_err_to_name(ret));
            }
        } else {
            ESP_LOGW(TAG, "Failed to add CH422G devices to I2C bus");
        }
        
        /* Clean up CH422G device handles */
        if (ch422g_mode_dev) i2c_master_bus_rm_device(ch422g_mode_dev);
        if (ch422g_port_dev) i2c_master_bus_rm_device(ch422g_port_dev);
        
        /* Wait for GT911 to boot after reset */
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    
    /* ── I2C Bus Scan — determine what's actually on the bus ── */
    ESP_LOGI(TAG, "Touch I2C bus scan (SDA=%d, SCL=%d, %d Hz):",
             TOUCH_I2C_SDA_PIN, TOUCH_I2C_SCL_PIN, TOUCH_I2C_FREQ_HZ);
    int found_count = 0;
    for (uint8_t addr = 0x03; addr <= 0x77; addr++) {
        esp_err_t probe_ret = i2c_master_probe(touch_i2c_bus, addr, 100);
        if (probe_ret == ESP_OK) {
            found_count++;
            ESP_LOGI(TAG, "  0x%02X %s%s",
                     addr,
                     (addr == 0x5D) ? "(GT911 addr A)" : 
                     (addr == 0x14) ? "(GT911 addr B)" :
                     (addr == 0x2D) ? "(unknown — half-shifted 0x5D?)" : "",
                     (addr == TOUCH_GT911_ADDR) ? " ← configured" : "");
        }
    }
    ESP_LOGI(TAG, "  Total: %d devices on touch I2C bus", found_count);
    
    /* ── No INT/RST pins available on this board — probe directly ── */
    /* Waveshare ESP32-S3-Touch-LCD-4.3: TP_INT=-1, TP_RST=-1 */
    
    /* Check configured address first */
    esp_err_t probe_ret = i2c_master_probe(touch_i2c_bus, TOUCH_GT911_ADDR, 100);
    if (probe_ret == ESP_OK) {
        ESP_LOGI(TAG, "GT911 FOUND at 0x%02X ✓", TOUCH_GT911_ADDR);
        return ESP_OK;
    }
    
    /* Try alternate address */
    probe_ret = i2c_master_probe(touch_i2c_bus, 0x14, 100);
    if (probe_ret == ESP_OK) {
        ESP_LOGW(TAG, "GT911 at ALTERNATE addr 0x14 — reconfiguring device handle");
        i2c_master_bus_rm_device(gt911_dev);
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = 0x14,
            .scl_speed_hz    = TOUCH_I2C_FREQ_HZ,
        };
        esp_err_t ret = i2c_master_bus_add_device(touch_i2c_bus, &dev_cfg, &gt911_dev);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "GT911 reconfigured at 0x14 ✓");
        }
        return ret;
    }
    
    ESP_LOGE(TAG, "GT911 NOT found at 0x5D or 0x14 — touch will NOT work");
    return ESP_ERR_NOT_FOUND;
}

/**
 * @brief Write to a 16-bit GT911 register.
 */
static esp_err_t gt911_write_reg(uint16_t reg, const uint8_t *data, size_t len)
{
    if (!gt911_dev) return ESP_ERR_INVALID_STATE;
    uint8_t buf[2 + len];
    buf[0] = (reg >> 8) & 0xFF;
    buf[1] = reg & 0xFF;
    if (data && len) memcpy(&buf[2], data, len);
    return i2c_master_transmit(gt911_dev, buf, 2 + len, 50);
}

/**
 * @brief Read from a 16-bit GT911 register.
 */
static esp_err_t gt911_read_reg(uint16_t reg, uint8_t *data, size_t len)
{
    if (!gt911_dev) return ESP_ERR_INVALID_STATE;
    uint8_t addr[2] = { (reg >> 8) & 0xFF, reg & 0xFF };
    return i2c_master_transmit_receive(gt911_dev, addr, 2, data, len, 50);
}

/**
 * @brief Read touch point from GT911 via I2C.
 *
 * GT911 register map:
 *   0x814E — Status register (bit7=buffer_ready, bits3:0=num_touches)
 *   0x8150 — Touch point 1 (8 bytes: id, xL, xH, yL, yH, sizeL, sizeH, reserved)
 *
 * After reading, status register must be cleared (write 0x00).
 */
static esp_err_t touch_gt911_read_point(uint16_t *x, uint16_t *y, bool *touched)
{
    *touched = false;
    *x = 0;
    *y = 0;

    if (!gt911_dev) return ESP_ERR_INVALID_STATE;

    /* Read status register */
    uint8_t status = 0;
    esp_err_t ret = gt911_read_reg(GT911_REG_STATUS, &status, 1);
    if (ret != ESP_OK) return ret;

    /* Bit 7 = buffer ready, bits 3:0 = number of touches (0-5) */
    bool buffer_ready = (status & 0x80) != 0;
    uint8_t num_touches = status & 0x0F;

    /* Clear status register (must be done every read) */
    uint8_t zero = 0x00;
    gt911_write_reg(GT911_REG_STATUS, &zero, 1);

    if (!buffer_ready || num_touches == 0 || num_touches > 5) {
        return ESP_OK;  /* No touch */
    }

    /* Read first touch point (8 bytes starting at 0x8150) */
    uint8_t touch_data[8];
    ret = gt911_read_reg(GT911_REG_TOUCHES_BASE, touch_data, 8);
    if (ret != ESP_OK) return ret;

    /* Parse: [0]=trackID, [1]=xL, [2]=xH, [3]=yL, [4]=yH, [5]=szL, [6]=szH */
    uint16_t raw_x = (touch_data[2] << 8) | touch_data[1];
    uint16_t raw_y = (touch_data[4] << 8) | touch_data[3];

    /* Clamp to display resolution */
    if (raw_x >= LCD_H_RES) raw_x = LCD_H_RES - 1;
    if (raw_y >= LCD_V_RES) raw_y = LCD_V_RES - 1;

    *x = raw_x;
    *y = raw_y;
    *touched = true;

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
    uint32_t touch_count = 0;
    uint32_t error_count = 0;
    uint32_t last_report = 0;
    
    ESP_LOGI(TAG, "Touch reading task started");
    
    while (1) {
        esp_err_t ret = touch_gt911_read_point(&x, &y, &pressed);
        if (ret == ESP_OK) {
            if (display_lvgl_lock(10)) {
                touch_x = x;
                touch_y = y;
                touched = pressed;
                display_lvgl_unlock();
            }
            if (pressed) {
                touch_count++;
                ESP_LOGI(TAG, "TOUCH: x=%d y=%d (count=%"PRIu32")", x, y, touch_count);
            }
        } else {
            error_count++;
        }
        
        /* Periodic status every 30s */
        uint32_t now = esp_log_timestamp();
        if (now - last_report > 30000) {
            last_report = now;
            ESP_LOGI(TAG, "Touch stats: touches=%"PRIu32" errors=%"PRIu32"",
                     touch_count, error_count);
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
        .num_fbs = 2,  /* Double framebuffer for tear-free vsync rendering */
        .bounce_buffer_size_px = LCD_BOUNCE_BUFFER_SIZE,  /* 20-line bounce buffer — prevents PSRAM DMA contention artifacts */
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

    /* Register vsync callback for tear-free rendering */
    s_vsync_sem = xSemaphoreCreateBinary();
    esp_lcd_rgb_panel_event_callbacks_t cbs = {
        .on_vsync = on_vsync_event,
    };
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(panel_handle, &cbs, NULL));
    ESP_LOGI(TAG, "Vsync callback registered (double-buffer tear-free mode)");

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
    
    /* Get the two hardware framebuffers allocated by the RGB panel driver.
     * In DIRECT mode, LVGL renders directly into these — no intermediate copy.
     * The flush callback just swaps which one the DMA reads from. */
    void *fb0 = NULL, *fb1 = NULL;
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panel_handle, 2, &fb0, &fb1));
    size_t fb_size = LCD_H_RES * LCD_V_RES * sizeof(uint16_t);  /* RGB565 */
    ESP_LOGI(TAG, "  Framebuffers: fb0=%p fb1=%p (%zu KB each)", fb0, fb1, fb_size / 1024);

    /* DIRECT render mode: LVGL writes into one FB while the other is displayed.
     * Combined with the vsync-gated flush callback, this eliminates tearing. */
    lv_display_set_buffers(lvgl_disp, fb0, fb1, fb_size, LV_DISPLAY_RENDER_MODE_DIRECT);
    
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
    ESP_LOGI(TAG, "  Render mode: DIRECT (double-framebuffer, tear-free)");
    ESP_LOGI(TAG, "  Framebuffer size: %zu KB each", fb_size / 1024);
    
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
