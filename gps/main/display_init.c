/**
 * @file display_init.c
 * @brief OpenDash GPS / Telemetry Unit — Hardware Initialization
 *
 * Hardware: Waveshare ESP32-S3-Touch-AMOLED-1.75
 * Display Controller: CO5300 (QSPI interface)
 * Touch Controller: CST9217 (I2C)
 * Resolution: 466×466 Round AMOLED
 *
 * Direct LVGL + ESP LCD integration (no BSP / esp_lvgl_adapter).
 * The BSP and esp_lvgl_adapter are incompatible with ESP-IDF 6.1-dev
 * (API mismatch: lv_event_get_invalidated_area doesn't exist in LVGL 9.2).
 *
 * This file manually initializes:
 *   1. I2C master bus (shared: touch, IMU, GPS, PMU, IO expander)
 *   2. SPI bus + CO5300 QSPI AMOLED panel
 *   3. CST9217 touch controller via esp_lcd_touch component
 *   4. LVGL display driver + draw buffers + tick timer
 *   5. Boot button for screen mode cycling
 *
 * ============================================================================
 * CO5300 QSPI Protocol Notes:
 *   - Commands are 32-bit: opcode(8) + cmd(8) + padding(16)
 *   - Opcodes: 0x02 (write param), 0x32 (write color), 0x03 (read)
 *   - Coordinates must be 2-pixel aligned (QSPI requirement)
 *   - Brightness via register 0x51 (AMOLED, no PWM backlight)
 *   - X gap = 6 pixels (column address starts at 6)
 * ============================================================================
 *
 * @see CO5300 Driver: espressif/esp_lcd_co5300
 * @see CST9217 Driver: waveshare/esp_lcd_touch_cst9217
 * @see LVGL 9.2: https://docs.lvgl.io/9.2/
 */

#include "display_init.h"
#include "ui_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_touch_cst9217.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lvgl.h"

static const char *TAG = "display_init";

/* ──────────────────────────────────────────────────────────────────────────
 * Pin Definitions — Waveshare ESP32-S3-Touch-AMOLED-1.75
 * ──────────────────────────────────────────────────────────────────────── */

/* CO5300 QSPI AMOLED */
#define LCD_QSPI_HOST       SPI2_HOST
#define LCD_PIN_CS           GPIO_NUM_12
#define LCD_PIN_SCLK         GPIO_NUM_38
#define LCD_PIN_DATA0        GPIO_NUM_4
#define LCD_PIN_DATA1        GPIO_NUM_5
#define LCD_PIN_DATA2        GPIO_NUM_6
#define LCD_PIN_DATA3        GPIO_NUM_7
#define LCD_PIN_RST          GPIO_NUM_39
#define LCD_QSPI_CLK_HZ     (40 * 1000 * 1000)  /* 40 MHz */

/* I2C Bus (shared: touch, IMU, GPS, PMU, IO expander) */
#define I2C_MASTER_NUM       I2C_NUM_1
#define I2C_PIN_SDA          GPIO_NUM_15
#define I2C_PIN_SCL          GPIO_NUM_14
#define I2C_MASTER_FREQ_HZ   400000   /* 400 kHz fast mode */

/* CST9217 Touch Controller */
#define TOUCH_I2C_ADDR       0x5A
#define TOUCH_PIN_RST        GPIO_NUM_40
#define TOUCH_PIN_INT        GPIO_NUM_11

/* Boot Button */
#define BOOT_BUTTON_GPIO     GPIO_NUM_0
#define BOOT_BUTTON_DEBOUNCE_MS  50

/* LVGL Configuration */
#define LVGL_TICK_PERIOD_MS  2
#define LVGL_DRAW_BUF_LINES 50   /* 50 lines per draw buffer */

/* CO5300 QSPI command opcodes */
#define LCD_OPCODE_WRITE_CMD   0x02
#define LCD_OPCODE_WRITE_COLOR 0x32

/* ──────────────────────────────────────────────────────────────────────────
 * Static Handles
 * ──────────────────────────────────────────────────────────────────────── */

static i2c_master_bus_handle_t i2c_bus_handle = NULL;
static esp_lcd_panel_handle_t  panel_handle   = NULL;
static esp_lcd_panel_io_handle_t io_handle    = NULL;
static esp_lcd_touch_handle_t  touch_handle   = NULL;
static SemaphoreHandle_t       lvgl_mux       = NULL;
static lv_display_t           *lvgl_disp      = NULL;
static uint8_t                 current_brightness = 80;
static TaskHandle_t            button_task_handle = NULL;

/* Touch state */
static bool     touch_pressed = false;
static uint16_t touch_x = 0, touch_y = 0;

