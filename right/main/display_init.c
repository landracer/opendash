/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file display_init.c
 * @brief OpenDash Left/Right Gauges — Display Hardware Initialization
 *
 * Hardware: Waveshare ESP32-S3-Touch-LCD-2.8C
 * LCD Controller: ST7701S (3-wire 9-bit SPI init via ESP-IDF SPI master,
 *                          then RGB565 parallel interface for pixels)
 * IO Expander: TCA9554PWR at I2C 0x20 — controls LCD Reset (EXIO1) and
 *              SPI CS (EXIO3)
 * Resolution: 480×480 Round IPS LCD (RGB565 16-bit color)
 *
 * Initialization sequence (from official Waveshare demo):
 *   1. I2C master bus (SDA=15, SCL=7, 400kHz) — new driver/i2c_master.h API
 *   2. TCA9554 IO expander init (all outputs)
 *   3. LCD hardware reset via TCA9554 EXIO1
 *   4. SPI CS enable via TCA9554 EXIO3
 *   5. SPI master init (MOSI=1, SCLK=2, 4MHz, command_bits=1, address_bits=8)
 *   6. ST7701S register programming via SPI
 *   7. RGB panel init (18MHz PCLK, correct timing from Waveshare demo)
 *   8. SPI CS disable via TCA9554 EXIO3
 *   9. LEDC backlight on GPIO6
 *  10. GT911 touch controller probe on same I2C bus
 *  11. LVGL 9.x with PSRAM draw buffer
 *
 * Pin mapping source: Official Waveshare ESP32-S3-LCD-2.8C Demo (ESP-IDF)
 */

#include "display_init.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"  /* New I2C master API (legacy driver/i2c.h is forbidden project-wide) */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "lvgl.h"

#include "ui_manager.h"   /* for ui_manager_next_screen() in boot button task */

static const char *TAG = "display_init";

/* ────────────────────────────────────────────────────────────────────────────
 * Constants — from Waveshare demo
 * ──────────────────────────────────────────────────────────────────────────── */
#define LCD_PIXEL_CLOCK_HZ      (18 * 1000 * 1000)  /* 18 MHz PCLK (Waveshare) */
#define LVGL_TICK_PERIOD_MS     2
#define LVGL_BUFFER_HEIGHT      48  /* legacy — DIRECT mode now uses HW framebuffers */
#define LCD_BOUNCE_BUFFER_SIZE  (20 * LCD_H_RES)  /* 9600 pixels = 20 lines */

/* LEDC backlight config (Waveshare: 4kHz, 13-bit resolution) */
#define BL_LEDC_TIMER           LEDC_TIMER_0
#define BL_LEDC_MODE            LEDC_LOW_SPEED_MODE
#define BL_LEDC_CHANNEL         LEDC_CHANNEL_0
#define BL_LEDC_DUTY_RES        LEDC_TIMER_13_BIT   /* 0-8191 */
#define BL_LEDC_FREQ_HZ         4000
#define BL_DEFAULT_PCT          70                   /* 70% default */

/* ────────────────────────────────────────────────────────────────────────────
 * Static handles
 * ──────────────────────────────────────────────────────────────────────────── */
static esp_lcd_panel_handle_t   s_panel       = NULL;
static SemaphoreHandle_t        s_lvgl_mux    = NULL;
static SemaphoreHandle_t        s_vsync_sem   = NULL;  /* Vsync sync for tear-free DIRECT-mode flushes */
static lv_display_t            *s_lvgl_disp   = NULL;
static spi_device_handle_t      s_spi_dev     = NULL;

/* New I2C master API handles — shared bus for TCA9554, GT911 (+ optional RTC/IMU) */
static i2c_master_bus_handle_t  s_i2c_bus     = NULL;
static i2c_master_dev_handle_t  s_tca_dev     = NULL;
static i2c_master_dev_handle_t  s_gt911_dev   = NULL;

/* Shadow register for TCA9554 output port.
 * The TCA9554 OUTPUT register readback is unreliable after RGB panel DMA starts
 * (reads 0x00 even when the register is set correctly).  Using a shadow variable
 * avoids destructive read-modify-write cycles that clear EXIO1 (LCD RST) when
 * the buzzer toggles EXIO8. */
static uint8_t s_tca_output_shadow = 0x00;

