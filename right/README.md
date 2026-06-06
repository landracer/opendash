<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OpenDash — Right Gauge Pod (2.8" Round LCD)

ESP-IDF project for the Right gauge pod in the OpenDash racecar dashboard system.

> **Central project reference:** [`../PROJECT_INDEX.md`](../PROJECT_INDEX.md)

## Hardware

| Item | Detail |
|---|---|
| **Board** | Waveshare ESP32-S3-Touch-LCD-2.8C |
| **Display** | 480×480 Round IPS LCD |
| **LCD Controller** | ST7701S — 3-wire 9-bit SPI init, then RGB565 parallel interface |
| **IO Expander** | TCA9554PWR (I2C addr 0x20) — controls LCD Reset + SPI CS |
| **Touch** | GT911 capacitive touch (I2C addr 0x5D) |
| **MCU** | ESP32-S3 dual-core @ 240 MHz |
| **Flash** | 16 MB (QIO 80 MHz) — custom partition table (2 MB app partitions) |
| **PSRAM** | 8 MB Octal SPI (required for LVGL frame buffer) |
| **Role** | I2C Slave — receives data from Center unit |

## Pin Mapping

All pin assignments verified against the **official Waveshare ESP32-S3-LCD-2.8C Demo** (ESP-IDF).

### LCD RGB Interface

| Signal | GPIO | Notes |
|---|---|---|
| HSYNC | 38 | Horizontal sync |
| VSYNC | 39 | Vertical sync |
| DE | 40 | Data enable |
| PCLK | 41 | Pixel clock (18 MHz) |
| DATA0–DATA15 | 5, 45, 48, 47, 21, 14, 13, 12, 11, 10, 9, 46, 3, 8, 18, 17 | B[0:4], G[0:5], R[0:4] (RGB565) |

### ST7701S SPI Init (3-Wire 9-Bit via ESP-IDF SPI Master)

| Signal | GPIO | Notes |
|---|---|---|
| MOSI | 1 | SPI data |
| SCLK | 2 | SPI clock (4 MHz) |
| CS | — | Via TCA9554 EXIO3 (I2C-controlled) |

> **SPI method:** Uses ESP-IDF `spi_master.h` with `command_bits=1` (D/CX flag) and `address_bits=8` (data byte) to implement the 9-bit protocol in hardware. CS is held low via TCA9554 during the entire init sequence.

### TCA9554PWR IO Expander (I2C addr 0x20)

| EXIO Pin | Function | Notes |
|---|---|---|
| EXIO1 | LCD Reset | Active-low pulse before SPI init |
| EXIO2 | GT911 Touch Reset | Pulsed during touch_probe() |
| EXIO3 | LCD SPI CS | Active-low during SPI programming |

### Backlight

| Signal | GPIO | Notes |
|---|---|---|
| BL | 6 | LEDC PWM, 4 kHz, 13-bit resolution, active high |

### Boot Button (Screen Cycling)

| Signal | GPIO | Notes |
|---|---|---|
| BOOT | 0 | Active-low, internal pull-up, 50 ms debounce |

Press to cycle display screens: **Gauge[0] → Gauge[1] → Gauge[2] → Odometer → Gauge[0] → ...**
Polled at 100 Hz by `button_read_task` (priority 3, core 0).
Up to 8 gauge pages configurable via `s_gauge_pages[]` in `ui_manager.c`.

### I2C Master Bus (shared: TCA9554 + GT911)

| Signal | GPIO | Notes |
|---|---|---|
| SDA | 15 | I2C port 0, 400 kHz |
| SCL | 7 | Shared by TCA9554 + GT911 |

### Touch (GT911)

| Parameter | Value |
|---|---|
| I2C Address | 0x5D (INT=LOW at reset) or 0x14 (INT=HIGH) |
| I2C Bus | Same as TCA9554 (port 0, SDA=15, SCL=7) |
| RST | TCA9554 EXIO2 (pulsed during probe) |
| INT | Board-specific GPIO (may be GPIO16 — conflicts with I2C slave SCL) |

> **Note:** Both addresses are probed. Full LVGL touch input device not yet registered.

### I2C Slave (Center ↔ Left Communication)

| Signal | GPIO | Notes |
|---|---|---|
| SDA | 4 | I2C port 1, slave mode |
| SCL | 16 | Address: 0x10 (Left), 0x11 (Right) |

## Architecture

### I2C Driver Usage

This project uses the **new ESP-IDF I2C driver APIs** exclusively:
- **`driver/i2c_master.h`** — For the master bus (TCA9554 + GT911)
- **`driver/i2c_slave.h`** — For Center unit communication (callback-based receive)
- The deprecated `driver/i2c.h` is **not used**

### Display Init Sequence

1. I2C master bus init (SDA=15, SCL=7, 400kHz)
2. TCA9554 IO expander init (all outputs, addr 0x20)
3. LCD hardware reset via TCA9554 EXIO1
4. SPI CS enable via TCA9554 EXIO3
5. SPI master init (MOSI=1, SCLK=2, 4MHz)
6. ST7701S register programming (from Waveshare demo sequence)
7. RGB panel init (18MHz PCLK, 480×480, timing from Waveshare demo)
8. SPI CS disable via TCA9554 EXIO3
9. LEDC backlight on GPIO6 (4kHz, 13-bit, 70% default)
10. GT911 touch probe on same I2C bus
11. LVGL 9.x init with PSRAM draw buffer
12. Boot button task start (GPIO 0, 100 Hz poll, 50 ms debounce)

### FreeRTOS Tasks

| Task | Priority | Core | Rate | Description |
|---|---|---|---|---|
| `app_main` (main loop) | Default | 0 | 50 ms | I2C receive → odometer accumulate → UI update |
| `ui_task` (LVGL) | 5 | 1 | ~5 ms | `lv_timer_handler()` rendering |
| `button_read_task` | 3 | 0 | 10 ms | Boot button polling (GPIO 0) |

> **WDT safety:** Odometer UI updates are rate-limited to 1/second and guarded
> so they only execute when the Odometer screen is active. A `taskYIELD()` is
> inserted between the two outlined-text updates (10 label writes total) to
> let IDLE0 run. The `ui_task` also wraps `lv_timer_handler()` in the LVGL
> mutex to prevent concurrent access from core 0 and core 1.

## Project Structure

```
left/
├── CMakeLists.txt              # Project-level CMake (includes ../common)
├── partitions.csv              # Custom partition table (2 MB app partitions)
├── sdkconfig.defaults          # ESP32-S3 + PSRAM + LVGL config
├── display.ini                 # Legacy pin mapping reference
├── README.md                   # This file
└── main/
    ├── CMakeLists.txt          # Component dependencies
    ├── idf_component.yml       # LVGL 9.2 dependency
    ├── main.c                  # Entry point, I2C slave, odometer, main loop
    ├── display_init.c          # I2C master, TCA9554, SPI, RGB panel, backlight, boot button
    ├── display_init.h          # Pin definitions, boot button config, public API
    ├── ui_manager.c            # LVGL round gauge UI (arc + primary + secondary + odo)
    └── ui_manager.h            # UI manager API (init, update, warning, navigate)
```

## Flash Partitions

Custom partition table for 16 MB flash (`partitions.csv`):

| Partition | Offset | Size | Purpose |
|---|---|---|---|
| `nvs` | `0x9000` | 20 KB | Config & odometer persistence |
| `otadata` | `0xE000` | 8 KB | OTA boot state |
| `factory` | `0x10000` | **2 MB** | Factory firmware |
| `ota_0` | `0x210000` | **2 MB** | OTA slot 0 |
| `ota_1` | `0x410000` | **2 MB** | OTA slot 1 |
| `storage` | `0x611000` | ~10 MB | General storage |

2 MB app partitions accommodate compiled-in background images (~1 MB).
Current binary: ~1.05 MB (49% headroom).

## Building & Flashing

### Prerequisites

- **ESP-IDF v5.3+** (tested with v6.1-dev)
- **VS Code** with ESP-IDF extension (recommended)
- **USB-C cable**

### Command Line

```bash
cd left/
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

## Display Screens (Multi-Page Gauge System)

The boot button (GPIO 0) cycles through up to 8 gauge pages + 1 odometer page.
All gauge pages share a **single set of LVGL widgets** — on page switch, labels,
unit suffixes, and arc range are swapped instantly (no create/destroy).

Default pages (configurable via `s_gauge_pages[]` in `ui_manager.c`):

| Screen | Primary (Arc) | Secondary (Box) | Features |
|---|---|---|---|
| Page 0 | OIL PRESS (0–700 kPa) | BOOST (kPa/PSI) | Pressure unit conversion |
| Page 1 | WATER TEMP (0–130°C) | SPEED (MPH/km/h) | Temp + speed conversion |
| Page 2 | RPM (0–8000) | AFR (:1) | **Shift-light blink** at >90% |
| Page 3 | ODOMETER | TRIP A | Distance unit conversion |

### Min/Max Tracking

Each gauge page tracks session min/max of the primary value.
Displayed below the primary value as `MIN:xx  MAX:yy`.
Resets on reboot (not persisted).

### Shift-Light Blink

On pages with `shift_light=true` (e.g. RPM), when the arc exceeds 90% the
arc indicator color toggles between **red** and **blue** at 150ms intervals.
Reverts to normal blue when below threshold.

## UI Layout — Gauge Mode

```
                    480 px
      ┌──────────────────────────────┐
     ╱                                ╲
    │     ┌── Arc (270° sweep) ───┐    │
    │     │ ARC_SIZE: 450px         │    │    ARC_WIDTH = 45px
    │     │ Outline: symmetric     │    │    ARC_OUTLINE_EXTRA = 5px
    │     │ Width: 45px            │    │
    │     │ Outline: +5px/side     │    │
    │     │                        │    │
    │     │  ┏━━ Primary Value ━━┓ │    │    XXLARGE (96px) font
    │     │  ┃   OIL PRESS       ┃ │    │    Centered in display
    │     │  ┃     47.2          ┃ │    │
    │     │  ┗━━━━━━━━━━━━━━━━━━┛ │    │
    │     │                        │    │
    │     └────────┐    ┌──────────┘    │    90° gap at bottom
    │         ┌─────────────────┐       │
    │         │ BOOST     ← label│       │    ┌ SECONDARY BOX ────────┐
    │         │    14.7    ← val │       │    │ 200×100 px, Y=+150    │
    │         │          kPa ←unit│       │    │ Label: TOP_LEFT  MED  │
    │         └─────────────────┘       │    │ Value: CENTER    XLARGE│
     ╲                                ╱     │ Unit:  BOT_RIGHT MED  │
      └──────────────────────────────┘      └──────────────────────────┘
```

### Key Layout Constants (in `ui_manager.c`)

| Constant | Value | Description |
|---|---|---|
| `ARC_SIZE` | 450 px | Bounding box for BOTH arcs (symmetric outline) |
| `ARC_WIDTH` | 45 px | Main arc width |
| `ARC_OUTLINE_EXTRA` | 5 px | Extra width per side for white outline |
| `ARC_SWEEP_DEG` | 270° | Arc sweep |
| `ARC_ROTATION_DEG` | 135° | Start angle (gap at bottom) |
| `SECONDARY_BOX_W` | 200 px | Secondary box width |
| `SECONDARY_BOX_H` | 100 px | Secondary box height (taller for readability) |
| `SECONDARY_BOX_Y` | +150 | Y offset from center (sits in arc gap) |

Default gauge pages: **3 gauge + 1 odo** (see `s_gauge_pages[]` in `ui_manager.c`).
Up to `GAUGE_PAGE_MAX` (8) pages can be defined at compile time.

## Unit Conversion

The display automatically converts values based on user preferences stored in NVS:

| Measurement | Options | Internal Unit |
|---|---|---|
| Temperature | °C / °F | Celsius |
| Speed | km/h / MPH | km/h |
| Pressure | kPa / BAR / PSI | kPa |
| Distance | km / mi | km |

See `common/include/opendash_display_config.h` for unit enums and
`common/include/opendash_ui_styles.h` for conversion functions.

## Warning System

Warnings overlay the current screen with flashing colored boxes:

```c
ui_manager_warning_trigger(OPENDASH_WARNING_CRITICAL, "LOW OIL", 500);
ui_manager_warning_clear();
```

| Level | Color | Description |
|---|---|---|
| `OPENDASH_WARNING_CAUTION` | Orange | Non-critical alert |
| `OPENDASH_WARNING_CRITICAL` | Red | Immediate attention required |

## Odometer

NVS-persisted trip and total distance meter:

- Accumulates distance from GPS speed × time
- Saves to NVS every 100 m (minimizes flash wear)
- Trip A/B resettable independently
- Displayed on Odometer screen (boot button to switch)
- NVS namespace: `"od_odo"`

## I2C Protocol

See [`../common/include/opendash_i2c_protocol.h`](../common/include/opendash_i2c_protocol.h) for the full protocol definition.

| Command | ID | Description |
|---|---|---|
| SET_DATA_POINT | 0x01 | Update a displayed value |
| SET_BRIGHTNESS | 0x04 | Change backlight brightness |
| SYSTEM_CMD | 0x07 | System commands (reboot, OTA, etc.) |

Received commands processed in `process_i2c_messages()` in `main.c`.
Includes odometer control (reset trip) and warning trigger/clear sub-commands.

## Left vs. Right

The `/right` and `/right` directories contain identical firmware. The only differences:

| Item | Left | Right |
|---|---|---|
| I2C Address | `0x10` | `0x11` |
| Node Type | `OPENDASH_NODE_LEFT` | `OPENDASH_NODE_RIGHT` |
| Default Primary | Boost | Boost |
| Default Secondary | AFR | AFR |
| Background Image | `background_right` | `background_right` |

To mirror code changes from left → right, sync the `.c`/`.h` files and apply
sed substitutions for the identifiers above.

---

**OpenDash Right Gauge** — Built for racers, by racers.