/* ──────────────────────────────────────────────────────────────────────────
 * CO5300 Vendor Init Commands
 * ──────────────────────────────────────────────────────────────────────── */

static const co5300_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},    /* RGB565 pixel format */
    {0x35, (uint8_t[]){0x00}, 1, 0},    /* Tearing effect line ON */
    {0x53, (uint8_t[]){0x20}, 1, 0},    /* Brightness ctrl enabled */
    {0x51, (uint8_t[]){0xFF}, 1, 0},    /* Max brightness */
    {0x63, (uint8_t[]){0xFF}, 1, 0},    /* HBM brightness */
    {0x2A, (uint8_t[]){0x00, 0x06, 0x01, 0xD7}, 4, 0},   /* Column: 6..471 */
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 600},  /* Row: 0..465 */
    {0x11, NULL, 0, 600},   /* Sleep Out */
    {0x29, NULL, 0, 0},     /* Display ON */
};

/* ──────────────────────────────────────────────────────────────────────────
 * LVGL Callbacks
 * ──────────────────────────────────────────────────────────────────────── */

static void lvgl_tick_timer_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

/**
 * @brief LVGL flush callback — sends rendered pixels to CO5300 via QSPI.
 *
 * The CO5300 QSPI requires 2-pixel aligned coordinates. The rounder
 * event ensures this before flush is called.
 */
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)lv_display_get_user_data(disp);

    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, px_map);

    lv_display_flush_ready(disp);
}

/**
 * @brief LVGL rounder callback — aligns coordinates to 2-pixel boundaries.
 *
 * Required by CO5300 QSPI interface. Without this, partial updates
 * will show pixel artifacts from misaligned writes.
 */
static void lvgl_rounder_cb(lv_event_t *e)
{
    lv_area_t *area = lv_event_get_param(e);
    if (area == NULL) return;

    area->x1 = (area->x1 >> 1) << 1;       /* Round down to even */
    area->y1 = (area->y1 >> 1) << 1;
    area->x2 = ((area->x2 >> 1) << 1) + 1;  /* Round up to odd */
    area->y2 = ((area->y2 >> 1) << 1) + 1;
}

/**
 * @brief LVGL touch input callback.
 */
static void touch_input_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    data->point.x = touch_x;
    data->point.y = touch_y;
    data->state = touch_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/* ──────────────────────────────────────────────────────────────────────────
 * I2C Bus Initialization
 * ──────────────────────────────────────────────────────────────────────── */

static esp_err_t i2c_bus_init(void)
{
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port   = I2C_MASTER_NUM,
        .sda_io_num = I2C_PIN_SDA,
        .scl_io_num = I2C_PIN_SCL,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t ret = i2c_new_master_bus(&bus_config, &i2c_bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C master bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "I2C bus initialized (SDA=%d, SCL=%d, %d kHz)",
             I2C_PIN_SDA, I2C_PIN_SCL, I2C_MASTER_FREQ_HZ / 1000);
    return ESP_OK;
}

/* ──────────────────────────────────────────────────────────────────────────
 * CO5300 QSPI AMOLED Panel Init
 * ──────────────────────────────────────────────────────────────────────── */

static esp_err_t lcd_panel_init(void)
{
    ESP_LOGI(TAG, "Initializing CO5300 QSPI AMOLED panel...");

    /* Initialize SPI bus
     * Note: data0/mosi, data1/miso, data2/quadwp, data3/quadhd are unions —
     * only use the data*_io_num names to avoid duplicate initializer warnings.
     */
    spi_bus_config_t bus_config = {
        .data0_io_num    = LCD_PIN_DATA0,
        .data1_io_num    = LCD_PIN_DATA1,
        .sclk_io_num     = LCD_PIN_SCLK,
        .data2_io_num    = LCD_PIN_DATA2,
        .data3_io_num    = LCD_PIN_DATA3,
        .max_transfer_sz = GPS_LCD_H_RES * GPS_LCD_V_RES * 2,
        .flags           = SPICOMMON_BUSFLAG_QUAD,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_QSPI_HOST, &bus_config, SPI_DMA_CH_AUTO));

    /* Create QSPI panel IO */
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num         = LCD_PIN_CS,
        .dc_gpio_num         = -1,         /* No DC pin in QSPI mode */
        .spi_mode            = 0,
        .pclk_hz             = LCD_QSPI_CLK_HZ,
        .trans_queue_depth    = 10,
        .lcd_cmd_bits        = 32,
        .lcd_param_bits      = 8,
        .flags = {
            .quad_mode = true,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_QSPI_HOST, &io_config, &io_handle));

    /* CO5300 vendor config */
    co5300_vendor_config_t vendor_config = {
        .init_cmds      = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags = {
            .use_qspi_interface = 1,
        },
    };

    /* Create CO5300 panel */
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num  = LCD_PIN_RST,
        .rgb_ele_order   = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel  = 16,
        .vendor_config   = &vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(io_handle, &panel_config, &panel_handle));

    /* Initialize panel (reset + init commands) */
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 6, 0));  /* X offset = 6 */
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    ESP_LOGI(TAG, "CO5300 panel initialized (%dx%d, QSPI @ %d MHz)",
             GPS_LCD_H_RES, GPS_LCD_V_RES, LCD_QSPI_CLK_HZ / 1000000);
    return ESP_OK;
}