/* ═══════════════════════════════════════════════════════════════════════════
 * Section 1 — I2C Master Bus (new driver/i2c_master.h API)
 *
 * Per project rule: no legacy driver/i2c.h anywhere in shipping code.
 *
 * Prior migration attempts produced NACKs on TCA9554 writes; the root cause
 * was missing `flags.enable_internal_pullup` + missing `glitch_ignore_cnt`
 * + per-device `scl_speed_hz` left at default. Both pull-ups (on-board
 * 4.7kΩ AND internal) plus a glitch filter make the bus reliable on this
 * Waveshare ESP32-S3-Touch-LCD-2.8C.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define I2C_TIMEOUT_MS    1000

static esp_err_t i2c_master_bus_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .i2c_port                     = I2C_MASTER_PORT,
        .sda_io_num                   = I2C_MASTER_SDA,
        .scl_io_num                   = I2C_MASTER_SCL,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "I2C master bus initialized (SDA=%d, SCL=%d, %dkHz, new API)",
             I2C_MASTER_SDA, I2C_MASTER_SCL, I2C_MASTER_FREQ_HZ / 1000);
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Section 2 — TCA9554PWR IO Expander (new driver/i2c_master.h API)
 *
 * The TCA9554 controls:
 *   EXIO1 (bit 0) = LCD Reset (active low pulse)
 *   EXIO3 (bit 2) = LCD SPI CS (active low)
 *
 * Address is 7-bit (0x20) — NOT pre-shifted; the new API does shifting + R/W bit
 * internally. `scl_speed_hz=400000` matches the TCA9554 Fast-mode rating.
 * ═══════════════════════════════════════════════════════════════════════════ */

static esp_err_t tca9554_write_reg(uint8_t reg, uint8_t data)
{
    if (s_tca_dev == NULL) return ESP_ERR_INVALID_STATE;
    uint8_t buf[2] = { reg, data };
    esp_err_t ret = i2c_master_transmit(s_tca_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "TCA9554 write reg 0x%02X=0x%02X FAILED: %s", reg, data, esp_err_to_name(ret));
    }
    return ret;
}

static uint8_t tca9554_read_reg(uint8_t reg)
{
    if (s_tca_dev == NULL) return 0xFF;
    uint8_t data = 0xFF;    /* init to 0xFF so failed reads are distinguishable */
    esp_err_t ret = i2c_master_transmit_receive(s_tca_dev, &reg, 1, &data, 1, I2C_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "TCA9554 read reg 0x%02X FAILED: %s (data=0x%02X)", reg, esp_err_to_name(ret), data);
    }
    return data;
}

static void tca9554_set_pin(uint8_t pin, bool level)
{
    /* pin is 1-based (EXIO1=1 .. EXIO8=8).
     * Uses shadow register — never reads OUTPUT back from TCA9554
     * because readback is unreliable after RGB panel DMA starts. */
    if (level) {
        s_tca_output_shadow |= (1 << (pin - 1));
    } else {
        s_tca_output_shadow &= ~(1 << (pin - 1));
    }
    tca9554_write_reg(TCA9554_OUTPUT_REG, s_tca_output_shadow);
}

static esp_err_t tca9554_init(void)
{
    /* Attach TCA9554 as a device on the new-API bus before any transaction. */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = TCA9554_I2C_ADDR,    /* 0x20, unshifted */
        .scl_speed_hz    = 400000,              /* 400kHz — TCA9554 Fast mode */
    };
    esp_err_t add_ret = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_tca_dev);
    if (add_ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device(TCA9554) failed: %s", esp_err_to_name(add_ret));
        return add_ret;
    }

    /* CRITICAL: The TCA9554 can be flaky on the first few I2C transactions
     * after power-on.  We retry each step and verify with readback.
     *
     * CRITICAL ORDER: Write OUTPUT *before* CONFIG!
     * At power-on, CONFIG=0xFF (all inputs) and OUTPUT=0x00 (all LOW).
     * If we write CONFIG=0x00 first, all pins start driving LOW.
     * If any unused pin is connected to SDA/SCL, this kills I2C.
     *
     * We set unused pins (P3-P6) HIGH as a safety measure.
     */

    /* Step 1: Preload output latch with safe values WHILE pins are still inputs.
     * EXIO1 (LCD RST)  = LOW  (bit 0) — will be toggled during hardware reset
     * EXIO2 (GT911 RST)= LOW  (bit 1) — keep touch controller in reset
     * EXIO3 (LCD CS)    = HIGH (bit 2) — CS inactive
     * EXIO4–7 (unused)  = HIGH (bits 3-6) — safe for any bus connection
     * EXIO8 (buzzer)    = LOW  (bit 7) — off
     */
    s_tca_output_shadow = (1 << (TCA9554_EXIO_LCD_CS - 1))    /* bit 2 */
                        | (1 << 3) | (1 << 4) | (1 << 5) | (1 << 6);  /* P3-P6 */

    /* Retry OUTPUT write up to 10 times */
    esp_err_t ret;
    for (int i = 0; i < 10; i++) {
        ret = tca9554_write_reg(TCA9554_OUTPUT_REG, s_tca_output_shadow);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "TCA9554 OUTPUT latch set to 0x%02X on attempt %d", s_tca_output_shadow, i + 1);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    /* Small delay between register writes */
    vTaskDelay(pdMS_TO_TICKS(5));

    /* Step 2: Set all pins as outputs — output latch already has safe values.
     * Retry up to 10 times with READBACK verification (don't trust return code). */
    for (int i = 0; i < 10; i++) {
        tca9554_write_reg(TCA9554_CONFIG_REG, 0x00);
        vTaskDelay(pdMS_TO_TICKS(5));

        uint8_t cfg_rb = tca9554_read_reg(TCA9554_CONFIG_REG);
        if (cfg_rb == 0x00) {
            ESP_LOGI(TAG, "TCA9554 CONFIG set to 0x00 on attempt %d", i + 1);
            break;
        }
        ESP_LOGW(TAG, "TCA9554 CONFIG readback 0x%02X (want 0x00), attempt %d", cfg_rb, i + 1);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    /* Step 3: Verify actual pin levels */
    uint8_t inp_check = tca9554_read_reg(TCA9554_INPUT_REG);
    uint8_t cfg_check = tca9554_read_reg(TCA9554_CONFIG_REG);
    ESP_LOGI(TAG, "TCA9554 post-init: CONFIG=0x%02X INPUT=0x%02X shadow=0x%02X",
             cfg_check, inp_check, s_tca_output_shadow);

    ESP_LOGI(TAG, "TCA9554 IO expander initialized at 0x%02X", TCA9554_I2C_ADDR);
    return ESP_OK;
}

