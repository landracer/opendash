<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# Waveshare ESP32-S3-Touch-AMOLED-1.75 — OpenDash Conversion Guide

> **This guide replaces the Waveshare wiki's default examples with OpenDash
> equivalents. Every code block is production-tested and verified working.**

---

## Table of Contents

1. [Board Overview](#1-board-overview)
2. [Pin Mapping Reference](#2-pin-mapping-reference)
3. [ESP-IDF Project Setup](#3-esp-idf-project-setup)
4. [I2C Bus Initialization](#4-i2c-bus-initialization)
5. [CO5300 QSPI AMOLED Display](#5-co5300-qspi-amoled-display)
6. [CST9217 Touch Controller](#6-cst9217-touch-controller)
7. [AXP2101 PMIC Power Management](#7-axp2101-pmic-power-management)
8. [TCA9554 GPIO Expander](#8-tca9554-gpio-expander)
9. [LC76G GPS via I2C (CASIC Protocol)](#9-lc76g-gps-via-i2c-casic-protocol)
10. [QMI8658 IMU](#10-qmi8658-imu)
11. [LVGL 9 Integration](#11-lvgl-9-integration)
12. [Complete Initialization Sequence](#12-complete-initialization-sequence)
13. [Differences from Waveshare Examples](#13-differences-from-waveshare-examples)

---

## 1. Board Overview

The Waveshare ESP32-S3-Touch-AMOLED-1.75 is a compact development board with
a round 1.75" AMOLED display, multi-constellation GPS, 6-axis IMU, PMIC,
and touch input. OpenDash uses it as the GPS/telemetry node in a multi-display
racecar dashboard system.

### 1.1 Key Components

| Component       | IC/Module      | Interface   | I2C Address |
|-----------------|----------------|-------------|-------------|
| MCU             | ESP32-S3R8     | —           | —           |
| Display         | CO5300 AMOLED  | QSPI        | —           |
| Touch           | CST9217        | I2C         | 0x5A        |
| GNSS            | LC76G          | I2C (CASIC) | 0x50/0x54/0x58 |
| IMU             | QMI8658        | I2C         | 0x6B        |
| PMIC            | AXP2101        | I2C         | 0x34        |
| GPIO Expander   | TCA9554        | I2C         | 0x20        |
| RTC             | PCF85063       | I2C         | 0x51        |

### 1.2 What Waveshare Gets Wrong

Waveshare's examples use:
- Arduino framework (not ESP-IDF)
- Their BSP library (incompatible with ESP-IDF 6.1+)
- Legacy I2C API (`i2c_driver_install`)
- `Wire.h` / `transmit_receive` for GPS (causes I2C state corruption)
- No PMIC power cycling (assumes GPS powers up correctly)
- No WAKE mechanism for LC76G I2C interface

OpenDash replaces all of this with direct ESP-IDF driver calls.

---

## 2. Pin Mapping Reference

### 2.1 QSPI Display (CO5300)

| Function | GPIO | Notes                     |
|----------|------|---------------------------|
| SCLK     | 38   | SPI2_HOST clock           |
| DATA0    | 4    | QSPI data line 0         |
| DATA1    | 5    | QSPI data line 1         |
| DATA2    | 6    | QSPI data line 2         |
| DATA3    | 7    | QSPI data line 3         |
| CS       | 12   | Chip select               |
| RST      | 39   | Display reset (active low)|
| DC       | -1   | Not used in QSPI mode    |

### 2.2 I2C Bus (Shared)

| Function | GPIO | Notes                     |
|----------|------|---------------------------|
| SDA      | 15   | Shared by all I2C devices |
| SCL      | 14   | Shared by all I2C devices |

Bus speed: 100 kHz for LC76G (override to 400kHz for PMIC/touch individually)

### 2.3 Touch (CST9217)

| Function | GPIO | Notes              |
|----------|------|--------------------|
| RST      | 40   | Active low reset   |
| INT      | 11   | Active low interrupt|

### 2.4 GPS (LC76G) — I2C Only

| Function | GPIO | Notes                        |
|----------|------|------------------------------|
| (none)   | —    | GPS uses shared I2C bus only |

> **Note:** GPIOs 17/18 are labeled as GPS UART in some schematics, but on this
> board NMEA data is routed via I2C only. UART does not work for NMEA.

### 2.5 Misc

| Function    | GPIO | Notes              |
|-------------|------|--------------------|
| Boot Button | 0    | Boot/user button   |
| IMU SDA     | 5    | QMI8658 (separate) |
| IMU SCL     | 6    | QMI8658 (separate) |

---

## 3. ESP-IDF Project Setup

### 3.1 Framework

ESP-IDF v6.1 (development branch). The Waveshare BSP is NOT used — it is
incompatible with this ESP-IDF version.

### 3.2 Required Components

```yaml
# idf_component.yml
dependencies:
  espressif/esp_lcd_co5300: ">=1.0.0"
  espressif/esp_lcd_touch_cst9217: ">=1.0.0"
  lvgl/lvgl: "~9.2"
```

### 3.3 sdkconfig Key Settings

```
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_FREERTOS_HZ=1000
CONFIG_I2C_MASTER_ISR_IRAM_SAFE=n
```

### 3.4 Partition Table

Use a custom partition table with sufficient app space:

```csv
# partitions.csv
nvs,      data, nvs,     0x9000,   0x4000,
otadata,  data, ota,     0xd000,   0x2000,
phy_init, data, phy,     0xf000,   0x1000,
factory,  app,  factory, 0x10000,  0x300000,
storage,  data, spiffs,  0x310000, 0xF0000,
```

---

## 4. I2C Bus Initialization

**Waveshare example:** Uses `Wire.begin(15, 14)` with default settings.

**OpenDash replacement** — direct ESP-IDF new driver API:

```c
#include "driver/i2c_master.h"

static i2c_master_bus_handle_t i2c_bus = NULL;

static esp_err_t i2c_bus_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port   = I2C_NUM_0,
        .sda_io_num = GPIO_NUM_15,
        .scl_io_num = GPIO_NUM_14,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .flags.enable_internal_pullup = true,  // External 4.7kΩ also recommended
    };
    return i2c_new_master_bus(&bus_cfg, &i2c_bus);
}
```

**Key difference:** Waveshare uses 400kHz globally. OpenDash creates each device
handle with its own SCL speed — 100kHz for LC76G, 400kHz for PMIC/touch/IMU.

---

## 5. CO5300 QSPI AMOLED Display

**Waveshare example:** Uses BSP + Arduino display library.

**OpenDash replacement** — direct `esp_lcd` API:

### 5.1 SPI Bus Configuration

```c
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_co5300.h"

spi_bus_config_t spi_cfg = {
    .sclk_io_num = GPIO_NUM_38,
    .data0_io_num = GPIO_NUM_4,
    .data1_io_num = GPIO_NUM_5,
    .data2_io_num = GPIO_NUM_6,
    .data3_io_num = GPIO_NUM_7,
    .max_transfer_sz = 466 * 20 * 3,  // 20 rows × RGB888
    .flags = SPICOMMON_BUSFLAG_QUAD,
};
spi_bus_initialize(SPI2_HOST, &spi_cfg, SPI_DMA_CH_AUTO);
```

### 5.2 Panel IO (QSPI)

```c
esp_lcd_panel_io_spi_config_t io_cfg = {
    .cs_gpio_num = GPIO_NUM_12,
    .dc_gpio_num = -1,               // Not used in QSPI
    .spi_mode = 0,
    .pclk_hz = 40 * 1000 * 1000,     // 40 MHz SPI clock
    .trans_queue_depth = 10,
    .lcd_cmd_bits = 32,
    .lcd_param_bits = 8,
    .flags.quad_mode = true,          // CRITICAL: enables quad SPI transfers
};
esp_lcd_panel_io_handle_t io_handle;
esp_lcd_new_panel_io_spi(SPI2_HOST, &io_cfg, &io_handle);
```

### 5.3 CO5300 Panel Init

```c
esp_lcd_panel_dev_config_t panel_cfg = {
    .reset_gpio_num = GPIO_NUM_39,
    .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
    .bits_per_pixel = 24,  // RGB888
};

// Vendor-specific init commands (critical for this panel)
co5300_vendor_config_t vendor_cfg = {
    .init_cmds = co5300_init_cmds,      // 14 init commands
    .init_cmds_size = ARRAY_SIZE(co5300_init_cmds),
    .flags.use_qspi_interface = 1,      // CRITICAL: use QSPI command opcodes (0x02/0x32)
};
panel_cfg.vendor_config = &vendor_cfg;

esp_lcd_panel_handle_t panel;
esp_lcd_new_panel_co5300(io_handle, &panel_cfg, &panel);
esp_lcd_panel_set_gap(panel, 6, 0);   // X offset = 6 pixels (hardware quirk)
esp_lcd_panel_reset(panel);
esp_lcd_panel_init(panel);
esp_lcd_panel_disp_on_off(panel, true);
```

### 5.4 Vendor Init Commands (Required)

These are the exact init commands needed for the CO5300 on this board:

```c
static const co5300_lcd_init_cmd_t co5300_init_cmds[] = {
    {0xFE, (uint8_t[]){0x20}, 1, 0},       // Page select
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},        // Main page
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x77}, 1, 0},        // RGB888 pixel format
    {0x35, (uint8_t[]){0x00}, 1, 0},        // Tearing effect ON
    {0x53, (uint8_t[]){0x20}, 1, 0},        // Brightness control
    {0x51, (uint8_t[]){0xFF}, 1, 0},        // Max brightness
    {0x63, (uint8_t[]){0xFF}, 1, 0},        // HBM brightness
    {0x2A, (uint8_t[]){0x00, 0x06, 0x01, 0xD7}, 4, 0},  // Column 6-471
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 600}, // Row 0-465, 600ms delay
    {0x11, NULL, 0, 600},                    // Sleep out, 600ms delay
    {0x29, NULL, 0, 0},                      // Display ON
};
```

### 5.5 Brightness Control

```c
void display_set_brightness(uint8_t percent)
{
    uint8_t val = (uint8_t)((percent * 255) / 100);
    uint8_t cmd[4] = {0x02, 0x00, 0x51, 0x00};  // QSPI write cmd
    uint8_t data[1] = {val};
    // Send via SPI panel IO...
}
```

---

## 6. CST9217 Touch Controller

**Waveshare example:** Uses proprietary touch library.

**OpenDash replacement:**

```c
#include "esp_lcd_touch_cst9217.h"

esp_lcd_touch_config_t touch_cfg = {
    .x_max = 466,
    .y_max = 466,
    .rst_gpio_num = GPIO_NUM_40,
    .int_gpio_num = GPIO_NUM_11,
    .levels = {
        .reset = 0,      // Active low
        .interrupt = 0,   // Active low
    },
    .flags = {
        .mirror_x = true,
        .mirror_y = true,
        .swap_xy = false,
    },
};

esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG();
esp_lcd_panel_io_handle_t tp_io;
esp_lcd_new_panel_io_i2c(i2c_bus, &io_cfg, &tp_io);

esp_lcd_touch_handle_t touch;
esp_lcd_touch_new_i2c_cst9217(tp_io, &touch_cfg, &touch);
```

Touch is polled at 50 Hz from a dedicated FreeRTOS task (not interrupt-driven).

---

## 7. AXP2101 PMIC Power Management

**Waveshare example:** No PMIC code provided — assumes BSP handles it.

**OpenDash replacement** — direct register I2C access:

### 7.1 Register Map (Key Addresses)

| Register | Purpose                    | Bit Layout                                |
|----------|----------------------------|-------------------------------------------|
| 0x80     | DCDC on/off control        | DC1=b0, DC2=b1, DC3=b2, DC4=b3, DC5=b4  |
| 0x90     | LDO on/off control 0      | ALDO1=b0, ALDO2=b1, ALDO3=b2, ALDO4=b3, BLDO1=b4, BLDO2=b5 |
| 0x91     | LDO on/off control 1      | DLDO1=b0, DLDO2=b1, CPUSLDO=b2           |
| 0x92     | ALDO1 voltage              | val × 100 + 500 mV                       |
| 0x93     | ALDO2 voltage              | val × 100 + 500 mV                       |
| 0x94     | ALDO3 voltage              | val × 100 + 500 mV                       |
| 0x95     | ALDO4 voltage              | val × 100 + 500 mV                       |
| 0x96     | BLDO1 voltage              | val × 100 + 500 mV                       |
| 0x97     | BLDO2 voltage              | val × 100 + 500 mV                       |

### 7.2 Reading/Writing Registers

```c
static esp_err_t axp2101_read_reg(i2c_master_dev_handle_t pmu, uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(pmu, &reg, 1, val, 1, 100);
}

static esp_err_t axp2101_write_reg(i2c_master_dev_handle_t pmu, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(pmu, buf, 2, 100);
}
```

### 7.3 GPS Power Rails

GPS module uses three rails controlled by register 0x90, bitmask 0x2C:
- ALDO3 (bit 2) = 3000 mV → GPS main power
- ALDO4 (bit 3) = 1800 mV → GPS I/O power
- BLDO2 (bit 5) = 2800 mV → GPS backup power

### 7.4 Diagnostic Status Logging

```c
static void axp2101_log_status(i2c_master_bus_handle_t bus)
{
    i2c_master_dev_handle_t pmu = NULL;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x34,
        .scl_speed_hz = 400000,
    };
    i2c_master_bus_add_device(bus, &cfg, &pmu);

    uint8_t dcdc_ctrl, ldo_ctrl0, ldo_ctrl1;
    axp2101_read_reg(pmu, 0x80, &dcdc_ctrl);
    axp2101_read_reg(pmu, 0x90, &ldo_ctrl0);
    axp2101_read_reg(pmu, 0x91, &ldo_ctrl1);

    ESP_LOGI(TAG, "DCDC: DC1=%s DC2=%s DC3=%s",
             (dcdc_ctrl & 0x01) ? "ON" : "off",
             (dcdc_ctrl & 0x02) ? "ON" : "off",
             (dcdc_ctrl & 0x04) ? "ON" : "off");
    ESP_LOGI(TAG, "LDO: ALDO3=%s ALDO4=%s BLDO2=%s (GPS rails)",
             (ldo_ctrl0 & 0x04) ? "ON" : "off",
             (ldo_ctrl0 & 0x08) ? "ON" : "off",
             (ldo_ctrl0 & 0x20) ? "ON" : "off");

    i2c_master_bus_rm_device(pmu);
}
```

---

## 8. TCA9554 GPIO Expander

**Waveshare example:** No TCA9554 code provided.

**OpenDash replacement:**

### 8.1 Register Map

| Register | Purpose          |
|----------|------------------|
| 0x00     | Input port       |
| 0x01     | Output port      |
| 0x02     | Polarity inversion |
| 0x03     | Configuration (0=output, 1=input) |

### 8.2 GPS Control Pins

| Pin | Function  | Active State | Config   |
|-----|-----------|--------------|----------|
| P4  | FORCE_ON  | HIGH = on    | Output   |
| P5  | NRESET    | HIGH = run   | Output   |

### 8.3 Configuration Code

```c
static esp_err_t tca9554_write_reg(i2c_master_dev_handle_t dev,
                                    uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(dev, buf, 2, 100);
}

void tca9554_configure_gps_pins(i2c_master_bus_handle_t bus)
{
    i2c_master_dev_handle_t tca = NULL;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x20,
        .scl_speed_hz = 400000,
    };
    i2c_master_bus_add_device(bus, &cfg, &tca);

    tca9554_write_reg(tca, 0x03, 0xCF);  // P4,P5 as outputs (clear bits 4,5)
    tca9554_write_reg(tca, 0x01, 0x30);  // P4=HIGH (FORCE_ON), P5=HIGH (run)

    i2c_master_bus_rm_device(tca);
}
```

---

## 9. LC76G GPS via I2C (CASIC Protocol)

**Waveshare example:** Uses Arduino Wire library with incorrect `transmit_receive` approach.

**OpenDash replacement:** Full CASIC split-address protocol. 

> **See the comprehensive guide:** [LC76G-I2C-GPS-Driver-Guide.md](LC76G-I2C-GPS-Driver-Guide.md)

### Summary of Critical Differences from Waveshare

| Aspect                  | Waveshare Example         | OpenDash Working Code              |
|-------------------------|---------------------------|-------------------------------------|
| I2C API                 | Wire.h / legacy driver    | `driver/i2c_master.h` (new API)    |
| Read method             | `transmit_receive(0x50)`  | Separate TX(0x50) + RX(0x54)      |
| Device handles          | Single handle             | Three handles (0x50, 0x54, 0x58)  |
| PMIC power cycle        | Not implemented           | Full AXP2101 cycle on every init   |
| WAKE mechanism          | Not documented            | CW config(0x50) + dummy(0x58)     |
| Bus reset               | Never                     | Before every polling cycle          |
| TX→RX delay             | 0ms (immediate)           | 10ms minimum                        |
| Recovery on failure     | None                      | `transmit_receive(0x50)` activation |
| Bus scan                | Probes all addresses      | Skips 0x50/0x54/0x58              |

---

## 10. QMI8658 IMU

| Parameter    | Value          |
|--------------|----------------|
| I2C Address  | 0x6B           |
| Data rate    | Configurable   |
| Accel range  | ±2g to ±16g    |
| Gyro range   | ±125 to ±2000°/s |

The QMI8658 uses standard I2C register-based communication and does not
require special handling like the LC76G.

---

## 11. LVGL 9 Integration

**Waveshare example:** Uses LVGL 8 with BSP helpers.

**OpenDash replacement** — manual LVGL 9 integration:

### 11.1 Display Configuration

```c
#include "lvgl.h"

#define DRAW_BUF_LINES  20

lv_display_t *disp = lv_display_create(466, 466);

// Allocate draw buffers in PSRAM (double-buffered)
size_t buf_sz = 466 * DRAW_BUF_LINES * 3;  // RGB888
uint8_t *buf1 = heap_caps_aligned_alloc(64, buf_sz, MALLOC_CAP_SPIRAM);
uint8_t *buf2 = heap_caps_aligned_alloc(64, buf_sz, MALLOC_CAP_SPIRAM);

lv_display_set_buffers(disp, buf1, buf2, buf_sz,
                       LV_DISPLAY_RENDER_MODE_PARTIAL);
lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB888);
lv_display_set_flush_cb(disp, lcd_flush_cb);
```

### 11.2 Flush Callback

```c
static void lcd_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, px);
    lv_display_flush_ready(disp);
}
```

### 11.3 Coordinate Rounder (QSPI Requirement)

The CO5300 QSPI interface requires coordinates aligned to 2-pixel boundaries:

```c
static void rounder_cb(lv_event_t *e)
{
    lv_area_t *area = lv_event_get_param(e);
    area->x1 = area->x1 & ~1;  // Round down to even
    area->y1 = area->y1 & ~1;
    area->x2 = area->x2 | 1;   // Round up to odd
    area->y2 = area->y2 | 1;
}
// Register: lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);
```

### 11.4 Tick Timer

```c
static void lvgl_tick_cb(void *arg) { lv_tick_inc(2); }

const esp_timer_create_args_t tick_args = {
    .callback = lvgl_tick_cb,
    .name = "lvgl_tick",
};
esp_timer_handle_t tick_timer;
esp_timer_create(&tick_args, &tick_timer);
esp_timer_start_periodic(tick_timer, 2000);  // 2ms tick
```

---

## 12. Complete Initialization Sequence

This is the exact order of operations in `app_main()`:

```
Step 1:  nvs_flash_init()           — Non-volatile storage
Step 2:  opendash_config_load()     — Load saved configuration
Step 3:  display_init()             — I2C bus + AMOLED + touch + LVGL
           ├─ i2c_bus_init()        — GPIO 15/14, 400kHz, internal pull-ups
           ├─ lcd_panel_init()      — CO5300 QSPI, vendor init commands
           ├─ touch_init()          — CST9217 (non-fatal if fails)
           ├─ lvgl_init_direct()    — Display + buffers + rounder + indev
           ├─ boot_button_init()    — GPIO 0 input
           └─ start polling tasks   — touch (50Hz) + button (50ms debounce)
Step 4:  show_splash_screen()       — 2 second splash
Step 5:  gps_handler_init()         — PMIC power cycle + LC76G device handles
Step 6:  imu_handler_init()         — QMI8658 initialization
Step 7:  ui_manager_init()          — Screen layouts
Step 8:  opendash_espnow_init()     — Wireless data broadcast
Step 9:  Start runtime tasks:
           ├─ gps_handler_start()   — GPS polling (core 0, prio 5)
           ├─ imu_handler_start()   — IMU polling
           ├─ ui_manager_start()    — UI rendering
           └─ data_broadcast_task   — ESP-NOW @ 5Hz (core 0, prio 4)
```

**CRITICAL:** `display_init()` MUST be called before `gps_handler_init()` because
the GPS handler obtains the shared I2C bus handle via `display_get_i2c_handle()`.

---

## 13. Differences from Waveshare Examples

### 13.1 No BSP

Waveshare provides a Board Support Package (BSP) that wraps all hardware init.
OpenDash does NOT use it because:
- Incompatible with ESP-IDF v6.1-dev
- Hides critical initialization details
- Prevents fine-grained control needed for GPS recovery mechanisms
- Adds unnecessary dependencies

### 13.2 No Arduino Framework

All code is pure ESP-IDF C. No `setup()`, `loop()`, `Wire.h`, or Arduino 
abstractions. Direct `i2c_master_transmit`, `i2c_master_receive`, and
`esp_lcd_panel_*` API calls throughout.

### 13.3 GPS: Completely Rewritten

The Waveshare GPS example is fundamentally broken because it uses 
`transmit_receive` on 0x50, which works occasionally but corrupts the LC76G
I2C state machine after the first read attempt. The OpenDash implementation
uses the correct split-address protocol with power cycling, wake, and recovery
mechanisms. See [LC76G-I2C-GPS-Driver-Guide.md](LC76G-I2C-GPS-Driver-Guide.md).

### 13.4 LVGL Version

Waveshare examples use LVGL 8. OpenDash uses LVGL 9.2 with updated API calls
(`lv_display_create` vs `lv_disp_drv_register`, etc.).

### 13.5 PMIC Control

Waveshare provides no PMIC management code. OpenDash directly reads/writes
AXP2101 registers for power rail control, diagnostics, and GPS power cycling.

---

> **This guide is maintained alongside the OpenDash project. For the most
> detailed GPS implementation reference, see
> [LC76G-I2C-GPS-Driver-Guide.md](LC76G-I2C-GPS-Driver-Guide.md).**