/* ──────────────────────────────────────────────────────────────────────────
 * CST9217 Touch Controller Init
 * ──────────────────────────────────────────────────────────────────────── */

static esp_err_t touch_init(void)
{
    ESP_LOGI(TAG, "Initializing CST9217 touch controller...");

    /* Create touch panel IO over I2C */
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG();
    tp_io_config.scl_speed_hz = I2C_MASTER_FREQ_HZ;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus_handle, &tp_io_config, &tp_io_handle));

    /* Touch controller config */
    esp_lcd_touch_config_t tp_cfg = {
        .x_max       = GPS_LCD_H_RES,
        .y_max       = GPS_LCD_V_RES,
        .rst_gpio_num = TOUCH_PIN_RST,
        .int_gpio_num = TOUCH_PIN_INT,
        .levels = {
            .reset     = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy  = 0,
            .mirror_x = 1,
            .mirror_y = 1,
        },
    };

    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_cst9217(tp_io_handle, &tp_cfg, &touch_handle));
    ESP_LOGI(TAG, "CST9217 touch initialized (addr=0x%02X, INT=%d, RST=%d)",
             TOUCH_I2C_ADDR, TOUCH_PIN_INT, TOUCH_PIN_RST);
    return ESP_OK;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Touch Reading Task
 * ──────────────────────────────────────────────────────────────────────── */

static void touch_read_task(void *pvParameters)
{
    (void)pvParameters;
    esp_lcd_touch_point_data_t points[1];
    uint8_t count = 0;

    ESP_LOGI(TAG, "Touch reading task started");
    while (1) {
        if (touch_handle != NULL) {
            esp_lcd_touch_read_data(touch_handle);
            esp_err_t ret = esp_lcd_touch_get_data(touch_handle, points, &count, 1);

            if (display_lvgl_lock(10)) {
                if (ret == ESP_OK && count > 0) {
                    touch_pressed = true;
                    touch_x = points[0].x;
                    touch_y = points[0].y;
                } else {
                    touch_pressed = false;
                }
                display_lvgl_unlock();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));  /* 50 Hz */
    }
}

/* ──────────────────────────────────────────────────────────────────────────
 * Boot Button
 * ──────────────────────────────────────────────────────────────────────── */

static esp_err_t boot_button_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask  = (1ULL << BOOT_BUTTON_GPIO),
        .mode          = GPIO_MODE_INPUT,
        .pull_up_en    = GPIO_PULLUP_ENABLE,
        .pull_down_en  = GPIO_PULLDOWN_DISABLE,
        .intr_type     = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    return ESP_OK;
}