static void lcd_hardware_reset(void)
{
    /* Toggle EXIO1 (LCD_RST): low → wait → high → wait */
    tca9554_set_pin(TCA9554_EXIO_LCD_RST, false);
    vTaskDelay(pdMS_TO_TICKS(10));
    tca9554_set_pin(TCA9554_EXIO_LCD_RST, true);
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_LOGI(TAG, "LCD hardware reset via TCA9554 EXIO1");
}

static void lcd_spi_cs_enable(void)
{
    tca9554_set_pin(TCA9554_EXIO_LCD_CS, false);  /* CS active low */
    vTaskDelay(pdMS_TO_TICKS(10));
}

static void lcd_spi_cs_disable(void)
{
    tca9554_set_pin(TCA9554_EXIO_LCD_CS, true);   /* CS inactive */
    vTaskDelay(pdMS_TO_TICKS(10));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Section 3 — ST7701S SPI Init (ESP-IDF SPI master driver)
 *
 * Uses the proper hardware SPI with 1-bit command (D/CX) + 8-bit address.
 * This implements the 3-wire 9-bit SPI protocol required by ST7701S.
 * CS is controlled externally via TCA9554 EXIO3.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void spi_cmd(uint8_t cmd)
{
    spi_transaction_t t = {
        .cmd    = 0,        /* D/CX = 0 → command */
        .addr   = cmd,      /* The command byte */
        .length = 0,        /* No additional payload */
    };
    spi_device_transmit(s_spi_dev, &t);
}

static void spi_data(uint8_t data)
{
    spi_transaction_t t = {
        .cmd    = 1,        /* D/CX = 1 → data/parameter */
        .addr   = data,     /* The data byte */
        .length = 0,
    };
    spi_device_transmit(s_spi_dev, &t);
}

static esp_err_t spi_master_init(void)
{
    spi_bus_config_t bus_cfg = {
        .miso_io_num   = -1,
        .mosi_io_num   = LCD_SPI_MOSI,
        .sclk_io_num   = LCD_SPI_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SOC_SPI_MAXIMUM_BUFFER_SIZE,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev_cfg = {
        .command_bits   = 1,        /* D/CX bit */
        .address_bits   = 8,        /* Payload byte */
        .clock_speed_hz = 4000000,  /* 4 MHz SPI clock */
        .mode           = 0,        /* CPOL=0, CPHA=0 */
        .spics_io_num   = -1,       /* CS via TCA9554, not HW */
        .queue_size     = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev_cfg, &s_spi_dev));

    ESP_LOGI(TAG, "SPI master initialized (MOSI=%d, SCLK=%d, 4MHz)",
             LCD_SPI_MOSI, LCD_SPI_SCLK);
    return ESP_OK;
}

/**
 * @brief Program all ST7701S registers.
 *
 * Command sequence from official Waveshare ESP32-S3-LCD-2.8C Demo.
 * This is the exact proven init for the 480×480 round ST7701S panel.
 */
static esp_err_t st7701s_init_registers(void)
{
    ESP_LOGI(TAG, "Programming ST7701S registers via SPI master");

    /* ── Page 3: Extended (preamble) ─────────────────────────────────────── */
    spi_cmd(0xFF);
    spi_data(0x77); spi_data(0x01); spi_data(0x00);
    spi_data(0x00); spi_data(0x13);

    spi_cmd(0xEF); spi_data(0x08);

    /* ── Page 1: Display Timing ──────────────────────────────────────────── */
    spi_cmd(0xFF);
    spi_data(0x77); spi_data(0x01); spi_data(0x00);
    spi_data(0x00); spi_data(0x10);

    spi_cmd(0xC0); spi_data(0x3B); spi_data(0x00);   /* LNESET: 480 lines */
    spi_cmd(0xC1); spi_data(0x10); spi_data(0x0C);   /* PORCTRL */
    spi_cmd(0xC2); spi_data(0x07); spi_data(0x0A);   /* INVSEL */
    spi_cmd(0xC7); spi_data(0x00);
    spi_cmd(0xCC); spi_data(0x10);
    spi_cmd(0xCD); spi_data(0x08);

    /* Positive Gamma */
    spi_cmd(0xB0);
    spi_data(0x05); spi_data(0x12); spi_data(0x98); spi_data(0x0E);
    spi_data(0x0F); spi_data(0x07); spi_data(0x07); spi_data(0x09);
    spi_data(0x09); spi_data(0x23); spi_data(0x05); spi_data(0x52);
    spi_data(0x0F); spi_data(0x67); spi_data(0x2C); spi_data(0x11);

    /* Negative Gamma */
    spi_cmd(0xB1);
    spi_data(0x0B); spi_data(0x11); spi_data(0x97); spi_data(0x0C);
    spi_data(0x12); spi_data(0x06); spi_data(0x06); spi_data(0x08);
    spi_data(0x08); spi_data(0x22); spi_data(0x03); spi_data(0x51);
    spi_data(0x11); spi_data(0x66); spi_data(0x2B); spi_data(0x0F);

    /* ── Page 2: Power Control ───────────────────────────────────────────── */
    spi_cmd(0xFF);
    spi_data(0x77); spi_data(0x01); spi_data(0x00);
    spi_data(0x00); spi_data(0x11);

    spi_cmd(0xB0); spi_data(0x5D);
    spi_cmd(0xB1); spi_data(0x3E);
    spi_cmd(0xB2); spi_data(0x81);
    spi_cmd(0xB3); spi_data(0x80);
    spi_cmd(0xB5); spi_data(0x4E);
    spi_cmd(0xB7); spi_data(0x85);
    spi_cmd(0xB8); spi_data(0x20);
    spi_cmd(0xC1); spi_data(0x78);
    spi_cmd(0xC2); spi_data(0x78);
    spi_cmd(0xD0); spi_data(0x88);

    spi_cmd(0xE0);
    spi_data(0x00); spi_data(0x00); spi_data(0x02);

    spi_cmd(0xE1);
    spi_data(0x06); spi_data(0x30); spi_data(0x08); spi_data(0x30);
    spi_data(0x05); spi_data(0x30); spi_data(0x07); spi_data(0x30);
    spi_data(0x00); spi_data(0x33); spi_data(0x33);

    spi_cmd(0xE2);
    spi_data(0x11); spi_data(0x11); spi_data(0x33); spi_data(0x33);
    spi_data(0xF4); spi_data(0x00); spi_data(0x00); spi_data(0x00);
    spi_data(0xF4); spi_data(0x00); spi_data(0x00); spi_data(0x00);

    spi_cmd(0xE3);
    spi_data(0x00); spi_data(0x00); spi_data(0x11); spi_data(0x11);

    spi_cmd(0xE4);
    spi_data(0x44); spi_data(0x44);

    spi_cmd(0xE5);
    spi_data(0x0D); spi_data(0xF5); spi_data(0x30); spi_data(0xF0);
    spi_data(0x0F); spi_data(0xF7); spi_data(0x30); spi_data(0xF0);
    spi_data(0x09); spi_data(0xF1); spi_data(0x30); spi_data(0xF0);
    spi_data(0x0B); spi_data(0xF3); spi_data(0x30); spi_data(0xF0);

    spi_cmd(0xE6);
    spi_data(0x00); spi_data(0x00); spi_data(0x11); spi_data(0x11);

    spi_cmd(0xE7);
    spi_data(0x44); spi_data(0x44);

    spi_cmd(0xE8);
    spi_data(0x0C); spi_data(0xF4); spi_data(0x30); spi_data(0xF0);
    spi_data(0x0E); spi_data(0xF6); spi_data(0x30); spi_data(0xF0);
    spi_data(0x08); spi_data(0xF0); spi_data(0x30); spi_data(0xF0);
    spi_data(0x0A); spi_data(0xF2); spi_data(0x30); spi_data(0xF0);

    spi_cmd(0xE9);
    spi_data(0x36); spi_data(0x01);

    spi_cmd(0xEB);
    spi_data(0x00); spi_data(0x01); spi_data(0xE4); spi_data(0xE4);
    spi_data(0x44); spi_data(0x88); spi_data(0x40);

    spi_cmd(0xED);
    spi_data(0xFF); spi_data(0x10); spi_data(0xAF); spi_data(0x76);
    spi_data(0x54); spi_data(0x2B); spi_data(0xCF); spi_data(0xFF);
    spi_data(0xFF); spi_data(0xFC); spi_data(0xB2); spi_data(0x45);
    spi_data(0x67); spi_data(0xFA); spi_data(0x01); spi_data(0xFF);

    spi_cmd(0xEF);
    spi_data(0x08); spi_data(0x08); spi_data(0x08); spi_data(0x45);
    spi_data(0x3F); spi_data(0x54);

    /* ── User Command Page ───────────────────────────────────────────────── */
    spi_cmd(0xFF);
    spi_data(0x77); spi_data(0x01); spi_data(0x00);
    spi_data(0x00); spi_data(0x00);

    spi_cmd(0x11);  /* Sleep Out */
    vTaskDelay(pdMS_TO_TICKS(120));

    spi_cmd(0x3A); spi_data(0x66);  /* COLMOD: RGB666 (Waveshare demo value) */
    spi_cmd(0x36); spi_data(0x00);  /* MADCTL: Normal orientation */
    spi_cmd(0x35); spi_data(0x00);  /* Tearing Effect Line ON */

    spi_cmd(0x29);  /* Display On */
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_LOGI(TAG, "ST7701S register programming complete");
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Section 4 — Backlight (LEDC PWM on GPIO6)
 * ═══════════════════════════════════════════════════════════════════════════ */

static esp_err_t backlight_init(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = BL_LEDC_MODE,
        .duty_resolution = BL_LEDC_DUTY_RES,
        .timer_num       = BL_LEDC_TIMER,
        .freq_hz         = BL_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    ledc_channel_config_t ch_cfg = {
        .speed_mode = BL_LEDC_MODE,
        .channel    = BL_LEDC_CHANNEL,
        .timer_sel  = BL_LEDC_TIMER,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = LCD_BL_GPIO,
        .duty       = 0,       /* Start off, set later */
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));

    /* Set default brightness */
    display_set_brightness(BL_DEFAULT_PCT);

    ESP_LOGI(TAG, "Backlight PWM initialized on GPIO%d (%d%%)",
             LCD_BL_GPIO, BL_DEFAULT_PCT);
    return ESP_OK;
}

esp_err_t display_set_brightness(uint8_t brightness)
{
    if (brightness > 100) brightness = 100;
    /* 13-bit resolution: max = 8191 */
    uint32_t duty = (uint32_t)brightness * 8191 / 100;
    ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty);
    ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
    return ESP_OK;
}

