/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file display_init.h
 * @brief OpenDash Left/Right Gauges — Display Hardware Initialization
 *
 * Hardware: Waveshare ESP32-S3-Touch-LCD-2.8C (480×480 Round IPS LCD)
 * LCD Controller: ST7701S (3-wire 9-bit SPI init + RGB565 parallel interface)
 * IO Expander: TCA9554PWR (I2C) — controls LCD reset + SPI CS
 * Touch Controller: GT911 (I2C, shared bus with TCA9554)
 *
 * Pin mapping from official Waveshare ESP32-S3-LCD-2.8C Demo (ESP-IDF).
 */

#ifndef DISPLAY_INIT_H
#define DISPLAY_INIT_H

#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ────────────────────────────────────────────────────────────────────────────
 * LCD Resolution
 * ──────────────────────────────────────────────────────────────────────────── */
#define LCD_H_RES   480
#define LCD_V_RES   480

/* ────────────────────────────────────────────────────────────────────────────
 * I2C Master Bus — shared by TCA9554 + GT911 (+ QMI8658, PCF85063 if needed)
 *
 * Pin mapping from Waveshare demo: SDA=GPIO15, SCL=GPIO7, 400kHz
 * ──────────────────────────────────────────────────────────────────────────── */
#define I2C_MASTER_PORT     0
#define I2C_MASTER_SDA      GPIO_NUM_15
#define I2C_MASTER_SCL      GPIO_NUM_7
#define I2C_MASTER_FREQ_HZ  400000

/* ────────────────────────────────────────────────────────────────────────────
 * TCA9554PWR I2C IO Expander
 *
 * Controls LCD reset (EXIO1) and SPI CS (EXIO3).
 * Must be initialized BEFORE LCD SPI programming.
 * ──────────────────────────────────────────────────────────────────────────── */
#define TCA9554_I2C_ADDR        0x20
#define TCA9554_INPUT_REG       0x00
#define TCA9554_OUTPUT_REG      0x01
#define TCA9554_POLARITY_REG    0x02
#define TCA9554_CONFIG_REG      0x03
/* Pin assignments on TCA9554 (1-based: EXIO1=pin 1, EXIO2=pin 2, etc.)
 * Mapping from Waveshare ESP-IDF demo TCA9554PWR.h                     */
#define TCA9554_EXIO_LCD_RST    1       /* EXIO1 = LCD Reset */
#define TCA9554_EXIO_TP_RST     2       /* EXIO2 = GT911 Touch Reset */
#define TCA9554_EXIO_LCD_CS     3       /* EXIO3 = LCD SPI Chip Select */
#define TCA9554_EXIO_BUZZER     8       /* EXIO8 = Onboard buzzer (active HIGH) */

/* ────────────────────────────────────────────────────────────────────────────
 * ST7701S 3-Wire 9-Bit SPI Init Pins
 *
 * Uses ESP-IDF SPI master driver with command_bits=1 (D/CX) + address_bits=8.
 * CS is controlled via TCA9554 EXIO3 (no hardware CS pin).
 * SPI pins do NOT overlap with RGB data pins on this board.
 * ──────────────────────────────────────────────────────────────────────────── */
#define LCD_SPI_MOSI    GPIO_NUM_1
#define LCD_SPI_SCLK    GPIO_NUM_2

/* ────────────────────────────────────────────────────────────────────────────
 * RGB Interface Control Pins
 * ──────────────────────────────────────────────────────────────────────────── */
#define LCD_PIN_HSYNC   GPIO_NUM_38
#define LCD_PIN_VSYNC   GPIO_NUM_39
#define LCD_PIN_DE      GPIO_NUM_40
#define LCD_PIN_PCLK    GPIO_NUM_41

/* ────────────────────────────────────────────────────────────────────────────
 * RGB Data Pins — 16-bit RGB565
 * Pin mapping from Waveshare demo (verified against schematic).
 * ──────────────────────────────────────────────────────────────────────────── */
#define LCD_PIN_DATA0   GPIO_NUM_5      /* B0 */
#define LCD_PIN_DATA1   GPIO_NUM_45     /* B1 */
#define LCD_PIN_DATA2   GPIO_NUM_48     /* B2 */
#define LCD_PIN_DATA3   GPIO_NUM_47     /* B3 */
#define LCD_PIN_DATA4   GPIO_NUM_21     /* B4 */
#define LCD_PIN_DATA5   GPIO_NUM_14     /* G0 */
#define LCD_PIN_DATA6   GPIO_NUM_13     /* G1 */
#define LCD_PIN_DATA7   GPIO_NUM_12     /* G2 */
#define LCD_PIN_DATA8   GPIO_NUM_11     /* G3 */
#define LCD_PIN_DATA9   GPIO_NUM_10     /* G4 */
#define LCD_PIN_DATA10  GPIO_NUM_9      /* G5 */
#define LCD_PIN_DATA11  GPIO_NUM_46     /* R0 */
#define LCD_PIN_DATA12  GPIO_NUM_3      /* R1 */
#define LCD_PIN_DATA13  GPIO_NUM_8      /* R2 */
#define LCD_PIN_DATA14  GPIO_NUM_18     /* R3 */
#define LCD_PIN_DATA15  GPIO_NUM_17     /* R4 */

/* ────────────────────────────────────────────────────────────────────────────
 * Backlight — LEDC PWM on GPIO6
 * From Waveshare demo: EXAMPLE_PIN_NUM_BK_LIGHT = 6, active high.
 * ──────────────────────────────────────────────────────────────────────────── */