static void button_read_task(void *pvParameters)
{
    (void)pvParameters;
    int last_state = 1;
    uint32_t last_press = 0;

    ESP_LOGI(TAG, "Button task started — press boot button to cycle screens");
    while (1) {
        int level = gpio_get_level(BOOT_BUTTON_GPIO);
        uint32_t now = esp_log_timestamp();

        if (level != last_state && (now - last_press) > BOOT_BUTTON_DEBOUNCE_MS) {
            last_state = level;
            last_press = now;
            if (level == 0) {
                ESP_LOGI(TAG, "Boot button pressed — switching screen");
                if (display_lvgl_lock(10)) {
                    ui_manager_next_screen();
                    display_lvgl_unlock();
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ──────────────────────────────────────────────────────────────────────────
 * LVGL Initialization (Direct — no BSP / adapter)
 * ──────────────────────────────────────────────────────────────────────── */

static esp_err_t lvgl_init_direct(void)
{
    ESP_LOGI(TAG, "Initializing LVGL 9.x (direct mode, no BSP)");

    lv_init();

    /* Mutex for thread-safe LVGL access */
    lvgl_mux = xSemaphoreCreateMutex();
    if (lvgl_mux == NULL) {
        ESP_LOGE(TAG, "Failed to create LVGL mutex");
        return ESP_FAIL;
    }

    /* Create LVGL display */
    lvgl_disp = lv_display_create(GPS_LCD_H_RES, GPS_LCD_V_RES);
    if (lvgl_disp == NULL) {
        ESP_LOGE(TAG, "Failed to create LVGL display");
        return ESP_FAIL;
    }

    lv_display_set_color_format(lvgl_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(lvgl_disp, lvgl_flush_cb);
    lv_display_set_user_data(lvgl_disp, panel_handle);

    /* Register rounder for QSPI 2-pixel alignment */
    lv_display_add_event_cb(lvgl_disp, lvgl_rounder_cb,
                            LV_EVENT_INVALIDATE_AREA, NULL);

    /* Allocate draw buffers in PSRAM */
    size_t buf_size = GPS_LCD_H_RES * LVGL_DRAW_BUF_LINES * sizeof(uint16_t);
    void *buf1 = heap_caps_aligned_alloc(64, buf_size, MALLOC_CAP_SPIRAM);
    void *buf2 = heap_caps_aligned_alloc(64, buf_size, MALLOC_CAP_SPIRAM);

    if (buf1 == NULL) {
        ESP_LOGE(TAG, "Failed to allocate LVGL draw buffer (%zu bytes)", buf_size);
        return ESP_FAIL;
    }

    lv_display_set_buffers(lvgl_disp, buf1, buf2, buf_size,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    /* Register touch input */
    lv_indev_t *touch_indev = lv_indev_create();
    if (touch_indev) {
        lv_indev_set_type(touch_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(touch_indev, touch_input_cb);
    }

    /* Tick timer */
    const esp_timer_create_args_t tick_args = {
        .callback = &lvgl_tick_timer_cb,
        .name     = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, LVGL_TICK_PERIOD_MS * 1000));

    ESP_LOGI(TAG, "LVGL initialized (draw buf: %d lines, double: %s)",
             LVGL_DRAW_BUF_LINES, buf2 ? "yes" : "no");
    return ESP_OK;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Public API
 * ──────────────────────────────────────────────────────────────────────── */

esp_err_t display_init(void)
{
    ESP_LOGI(TAG, "===== OpenDash GPS Display Init =====");
    ESP_LOGI(TAG, "Hardware: Waveshare ESP32-S3-Touch-AMOLED-1.75");
    ESP_LOGI(TAG, "Resolution: %dx%d AMOLED (CO5300 QSPI)", GPS_LCD_H_RES, GPS_LCD_V_RES);
    ESP_LOGI(TAG, "Mode: Direct LVGL integration (no BSP)");

    esp_err_t ret;

    /* 1. I2C bus (shared by touch, IMU, GPS, PMU) */
    ret = i2c_bus_init();
    if (ret != ESP_OK) return ret;

    /* 2. CO5300 QSPI AMOLED panel */
    ret = lcd_panel_init();
    if (ret != ESP_OK) return ret;

    /* 3. CST9217 touch controller */
    ret = touch_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Touch init failed — continuing without touch");
    }

    /* 4. LVGL display + buffers + tick */
    ret = lvgl_init_direct();
    if (ret != ESP_OK) return ret;

    /* 5. Boot button */
    boot_button_init();

    /* 6. Start touch reading task */
    xTaskCreatePinnedToCore(touch_read_task, "touch_read", 4096, NULL, 4, NULL, 0);

    /* 7. Start button reading task */
    xTaskCreatePinnedToCore(button_read_task, "button_task", 4096, NULL, 4,
                            &button_task_handle, 0);

    ESP_LOGI(TAG, "===== Display initialization complete =====");
    return ESP_OK;
}

i2c_master_bus_handle_t display_get_i2c_handle(void)
{
    return i2c_bus_handle;
}

bool display_lvgl_lock(uint32_t timeout_ms)
{
    if (lvgl_mux == NULL) return false;
    return xSemaphoreTake(lvgl_mux, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void display_lvgl_unlock(void)
{
    if (lvgl_mux != NULL) {
        xSemaphoreGive(lvgl_mux);
    }
}

lv_display_t *display_get_lvgl_disp(void)
{
    return lvgl_disp;
}

esp_err_t display_set_brightness(uint8_t brightness)
{
    if (brightness > 100) brightness = 100;

    /* CO5300 brightness via register 0x51 through QSPI command */
    uint8_t hw_val = (brightness * 255) / 100;
    uint32_t cmd = (LCD_OPCODE_WRITE_CMD << 24) | (0x51 << 8);

    esp_err_t ret = esp_lcd_panel_io_tx_param(io_handle, cmd, &hw_val, 1);
    if (ret == ESP_OK) {
        current_brightness = brightness;
        ESP_LOGI(TAG, "Brightness set to %d%% (reg=0x%02X)", brightness, hw_val);
    }
    return ret;
}

uint8_t display_get_brightness(void)
{
    return current_brightness;
}