void display_pause_for_ota(void)
{
    display_set_brightness(0);
    if (s_panel) {
        /* disp_on_off is a no-op when disp_gpio_num=-1 — the RGB DMA bounce
         * engine keeps hammering PSRAM on CPU0, starving the NimBLE controller
         * that's also pinned to CPU0. Tear the panel down to actually stop it. */
        esp_lcd_panel_del(s_panel);
        s_panel = NULL;
    }
    ESP_LOGW(TAG, "Display paused for BLE OTA (panel torn down, RGB DMA halted)");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Section 5 — RGB LCD Panel (correct pins + timing from Waveshare demo)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Forward declaration — defined in Section 7 (LVGL) but needed for vsync CB registration */
static bool on_vsync_event(esp_lcd_panel_handle_t panel,
                            const esp_lcd_rgb_panel_event_data_t *edata,
                            void *user_ctx);

static esp_err_t rgb_panel_init(void)
{
    ESP_LOGI(TAG, "Creating RGB LCD panel (480×480, %d MHz PCLK)",
             LCD_PIXEL_CLOCK_HZ / 1000000);

    esp_lcd_rgb_panel_config_t panel_cfg = {
        .clk_src  = LCD_CLK_SRC_DEFAULT,
        .timings  = {
            .pclk_hz            = LCD_PIXEL_CLOCK_HZ,
            .h_res              = LCD_H_RES,
            .v_res              = LCD_V_RES,
            .hsync_pulse_width  = 8,
            .hsync_back_porch   = 10,
            .hsync_front_porch  = 50,
            .vsync_pulse_width  = 2,
            .vsync_back_porch   = 18,
            .vsync_front_porch  = 8,
        },
        .data_width     = 16,
        .num_fbs        = 2,  /* Double framebuffer for tear-free vsync rendering (TODO #12 parity with left) */
        .bounce_buffer_size_px = LCD_BOUNCE_BUFFER_SIZE,  /* 20-line bounce buffer — prevents PSRAM DMA contention artifacts */
        .hsync_gpio_num = LCD_PIN_HSYNC,
        .vsync_gpio_num = LCD_PIN_VSYNC,
        .de_gpio_num    = LCD_PIN_DE,
        .pclk_gpio_num  = LCD_PIN_PCLK,
        .disp_gpio_num  = -1,
        .data_gpio_nums = {
            LCD_PIN_DATA0,  LCD_PIN_DATA1,  LCD_PIN_DATA2,  LCD_PIN_DATA3,
            LCD_PIN_DATA4,  LCD_PIN_DATA5,  LCD_PIN_DATA6,  LCD_PIN_DATA7,
            LCD_PIN_DATA8,  LCD_PIN_DATA9,  LCD_PIN_DATA10, LCD_PIN_DATA11,
            LCD_PIN_DATA12, LCD_PIN_DATA13, LCD_PIN_DATA14, LCD_PIN_DATA15,
        },
    };
    panel_cfg.timings.flags.pclk_active_neg = false;
    panel_cfg.flags.fb_in_psram = true;

    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_cfg, &s_panel));
    ESP_LOGI(TAG, "RGB panel created");

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_LOGI(TAG, "RGB panel initialized");

    /* Register vsync callback for tear-free rendering */
    s_vsync_sem = xSemaphoreCreateBinary();
    esp_lcd_rgb_panel_event_callbacks_t cbs = {
        .on_vsync = on_vsync_event,
    };
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(s_panel, &cbs, NULL));
    ESP_LOGI(TAG, "Vsync callback registered (double-buffer tear-free mode)");

    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Section 6 — GT911 Touch Controller Probe
 *
 * The GT911 requires a hardware reset via TCA9554 EXIO2 before it will
 * respond to I2C probes.  Without this, the probe always returns NACK.
 *
 * Address selection (0x5D vs 0x14) depends on the INT pin state during
 * the reset rising edge.  We try both addresses after reset.
 *
 * NOTE: Full touch input device registration with LVGL is a future TODO.
 *       For now, we just detect the chip and log the result.
 * ═══════════════════════════════════════════════════════════════════════════ */