#define LCD_BL_GPIO     GPIO_NUM_6

/* ────────────────────────────────────────────────────────────────────────────
 * Touch Controller — GT911 (Capacitive)
 * Same I2C bus as TCA9554.
 * RST: TCA9554 EXIO2 (pulsed during touch_probe)
 * INT: Board-specific GPIO (may conflict with I2C slave SCL on GPIO16)
 * Address: 0x5D when INT LOW at reset, 0x14 when HIGH.
 *          Both addresses are tried during probe.
 * ──────────────────────────────────────────────────────────────────────────── */
#define TOUCH_I2C_ADDR_A    0x5D    /* GT911 address when INT=LOW at reset */
#define TOUCH_I2C_ADDR_B    0x14    /* GT911 address when INT=HIGH at reset */

/* ────────────────────────────────────────────────────────────────────────────
 * I2C Slave — Communication with Center unit
 * Uses I2C port 1 on available GPIOs (not used by display or SPI).
 * ──────────────────────────────────────────────────────────────────────────── */
#define I2C_SLAVE_PORT  1
#define I2C_SLAVE_SDA   GPIO_NUM_4
#define I2C_SLAVE_SCL   GPIO_NUM_16

/* ────────────────────────────────────────────────────────────────────────────
 * Boot Button — GPIO0 (shared with all ESP32-S3 boards)
 *
 * GPIO0 is NOT used by the 2.8" display's RGB data lines, so it is free
 * for the boot button. Active low, with internal pullup.
 * Used to cycle between display screens (same as center display).
 * ──────────────────────────────────────────────────────────────────────────── */
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define BOOT_BUTTON_DEBOUNCE_MS 50

/* ────────────────────────────────────────────────────────────────────────────
 * Public API
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Initialize all display hardware.
 *
 * Sequence: I2C bus → TCA9554 → LCD reset → SPI init → ST7701S regs →
 *           RGB panel → Backlight → GT911 probe → LVGL
 *
 * @return ESP_OK on success, error code on failure.
 */
esp_err_t display_init(void);

/**
 * @brief Get the I2C master bus handle (for adding more devices, e.g. RTC, IMU).
 *
 * Uses the new `driver/i2c_master.h` API — call
 * `i2c_master_bus_add_device(bus, &cfg, &dev)` with the returned handle.
 *
 * @return I2C master bus handle (NULL if not yet initialized).
 */
i2c_master_bus_handle_t display_get_i2c_handle(void);

/**
 * @brief Set display backlight brightness via LEDC PWM.
 * @param brightness 0–100 (0=off, 100=max)
 * @return ESP_OK on success.
 */
esp_err_t display_set_brightness(uint8_t brightness);

/**
 * @brief Power down the RGB panel + backlight for the BLE OTA window.
 *
 * Turns the backlight off and disables the RGB panel so its 18 MHz PCLK
 * DMA refresh stops hammering PSRAM / the memory bus. Frees CPU and bus
 * bandwidth for NimBLE + OTA flash writes. Not reversible — caller reboots
 * after OTA completes.
 */
void display_pause_for_ota(void);

/**
 * @brief Lock LVGL mutex for thread-safe access.
 * @param timeout_ms Max wait time.
 * @return true if acquired, false on timeout.
 */
bool display_lvgl_lock(uint32_t timeout_ms);

/**
 * @brief Unlock LVGL mutex.
 */
void display_lvgl_unlock(void);

/**
 * @brief Re-confirm TCA9554 IO expander outputs.
 *
 * Refreshes the TCA9554 output register to ensure EXIO1 (LCD RST)
 * stays HIGH.  Call periodically from the main loop as a safety net
 * against I2C bus glitches that could corrupt TCA9554 state and
 * cause the ST7701S display controller to reset unexpectedly.
 */
void display_refresh_io_state(void);

/* ── Buzzer API (TCA9554 EXIO8) ────────────────────────────────────── */

/** @brief Turn buzzer ON at full volume. */
void display_buzzer_on(void);

/** @brief Turn buzzer OFF. */
void display_buzzer_off(void);

/**
 * @brief Play a single beep.
 * @param duration_ms How long the beep sounds (50-1000ms typical).
 */
void display_buzzer_beep(uint32_t duration_ms);

/**
 * @brief Play a beep pattern.
 * @param count        Number of beeps.
 * @param on_ms        Duration of each beep (ms).
 * @param off_ms       Gap between beeps (ms).
 */
void display_buzzer_pattern(int count, uint32_t on_ms, uint32_t off_ms);

/** @brief 2 short beeps — successful boot. */
void display_buzzer_boot_ok(void);

/** @brief 3 rapid beeps — warning (I2C error, config fallback, etc.). */
void display_buzzer_warning(void);

/** @brief 1 long beep — critical error. */
void display_buzzer_error(void);

/* ── Boot Button Long-Press Callback ───────────────────────────────── */
typedef void (*button_long_press_cb_t)(void);
void display_register_long_press_cb(button_long_press_cb_t callback);

/**
 * @brief Start the boot-button polling task (GPIO0).
 *
 * Creates a FreeRTOS task that polls the boot button at 100 Hz.  On a
 * debounced short press the task calls ui_manager_next_screen() under the
 * LVGL lock.  On a long press (>= 5 s) it invokes the registered callback
 * (e.g. odometer reset) with buzzer feedback.
 *
 * Call this AFTER display_init() and ui_manager_start() so the LVGL
 * environment and screens are ready.
 */
void start_button_task(void);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_INIT_H */