static esp_err_t touch_probe(void)
{
    /*
     * GT911 touch is NOT used yet (no LVGL input device registered).
     * SKIP the reset pulse entirely — waking GT911 causes it to drive
     * SDA on the I2C master bus (GPIO15), which makes the bus stuck
     * for >1 second and can corrupt TCA9554 output register (EXIO1 LOW
     * = ST7701S reset = blank display).
     *
     * tca9554_init() already set EXIO2 = LOW (all outputs start LOW),
     * so GT911 is held in permanent reset and cannot touch the I2C bus.
     *
     * TODO: When touch support is implemented, carefully manage GT911
     *       reset timing and verify I2C bus health after probe.
     */
    ESP_LOGI(TAG, "GT911 touch: SKIPPED (held in reset via EXIO2=LOW)");
    ESP_LOGI(TAG, "  Touch not used yet — avoids I2C bus corruption");
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Section 7 — LVGL Integration
 * ═══════════════════════════════════════════════════════════════════════════ */

static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

/**
 * @brief VSYNC event callback — ISR context.
 * Unblocks the flush callback so LVGL only writes to the back framebuffer.
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
 */
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel = lv_display_get_user_data(disp);
    esp_lcd_panel_draw_bitmap(panel,
        area->x1, area->y1,
        area->x2 + 1, area->y2 + 1,
        px_map);
    /* Wait for vsync before returning — prevents writing to the displayed buffer */
    xSemaphoreTake(s_vsync_sem, pdMS_TO_TICKS(100));
    lv_display_flush_ready(disp);
}

static esp_err_t lvgl_init_internal(void)
{
    ESP_LOGI(TAG, "Initializing LVGL");

    lv_init();

    s_lvgl_mux = xSemaphoreCreateMutex();
    if (!s_lvgl_mux) {
        ESP_LOGE(TAG, "Failed to create LVGL mutex");
        return ESP_FAIL;
    }

    s_lvgl_disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    if (!s_lvgl_disp) {
        ESP_LOGE(TAG, "Failed to create LVGL display");
        return ESP_FAIL;
    }
    lv_display_set_color_format(s_lvgl_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_lvgl_disp, lvgl_flush_cb);
    lv_display_set_user_data(s_lvgl_disp, s_panel);

    /* Get the two hardware framebuffers for direct rendering */
    void *fb0 = NULL, *fb1 = NULL;
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(s_panel, 2, &fb0, &fb1));
    size_t fb_size = LCD_H_RES * LCD_V_RES * sizeof(uint16_t);
    lv_display_set_buffers(s_lvgl_disp, fb0, fb1, fb_size,
                           LV_DISPLAY_RENDER_MODE_DIRECT);

    const esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .name     = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer,
                                             LVGL_TICK_PERIOD_MS * 1000));

    ESP_LOGI(TAG, "LVGL initialized (direct mode, 2\u00d7%zuKB framebuffers)",
             fb_size / 1024);
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Section 8 — Public API
 * ═══════════════════════════════════════════════════════════════════════════ */

i2c_master_bus_handle_t display_get_i2c_handle(void)
{
    return s_i2c_bus;
}

bool display_lvgl_lock(uint32_t timeout_ms)
{
    if (!s_lvgl_mux) return false;
    return xSemaphoreTake(s_lvgl_mux, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void display_lvgl_unlock(void)
{
    if (s_lvgl_mux) {
        xSemaphoreGive(s_lvgl_mux);
    }
}

void display_refresh_io_state(void)
{
    /* Re-write TCA9554 CONFIG (all outputs) AND output register from shadow.
     * Guards against I2C bus glitches that corrupt TCA9554 registers.
     * CONFIG reverting to 0xFF (all inputs) causes EXIO1 to float → 
     * ST7701S reset → blank display.
     * Re-affirm both registers every call (every 5 seconds from main loop).
     * The shadow preserves buzzer state so we don't accidentally toggle it. */
    tca9554_write_reg(TCA9554_CONFIG_REG, 0x00);
    tca9554_write_reg(TCA9554_OUTPUT_REG, s_tca_output_shadow);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Buzzer — TCA9554 EXIO8 (onboard piezo buzzer, active HIGH)
 *
 * The Waveshare ESP32-S3-Touch-LCD-2.8C has an onboard buzzer connected
 * to TCA9554 pin P7 (EXIO8).  Setting the pin HIGH turns the buzzer ON.
 * ═══════════════════════════════════════════════════════════════════════════ */

void display_buzzer_on(void)
{
    tca9554_set_pin(TCA9554_EXIO_BUZZER, true);
}

void display_buzzer_off(void)
{
    tca9554_set_pin(TCA9554_EXIO_BUZZER, false);
}

void display_buzzer_beep(uint32_t duration_ms)
{
    display_buzzer_on();
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    display_buzzer_off();
}

void display_buzzer_pattern(int count, uint32_t on_ms, uint32_t off_ms)
{
    for (int i = 0; i < count; i++) {
        display_buzzer_on();
        vTaskDelay(pdMS_TO_TICKS(on_ms));
        display_buzzer_off();
        if (i < count - 1) {
            vTaskDelay(pdMS_TO_TICKS(off_ms));
        }
    }
}

void display_buzzer_boot_ok(void)
{
    /* 2 short beeps = clean boot */
    ESP_LOGI(TAG, "BUZZER: Boot OK (2 beeps)");
    display_buzzer_pattern(2, 80, 120);
}

void display_buzzer_warning(void)
{
    /* 3 rapid beeps = warning */
    ESP_LOGW(TAG, "BUZZER: Warning (3 beeps)");
    display_buzzer_pattern(3, 50, 80);
}

void display_buzzer_error(void)
{
    /* 1 long beep = critical error */
    ESP_LOGE(TAG, "BUZZER: Error (1 long beep)");
    display_buzzer_beep(500);
}

esp_err_t display_init(void)
{
    ESP_LOGI(TAG, "══════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "Display init: 480×480 Round IPS (ST7701S + TCA9554)");
    ESP_LOGI(TAG, "══════════════════════════════════════════════════════");

    /* Step 1: I2C master bus for TCA9554 and GT911 */
    ESP_ERROR_CHECK(i2c_master_bus_init());

    /* Step 2: TCA9554 IO expander */
    ESP_ERROR_CHECK(tca9554_init());

    /* Step 3: LCD hardware reset via TCA9554 EXIO1 */
    lcd_hardware_reset();

    /* Step 4: SPI CS enable via TCA9554 EXIO3 */
    lcd_spi_cs_enable();
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Step 5: SPI master init */
    ESP_ERROR_CHECK(spi_master_init());

    /* Step 6: Program ST7701S registers */
    ESP_ERROR_CHECK(st7701s_init_registers());

    /* Step 7: RGB panel init */
    ESP_ERROR_CHECK(rgb_panel_init());

    /* Critical: Allow RGB DMA to stabilize.
     * RGB panel DMA (PSRAM→LCD at 18MHz) starting above can cause a
     * transient that resets the TCA9554, reverting CONFIG to 0xFF
     * (all inputs) and EXIO1 (LCD RST) floats → ST7701S reset.
     * A 500ms delay lets the power supply and bus settle.            */
    ESP_LOGI(TAG, "Waiting for RGB DMA to stabilize...");
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Step 8: SPI CS disable (done with LCD SPI programming) */
    lcd_spi_cs_disable();

    /* Step 9: Backlight */
    ESP_ERROR_CHECK(backlight_init());

    /* Step 10: GT911 touch probe */
    touch_probe();

    /* Step 10b: Re-confirm TCA9554 state after RGB panel DMA start.
     * RGB panel DMA can cause the TCA9554 CONFIG register to revert
     * to its power-on default (0xFF = all inputs), making EXIO1 (LCD RST)
     * float → ST7701S reset → blank display.
     *
     * We now use a shadow register for OUTPUT, so we don't rely on readback.
     * Write CONFIG=0x00 (all outputs) + commit the shadow (expected pins).
     * Retry a few times with CONFIG readback to confirm the CONFIG register
     * is correct; OUTPUT readback is known to be unreliable.              */
    uint8_t expected_out = (1 << (TCA9554_EXIO_LCD_RST - 1))    /* bit 0 = EXIO1 HIGH */
                         | (1 << (TCA9554_EXIO_LCD_CS - 1));     /* bit 2 = EXIO3 HIGH */
    s_tca_output_shadow = expected_out;    /* sync shadow */

    bool config_ok = false;
    for (int attempt = 0; attempt < 10; attempt++) {
        tca9554_write_reg(TCA9554_CONFIG_REG, 0x00);
        tca9554_write_reg(TCA9554_OUTPUT_REG, s_tca_output_shadow);

        /* Read CONFIG to verify — OUTPUT readback is unreliable, skip it */
        uint8_t cfg_rb = tca9554_read_reg(TCA9554_CONFIG_REG);
        uint8_t inp_rb = tca9554_read_reg(TCA9554_INPUT_REG);   /* actual pin levels */

        if (cfg_rb == 0x00) {
            ESP_LOGI(TAG, "TCA9554 CONFIG OK on attempt %d (CONFIG=0x%02X, INPUT=0x%02X, shadow=0x%02X)",
                     attempt + 1, cfg_rb, inp_rb, s_tca_output_shadow);
            config_ok = true;
            break;
        }
        ESP_LOGW(TAG, "TCA9554 attempt %d: CONFIG=0x%02X INPUT=0x%02X (want CONFIG=0x00)",
                 attempt + 1, cfg_rb, inp_rb);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (!config_ok) {
        ESP_LOGE(TAG, "TCA9554 CONFIG could not be set to 0x00 after 10 attempts!");
        ESP_LOGE(TAG, "  ST7701S may have been reset — re-programming...");
    }

    /* Always re-program ST7701S if CONFIG was ever wrong (EXIO1 may have floated).
     * Also write CONFIG + OUTPUT one final time after reprogramming. */
    ESP_LOGI(TAG, "Re-programming ST7701S registers (safety)...");
    lcd_spi_cs_enable();
    st7701s_init_registers();
    lcd_spi_cs_disable();
    /* Final commit of shadow output state */
    tca9554_write_reg(TCA9554_CONFIG_REG, 0x00);
    tca9554_write_reg(TCA9554_OUTPUT_REG, s_tca_output_shadow);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Step 11: LVGL */
    ESP_ERROR_CHECK(lvgl_init_internal());

    ESP_LOGI(TAG, "Display initialization complete — ready for UI");
    return ESP_OK;
}

/* ────────────────────────────────────────────────────────────────────────────
 * Boot Button — GPIO0
 *
 * The ESP32-S3 boot button is on GPIO0 (active-low, internal pullup).
 * We poll it at 100 Hz with a simple debounce counter.  On a confirmed press
 * we cycle to the next display screen, exactly as the center display does.
 *
 * Why polling instead of interrupt?
 *   - Simpler, no need for ISR-safe queues
 *   - 10 ms period + 50 ms debounce = clean, jitter-free detection
 *   - Same pattern used by /center, proven reliable
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Configure GPIO0 as input with internal pull-up for boot button.
 */
static void boot_button_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_LOGI(TAG, "Boot button configured on GPIO%d", BOOT_BUTTON_GPIO);
}

/* Long-press callback (set from main.c for odometer reset) */
static button_long_press_cb_t s_long_press_cb = NULL;

void display_register_long_press_cb(button_long_press_cb_t callback)
{
    s_long_press_cb = callback;
}

/**
 * @brief FreeRTOS task — polls boot button at 100 Hz.
 *
 * Short press (< 5s): cycles display screen.
 * Long press (>= 5s): triggers long_press_cb (odometer reset).
 *
 * Debouncing: the button must be held low for BOOT_BUTTON_DEBOUNCE_MS
 * before registering a press.  The press fires once per push (edge, not
 * level), and the button must be released before it can fire again.
 */
static void button_read_task(void *arg)
{
    (void)arg;
    const int debounce_ticks = BOOT_BUTTON_DEBOUNCE_MS / 10;  /* 50/10 = 5 */
    const int long_press_ticks = 5000 / 10;  /* 5 seconds / 10ms = 500 ticks */
    int       held_count     = 0;
    bool      was_pressed    = false;
    bool      long_press_fired = false;

    for (;;) {
        /* GPIO0 = 0 when pressed (active low) */
        int level = gpio_get_level(BOOT_BUTTON_GPIO);

        if (level == 0) {
            /* Button is held down — increment counter */
            held_count++;

            /* Long-press detection (5 seconds) — fires once */
            if (held_count >= long_press_ticks && !long_press_fired) {
                long_press_fired = true;
                ESP_LOGW(TAG, "Boot button LONG PRESS (5s) — odometer reset");
                /* Buzzer feedback: 3 quick beeps */
                display_buzzer_pattern(3, 100, 100);
                if (s_long_press_cb) {
                    s_long_press_cb();
                }
            }
        } else {
            /* Button released */
            if (held_count >= debounce_ticks && !long_press_fired && !was_pressed) {
                /* Short press confirmed — switch screen */
                was_pressed = true;
                ESP_LOGI(TAG, "Boot button pressed — switching screen");
                if (display_lvgl_lock(100)) {
                    ui_manager_next_screen();
                    display_lvgl_unlock();
                }
            }
            /* Reset all state on release */
            held_count  = 0;
            was_pressed = false;
            long_press_fired = false;
        }

        vTaskDelay(pdMS_TO_TICKS(10));   /* 100 Hz poll rate */
    }
}

/**
 * @brief Create the boot-button polling task.
 *
 * Call AFTER display_init() and ui_manager_start() so LVGL screens exist.
 */
void start_button_task(void)
{
    boot_button_init();
    xTaskCreatePinnedToCore(
        button_read_task,       /* task function      */
        "boot_btn",             /* name               */
        2048,                   /* stack (bytes)      */
        NULL,                   /* parameter          */
        2,                      /* priority           */
        NULL,                   /* task handle (unused) */
        0                       /* core 0             */
    );
    ESP_LOGI(TAG, "Boot button task started on core 0");
}
