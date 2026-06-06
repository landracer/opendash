<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OpenDash — Project Index & Glossary

> **Central reference for the entire OpenDash codebase.**
> Start here to understand the project, then follow links to detailed documents.
>
> Last updated: June 2025

---

## Table of Contents

1. [What is OpenDash?](#what-is-opendash)
2. [Hardware Inventory](#hardware-inventory)
3. [Repository Map](#repository-map)
4. [Documentation Index](#documentation-index)
5. [Common Library Reference](#common-library-reference)
6. [Display Node Architecture](#display-node-architecture)
7. [I2C Bus & Protocol](#i2c-bus--protocol)
8. [Data Points (Sensor Legend)](#data-points-sensor-legend)
9. [Unit Conversion System](#unit-conversion-system)
10. [Font System](#font-system)
11. [Image / Asset Pipeline](#image--asset-pipeline)
12. [Flash Partitioning](#flash-partitioning)
13. [NVS (Non-Volatile Storage) Namespaces](#nvs-non-volatile-storage-namespaces)
14. [FreeRTOS Tasks & Threading](#freertos-tasks--threading)
15. [Build System](#build-system)
16. [Glossary](#glossary)

---

## What is OpenDash?

OpenDash is an open-source, modular racecar dashboard system.  Four ESP32-S3
display nodes communicate over a shared I2C bus to present real-time engine,
GPS, IMU, and battery data to the driver.

| Component | Role |
|---|---|
| **Center** | 4.3" main dash — I2C master, OBD2/CAN, data aggregator |
| **Left** | 2.8" round gauge pod — I2C slave `0x10`, oil pressure/temp |
| **Right** | 2.8" round gauge pod — I2C slave `0x11`, boost/AFR |
| **GPS** | 1.75" AMOLED — I2C slave `0x12`, GNSS + IMU + parachute |

All nodes run **ESP-IDF** (v5.3+ / v6.1-dev tested) with **LVGL 9.2** for
display rendering.  Shared code lives in `common/`.

---

## Hardware Inventory

| Node | Board | Display | Resolution | Controller | MCU |
|---|---|---|---|---|---|
| Center | Waveshare ESP32-S3-Touch-LCD-4.3 | 4.3" IPS | 800×480 | ST7262 RGB | ESP32-S3 |
| Left | Waveshare ESP32-S3-Touch-LCD-2.8C | 2.8" Round IPS | 480×480 | ST7701S + TCA9554 | ESP32-S3 |
| Right | Waveshare ESP32-S3-Touch-LCD-2.8C | 2.8" Round IPS | 480×480 | ST7701S + TCA9554 | ESP32-S3 |
| GPS | Waveshare ESP32-S3-Touch-AMOLED-1.75 | 1.75" Round AMOLED | 466×466 | CO5300 | ESP32-S3 |

All boards: 16 MB Flash, 8 MB PSRAM (Octal).

For detailed pin mappings, wiring, and IO expander usage see:
- [`docs/hardware.md`](docs/hardware.md) — all four nodes
- [`left/README.md`](left/README.md) — Left/Right pin tables
- [`gps/README.md`](gps/README.md) — GPS + IMU + GNSS wiring

---

## Repository Map

```
opendash/
│
├── PROJECT_INDEX.md            ★ YOU ARE HERE — master glossary & index
├── readme.md                   Landing page & quick links
├── QUICKSTART.md               5-minute getting-started guide
├── CHANGELOG.md                Version history
│
├── docs/                       ── Detailed Design Docs ──────────────────
│   ├── architecture.md         System architecture & data flow diagrams
│   ├── hardware.md             Hardware specs, pin maps, wiring
│   ├── i2c-protocol.md         I2C message format, commands, polling
│   ├── data-points.md          Data point IDs (0x0100–0x0500) with units
│   ├── setup-guide.md          Dev environment setup (ESP-IDF, toolchains)
│   ├── vscode-setup.md         VS Code workspace configuration
│   ├── FONTS_QUICK_START.md    Adding custom fonts (quick reference)
│   ├── FONT_SYSTEM_IMPLEMENTATION.md   Font pipeline internals
│   └── font-system-testing.md  Testing the font conversion system
│
├── common/                     ── Shared Component Library ──────────────
│   ├── include/                Public C headers (10 files)
│   │   ├── opendash_common.h           Node types, version, errors
│   │   ├── opendash_data_model.h       Data point IDs & registry
│   │   ├── opendash_i2c_protocol.h     I2C message format & commands
│   │   ├── opendash_display_config.h   Layout struct, unit enums, NVS API
│   │   ├── opendash_ui_styles.h        Colors, style helpers, unit converters
│   │   ├── opendash_fonts.h            Font size enum, set_font() helpers
│   │   ├── opendash_odometer.h         Odometer + trip meter structs, NVS
│   │   ├── opendash_obd2.h             OBD2 PID definitions
│   │   ├── opendash_checklist.h        Pre-flight checklist system
│   │   └── opendash_wifi_ble.h         WiFi/BLE management API
│   ├── src/                    Implementations (.c files)
│   │   ├── opendash_data_model.c
│   │   ├── opendash_display_config.c
│   │   ├── opendash_i2c_protocol.c
│   │   ├── opendash_odometer.c
│   │   └── opendash_checklist.c
│   ├── fonts/                  Font build pipeline
│   │   ├── ttf/                Source TrueType files
│   │   ├── font_config.json    Font sizes & default selection
│   │   ├── convert_fonts.py    TTF → LVGL C array converter
│   │   └── README.md           Font system documentation
│   └── images/                 Image build pipeline
│       ├── source/             Source PNGs per display
│       ├── convert_images.py   PNG → LVGL C array converter
│       └── README.md           Image system documentation
│
├── center/                     ── Center Display Project ────────────────
│   ├── main/
│   │   ├── main.c              Entry point, I2C master, OBD2/CAN
│   │   ├── display_init.c/h    ST7262 RGB panel, touch, LVGL init
│   │   └── ui_manager.c/h      Multi-screen UI (ENGINE, GPS, custom modes)
│   ├── partitions.csv          Flash partition table
│   ├── sdkconfig.defaults      Build config
│   └── README.md               Center-specific documentation
│
├── left/                       ── Left Gauge Pod ────────────────────────
│   ├── main/
│   │   ├── main.c              Entry, I2C slave (0x10), odometer, main loop
│   │   ├── display_init.c/h    ST7701S 3-wire SPI + RGB, TCA9554, boot button
│   │   └── ui_manager.c/h      Round gauge UI (arc, primary, secondary, odo)
│   ├── partitions.csv          Custom partition table (2MB app partitions)
│   ├── display.ini             Legacy pin reference
│   ├── sdkconfig.defaults      Build config
│   └── README.md               Left pod documentation
│
├── right/                      ── Right Gauge Pod ───────────────────────
│   └── (same structure as left/, I2C addr 0x11 instead of 0x10)
│
├── left-right/                 ── Combined Left+Right (alternative) ─────
│   └── (single project for both pods, deprecated in favor of separate)
│
├── gps/                        ── GPS / Telemetry Unit ──────────────────
│   ├── main/
│   │   ├── main.c              Entry, I2C slave (0x12)
│   │   ├── display_init.c/h    CO5300 AMOLED init
│   │   ├── ui_manager.c/h      AMOLED gauge UI
│   │   ├── gps_handler.c/h     LC76G GPS I2C driver (v15L2 PRODUCTION)
│   │   ├── imu_handler.c/h     QMI8658 accelerometer + gyro
│   │   └── parachute.c/h       Gyro-triggered parachute deployment
│   ├── sdkconfig.defaults
│   ├── INTENSIVE_TODO.md       Detailed phased build TODO (partially archived)
│   └── README.md               GPS unit documentation (I2C, NOT UART)
│
├── wiki/                       ── Authoritative Reference Docs ─────────
│   ├── LC76G-I2C-GPS-Driver-Guide.md   SOLE GPS REFERENCE (v2.0.0)
│   └── LC76G-10Hz-Spec-Breakout.md     10 Hz logging specs
│
├── build/                      Top-level CMake output (generated)
├── .claude/                    AI agent configuration
├── check_deps.sh               Verify build dependencies
├── push_*.sh                   Git branch push helpers
└── opendash.code-workspace     VS Code multi-root workspace file
```

---

## Documentation Index

Every documentation file in the project, grouped by purpose.

### Getting Started (Read First)

| Document | What You'll Learn |
|---|---|
| [**readme.md**](readme.md) | Project overview, feature list, quick links |
| [**QUICKSTART.md**](QUICKSTART.md) | 5-minute hardware + software setup |
| [**BUILD_DEPENDENCIES.md**](BUILD_DEPENDENCIES.md) | Install ESP-IDF, Node.js, Python, ImageMagick |
| [**docs/setup-guide.md**](docs/setup-guide.md) | Full dev environment setup walkthrough |
| [**docs/vscode-setup.md**](docs/vscode-setup.md) | VS Code workspace, build, flash, monitor config |

### Architecture & Design

| Document | What You'll Learn |
|---|---|
| [**docs/architecture.md**](docs/architecture.md) | System block diagram, node roles, software layers |
| [**docs/hardware.md**](docs/hardware.md) | Board specs, pin mappings, wiring for all 4 nodes |
| [**docs/i2c-protocol.md**](docs/i2c-protocol.md) | I2C message format, command table, polling cycle |
| [**docs/data-points.md**](docs/data-points.md) | All data point IDs (engine, GPS, IMU, BMS, system) |
| [**DISPLAY_SYNCHRONIZATION.md**](DISPLAY_SYNCHRONIZATION.md) | How shared code stays in sync across nodes |

### Per-Node Guides

| Document | What You'll Learn |
|---|---|
| [**center/README.md**](center/README.md) | Center display: multi-screen UI, display modes, CAN |
| [**left/README.md**](left/README.md) | Left gauge: pin map, init sequence, build, UI layout |
| [**right/README.md**](right/README.md) | Right gauge: same as left but I2C `0x11` |
| [**gps/README.md**](gps/README.md) | GPS unit: LC76G I2C CASIC, IMU, AMOLED |
| [**gps/INTENSIVE_TODO.md**](gps/INTENSIVE_TODO.md) | Phased build plan for GPS firmware (partially archived) |

### GPS / I2C Reference Documents

| Document | What You'll Learn |
|---|---|
| [**wiki/LC76G-I2C-GPS-Driver-Guide.md**](wiki/LC76G-I2C-GPS-Driver-Guide.md) | **SOLE AUTHORITATIVE GPS REFERENCE** — v2.0.0, v15L2 production |
| [**wiki/LC76G-10Hz-Spec-Breakout.md**](wiki/LC76G-10Hz-Spec-Breakout.md) | 10 Hz GPS logging: bandwidth, timing, implementation checklist |

### Asset Pipelines

| Document | What You'll Learn |
|---|---|
| [**common/fonts/README.md**](common/fonts/README.md) | Font management: TTF → LVGL C conversion pipeline |
| [**docs/FONTS_QUICK_START.md**](docs/FONTS_QUICK_START.md) | Quick guide to adding custom fonts |
| [**docs/FONT_SYSTEM_IMPLEMENTATION.md**](docs/FONT_SYSTEM_IMPLEMENTATION.md) | Font system internals and auto-conversion |
| [**docs/font-system-testing.md**](docs/font-system-testing.md) | Testing the font converter |
| [**common/images/README.md**](common/images/README.md) | Image pipeline: backgrounds, per-pod images |
| [**common/images/source/README.md**](common/images/source/README.md) | Where to place source images |

### Troubleshooting & History

| Document | What You'll Learn |
|---|---|
| [**docs/archived/COMPILE_ERRORS_RESOLUTION.md**](docs/archived/COMPILE_ERRORS_RESOLUTION.md) | Common compile error fixes |
| [**docs/archived/FONT_ISSUE_INVESTIGATION.md**](docs/archived/FONT_ISSUE_INVESTIGATION.md) | Font include path debugging |
| [**docs/archived/RESOLUTION_SUMMARY.md**](docs/archived/RESOLUTION_SUMMARY.md) | Auto-gen font/lib compile-error resolutions |
| [**SCREEN_ENHANCEMENTS.md**](SCREEN_ENHANCEMENTS.md) | Center display multi-screen + warning boxes |
| [**docs/archived/IMPLEMENTATION_COMPLETE.md**](docs/archived/IMPLEMENTATION_COMPLETE.md) | Status: center alignment + auto font conversion |
| [**docs/archived/IMPLEMENTATION_SUMMARY.md**](docs/archived/IMPLEMENTATION_SUMMARY.md) | All implementation work summary |
| [**CHANGELOG.md**](CHANGELOG.md) | Version history |

---

## Common Library Reference

Every shared header in `common/include/` and what it provides.

### `opendash_common.h` — Core Types & Version

| Symbol | Type | Description |
|---|---|---|
| `OPENDASH_VERSION_STR` | `#define` | Version string (`"0.1.0"`) |
| `opendash_node_t` | `enum` | Node types: `CENTER`, `LEFT`, `RIGHT`, `GPS`, `POD1`–`POD8` |
| `OPENDASH_NODE_IS_POD(n)` | `macro` | Returns true for pod-type nodes |
| `OPENDASH_MAX_SECTIONS` | `#define` | Max configurable screen sections (8) |
| `opendash_section_config_t` | `struct` | Per-section config (data point, display mode, alarms) |
| `opendash_alarm_config_t` | `struct` | Alarm thresholds (low, high, level) |
| `opendash_err_t` | `enum` | Error codes: `OK`, `ERR_INVALID_ARG`, `ERR_NVS`, etc. |

### `opendash_data_model.h` — Data Point Registry

Defines 16-bit IDs for every displayable value. Grouped by category:

| Range | Category | Examples |
|---|---|---|
| `0x0100`–`0x01FF` | Engine / OBD2 | RPM, speed, coolant, boost, oil, AFR, EGT |
| `0x0200`–`0x02FF` | GPS / Navigation | GPS speed, heading, lat/lon, lap time, delta |
| `0x0300`–`0x03FF` | IMU / Motion | G-force (3-axis), yaw/pitch/roll rate/angle |
| `0x0400`–`0x04FF` | Battery / BMS | Pack V/A, SOC, cell voltages, temp |
| `0x0500`–`0x05FF` | System | CPU temp, free heap, WiFi RSSI, uptime |

Full table: [`docs/data-points.md`](docs/data-points.md)

### `opendash_i2c_protocol.h` — I2C Message Format

| Field | Size | Description |
|---|---|---|
| SYNC | 1 B | Always `0xAA` |
| CMD | 1 B | Command ID |
| LENGTH | 1 B | Payload length (0–248) |
| PAYLOAD | 0–248 B | Command-specific data |
| CHECKSUM | 1 B | XOR of all preceding bytes |

Key commands: `SET_DATA_POINT (0x01)`, `SET_SCREEN_LAYOUT (0x02)`,
`SET_ALARM (0x03)`, `SET_BRIGHTNESS (0x04)`, `REQUEST_DATA (0x06)`,
`SYSTEM_CMD (0x07)`.

Full protocol: [`docs/i2c-protocol.md`](docs/i2c-protocol.md)

### `opendash_display_config.h` — Layout & Unit Configuration

| Symbol | Type | Description |
|---|---|---|
| `OPENDASH_NVS_NAMESPACE` | `#define` | NVS namespace: `"od_config"` |
| `opendash_temp_unit_t` | `enum` | `CELSIUS`, `FAHRENHEIT` |
| `opendash_speed_unit_t` | `enum` | `KMH`, `MPH` |
| `opendash_pressure_unit_t` | `enum` | `KPA`, `BAR`, `PSI` |
| `opendash_distance_unit_t` | `enum` | `KM`, `MI` |
| `opendash_display_layout_t` | `struct` | Full layout: sections[], brightness, theme, all unit prefs |
| `opendash_config_load()` | `func` | Load layout from NVS (uses defaults on first boot) |
| `opendash_config_save()` | `func` | Persist layout to NVS |
| `opendash_config_reset_defaults()` | `func` | Reset to node-appropriate factory defaults |

### `opendash_ui_styles.h` — Colors, Style Helpers, Unit Converters

**Color Palette:**

| Constant | Hex | Use |
|---|---|---|
| `COLOR_TEXT_PRIMARY` | `0xFFFFFF` | Main text (white) |
| `COLOR_TEXT_SECONDARY` | `0xAAAAAA` | Labels (light gray) |
| `COLOR_TEXT_OUTLINE` | `0x000000` | Text outline (black) |
| `COLOR_WARNING_BOX_RED` | `0xFF0000` | Critical warning flash |
| `COLOR_WARNING_BOX_ORANGE` | `0xFF6600` | Caution warning flash |
| `COLOR_OK` | `0x00FF00` | System OK (green) |
| `COLOR_CAUTION` | `0xFFAA00` | Caution state (orange) |

**Unit conversion functions** (all `static inline`):

| Function | Input → Output |
|---|---|
| `opendash_convert_temp(°C, unit)` | °C → °C or °F |
| `opendash_convert_speed(km/h, unit)` | km/h → km/h or MPH |
| `opendash_convert_pressure(kPa, unit)` | kPa → kPa, BAR, or PSI |
| `opendash_convert_distance(km, unit)` | km → km or mi |

### `opendash_fonts.h` — Font Size Enum

| Size Enum | Pixels | Typical Use |
|---|---|---|
| `OPENDASH_FONT_SIZE_SMALL` | 14 px | Fine print, status indicators |
| `OPENDASH_FONT_SIZE_MEDIUM` | 18 px | Labels ("BOOST", "OIL TEMP"), unit suffixes |
| `OPENDASH_FONT_SIZE_LARGE` | 32 px | Secondary values |
| `OPENDASH_FONT_SIZE_XLARGE` | 64 px | Primary values in secondary box |
| `OPENDASH_FONT_SIZE_XXLARGE` | 96 px | Primary gauge value (center of arc) |
| `OPENDASH_FONT_SIZE_XXXLARGE` | 128 px | Maximum size (headline use) |

Fonts are auto-converted from TTF at build time.  See `common/fonts/font_config.json` for active fonts and `common/fonts/convert_fonts.py` for the conversion pipeline.

### `opendash_odometer.h` — Odometer & Trip Meter

| Symbol | Description |
|---|---|
| `ODOMETER_NVS_NAMESPACE` | `"od_odo"` — separate NVS namespace |
| `ODOMETER_NVS_SAVE_INTERVAL_M` | Save to NVS every 100 m to reduce wear |
| `opendash_odometer_mode_t` | `TOTAL` or `TRIP_A` display mode |
| `opendash_odometer_t` | Struct: `total_m`, `trip_a_m`, `trip_b_m`, `speed_kmh`, `mode` |
| `opendash_odometer_init()` | Load last values from NVS |
| `odometer_accumulate()` | Add distance based on `speed × dt` |
| `odometer_reset_trip()` | Reset trip A or B |

### `opendash_obd2.h` — OBD2 PID Definitions

Standard OBD2 PID constants and helper macros for CAN frame encoding/decoding.

### `opendash_checklist.h` — Pre-Flight Checklist

Configurable task lists shared across I2C.  Items are touch-confirmable on any display.

### `opendash_wifi_ble.h` — Wireless Management

WiFi and BLE API for OTA updates, data sync, and companion app connectivity.

---

## Display Node Architecture

Every display project (`center/`, `left/`, `right/`, `gps/`) follows the same
three-file pattern:

```
main/
├── main.c           Entry point, ESP-NOW comm, data loop, odometer
├── display_init.c   Hardware: LCD controller, touch, LVGL init
├── display_init.h   Pin defines, public init/lock API
├── ui_manager.c     LVGL screen creation, widgets, updates
└── ui_manager.h     Public UI API (init, update, warning, navigate)
```

### Left/Right Gauge Pod — Multi-Page Gauge System

All gauge pages share a single set of LVGL widgets. On page switch, labels,
unit suffixes, and arc range are swapped instantly (no create/destroy).

Default pages (configurable via `s_gauge_pages[]` in `ui_manager.c`):

| Screen | Primary (Arc) | Secondary (Box) | Features |
|---|---|---|---|
| Page 0 | OIL PRESS (0–700 kPa) | BOOST (kPa/PSI) | Pressure conversion |
| Page 1 | WATER TEMP (0–130°C) | SPEED (MPH/km/h) | Temp + speed conversion |
| Page 2 | RPM (0–8000) | AFR (:1) | **Shift-light blink** at >90% |
| Page 3 | ODOMETER | TRIP A | Distance conversion |

Up to `GAUGE_PAGE_MAX` (8) pages can be defined at compile time.

- **Min/Max tracking:** Each gauge page tracks session min/max of primary value,
  displayed as `MIN:xx  MAX:yy` below the reading.
- **Shift-light:** Pages with `shift_light=true` blink the arc color red/blue
  at 150ms intervals when the arc exceeds 90%.

**Screen switching:** Boot button on GPIO 0.  Press cycles through all pages.
Button is polled at 100 Hz with 50 ms debounce (see `display_init.c`).

### Communication Architecture

**⚠️ Important Note:** The original design intended to use I2C for inter-node communication, but due to hardware limitations and GPIO conflicts, the system was re-implemented to use ESP-NOW (WiFi peer-to-peer) for communication between nodes. This provides zero-wire communication with no GPIO conflicts and better reliability.

- **Center Display:** Acts as ESP-NOW master, broadcasting data to peripheral nodes
- **Peripheral Nodes (Left, Right, GPS):** Act as ESP-NOW slaves, receiving data and responding to commands
- **Communication:** All nodes use ESP-NOW for wireless communication with no physical I2C connections between nodes

### Left/Right Gauge — UI Geometry (480×480 Round Display)

```
                    480 px
      ┌──────────────────────────────┐
     ╱                                ╲
    │     ┌─── Arc (270° sweep) ────┐  │
    │     │  ARC_SIZE: 450px         │  │
    │     │  Width: 45px             │  │   ARC_WIDTH = 45px
    │     │  Outline: +5px symmetric │  │   ARC_OUTLINE_EXTRA = 5px
    │     │  (same bbox, wider width)│  │   Outline arc total = 55px
    │     │                          │  │
    │     │   ┏━━ Primary Value ━━┓  │  │   XXLARGE (96px) font
    │     │   ┃    OIL PRESS      ┃  │  │   Centered in display
    │     │   ┃      47.2         ┃  │  │
    │     │   ┃  MIN:32  MAX:68   ┃  │  │   Session min/max tracking
    │     │   ┗━━━━━━━━━━━━━━━━━━┛  │  │
    │     │                          │  │
    │     └──────────┐  ┌────────────┘  │
    │      ┌──────────────────────┐     │
    │      │ BOOST          ← label│     │   ┌─ SECONDARY BOX ─────────┐
    │      │       14.7      ← val │     │   │ W=200  H=100  Y=+150   │
    │      │              kPa ← unit│     │   │ Label: TOP_LEFT  MED   │
    │      └──────────────────────┘     │   │ Value: CENTER     XLARGE│
     ╲                                ╱     │ Unit:  BOT_RIGHT  MED   │
      └──────────────────────────────┘      └──────────────────────────┘
```

Key layout constants (defined in `ui_manager.c`):

| Constant | Value | Description |
|---|---|---|
| `ARC_SIZE` | 450 px | Bounding box for BOTH arcs (symmetric outline) |
| `ARC_WIDTH` | 45 px | Drawing width of main arc |
| `ARC_OUTLINE_EXTRA` | 5 px | Extra width for outline arc (each side) |
| `ARC_SWEEP_DEG` | 270° | Arc sweep angle |
| `ARC_ROTATION_DEG` | 135° | Arc start angle (gap at bottom) |
| `SECONDARY_BOX_W` | 200 px | Secondary info box width |
| `SECONDARY_BOX_H` | 100 px | Secondary info box height |
| `SECONDARY_BOX_Y` | +150 | Y offset from center (in arc gap) |
| `SECONDARY_BOX_RADIUS` | 8 px | Corner radius |

### Warning System

Any node can trigger a warning overlay:

```c
ui_manager_warning_trigger(OPENDASH_WARNING_CRITICAL, "LOW OIL", 500);
ui_manager_warning_clear();
```

| Level | Color | Flash Rate |
|---|---|---|
| `OPENDASH_WARNING_CAUTION` | Orange (`0xFF6600`) | Configurable ms |
| `OPENDASH_WARNING_CRITICAL` | Red (`0xFF0000`) | Configurable ms |

---

## I2C Bus & Protocol

```
                ┌──────────┐
  Left (0x10) ◄─┤  CENTER  ├─► Right (0x11)
                │ (Master) │
                └────┬─────┘
                     │
                GPS (0x12)
                     │
                BMS (0x20)
```

- **Bus speed:** 400 kHz (I2C Fast Mode)
- **Master:** Center display, polls all slaves
- **Message format:** `[0xAA][CMD][LEN][PAYLOAD...][XOR_CHECKSUM]`
- **Polling rate:** ~50 Hz for sensor data, 2 Hz for BMS, 1 Hz for checklist

Left/Right pods use **two separate I2C ports**:
- **Port 0 (Master):** SDA=15, SCL=7 — internal bus for TCA9554 IO expander + GT911 touch
- **Port 1 (Slave):** SDA=4, SCL=16 — external bus to Center unit

Full protocol: [`docs/i2c-protocol.md`](docs/i2c-protocol.md)

---

## Data Points (Sensor Legend)

Every displayable value has a unique 16-bit ID.  The UI renders whichever ID
is assigned to each screen section.

| ID | Name | Unit | Category |
|---|---|---|---|
| `0x0100` | RPM | rpm | Engine |
| `0x0101` | Vehicle Speed | km/h | Engine |
| `0x0102` | Coolant Temp | °C | Engine |
| `0x0106` | Boost Pressure | kPa | Engine |
| `0x0107` | Oil Temp | °C | Engine |
| `0x0108` | Oil Pressure | kPa | Engine |
| `0x010A` | AFR | ratio | Engine |
| `0x0200` | GPS Speed | km/h | GPS |
| `0x0208` | Lap Time | ms | GPS |
| `0x020A` | Lap Delta | ms | GPS |
| `0x0300` | G-Force Lateral | g | IMU |
| `0x0400` | Pack Voltage | V | BMS |
| `0x0402` | SOC | % | BMS |

*(Abbreviated — full table with all 60+ data points in [`docs/data-points.md`](docs/data-points.md))*

---

## Unit Conversion System

All internal values are stored in SI units.  Display conversion happens at
render time based on user preferences stored in `opendash_display_layout_t`.

| Measurement | Internal Unit | Options | Header |
|---|---|---|---|
| Temperature | °C | °C / °F | `opendash_display_config.h` |
| Speed | km/h | km/h / MPH | `opendash_display_config.h` |
| Pressure | kPa | kPa / BAR / PSI | `opendash_display_config.h` |
| Distance | km | km / mi | `opendash_display_config.h` |

Conversion functions are `static inline` in `opendash_ui_styles.h`:

```c
float displayed = opendash_convert_pressure(raw_kpa, layout->pressure_unit);
const char *suffix = opendash_pressure_unit_str(layout->pressure_unit);
```

User preferences persist in NVS (`"od_config"` namespace).

---

## Font System

**Pipeline:** TrueType (`.ttf`) → `lv_font_conv` (npm) → LVGL C arrays → compiled in

| Step | Tool / File | Description |
|---|---|---|
| 1. Source fonts | `common/fonts/ttf/*.ttf` | Drop TrueType files here |
| 2. Configuration | `common/fonts/font_config.json` | Sizes, glyphs, default flag |
| 3. Conversion | `common/fonts/convert_fonts.py` | Runs `lv_font_conv` |
| 4. Output | `common/fonts/output/` | Generated `.c` + auto `opendash_font_config.h` |
| 5. Usage | `opendash_set_font(label, SIZE)` | Apply font in UI code |

Six predefined sizes: 14, 18, 32, 64, 96, 128 px (SMALL → XXXLARGE).

Docs: [`common/fonts/README.md`](common/fonts/README.md) ·
[`docs/FONTS_QUICK_START.md`](docs/FONTS_QUICK_START.md)

---

## Image / Asset Pipeline

**Pipeline:** Source PNG → `convert_images.py` → LVGL C array → compiled in

| Step | Tool / File | Description |
|---|---|---|
| 1. Source images | `common/images/source/<node>/` | Per-node source PNGs |
| 2. Conversion | `common/images/convert_images.py` | Resize + convert to C arrays |
| 3. Output | `<node>/main/assets/` | Generated `.c` files included by build |
| 4. Usage | `lv_image_set_src(img, &background_left)` | Reference in UI code |

Supports per-pod images:  `source/left/`, `source/right/`, `source/pod1/`, etc.
Falls back to `source/leftright/` then `source/` if pod-specific image not found.

Docs: [`common/images/README.md`](common/images/README.md)

---

## Flash Partitioning

Left/Right pods use a custom partition table for 16 MB flash:

| Partition | Type | Offset | Size | Purpose |
|---|---|---|---|---|
| `nvs` | data | `0x9000` | 20 KB | Non-volatile key-value storage |
| `otadata` | data | `0xE000` | 8 KB | OTA boot state |
| `factory` | app | `0x10000` | **2 MB** | Factory firmware |
| `ota_0` | app | `0x210000` | **2 MB** | OTA slot 0 |
| `ota_1` | app | `0x410000` | **2 MB** | OTA slot 1 |
| `storage` | data | `0x611000` | ~10 MB | General storage |

2 MB app partitions accommodate ~1 MB background images compiled into firmware.
Current binary is ~1.05 MB (49% headroom).

File: `left/partitions.csv` , `right/partitions.csv`

---

## NVS (Non-Volatile Storage) Namespaces

| Namespace | Purpose | Key Examples |
|---|---|---|
| `"od_config"` | Display layout & unit preferences | sections[], brightness, pressure_unit |
| `"od_odo"` | Odometer persistence | total_m, trip_a_m, trip_b_m |

Odometer saves to NVS every 100 meters to minimize flash wear.

---

## FreeRTOS Tasks & Threading

### Left/Right Gauge Pods

| Task | Priority | Core | Rate | Description |
|---|---|---|---|---|
| `app_main` (main loop) | Default | 0 | 50 ms | I2C receive → odometer → UI update |
| `ui_task` (LVGL) | 5 | 1 | ~5 ms | `lv_timer_handler()` rendering |
| `button_read_task` | 3 | 0 | 10 ms | Boot button polling (GPIO 0, 50 ms debounce) |

**WDT note:** Odometer UI updates are rate-limited to every 1000 ms (20 × 50 ms
ticks) and guarded by `current_mode != DISPLAY_MODE_ODO` to prevent starving
IDLE0 when updating 10+ LVGL labels on a hidden screen.

### Center Display

| Task | Priority | Core | Description |
|---|---|---|---|
| `ui_task` | 5 | 1 | LVGL rendering |
| `comms_task` | 4 | 0 | I2C master polling loop |
| `data_task` | 3 | 0 | Data processing, alarms, logging |
| `wifi_ble_task` | 2 | 0 | WiFi/BLE management |

### GPS Unit (additional tasks)

| Task | Priority | Core | Description |
|---|---|---|---|
| `gps_task` | 5 | 0 | GNSS data reading + parsing |
| `imu_task` | 5 | 0 | QMI8658 accelerometer + gyroscope |
| `parachute_task` | **6** | 0 | Gyro-triggered safety — highest priority |

---

## Build System

### Dependencies

| Tool | Version | Purpose |
|---|---|---|
| ESP-IDF | v5.3+ (v6.1-dev tested) | Build framework, FreeRTOS, drivers |
| LVGL | 9.2 | Display rendering (managed component) |
| Node.js + npm | Any LTS | `lv_font_conv` for font conversion |
| Python 3 + Pillow | Any | `convert_images.py` for image conversion |
| ImageMagick | Any | Image resizing |

### Build Any Node

```bash
cd <node>/          # center, left, right, or gps
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

### VS Code

Open `opendash.code-workspace` → set target → build from ESP-IDF extension.
See [`docs/vscode-setup.md`](docs/vscode-setup.md).

### Verify Dependencies

```bash
./check_deps.sh
```

---

## Glossary

| Term | Definition |
|---|---|
| **Arc** | 270° circular gauge rendered by LVGL `lv_arc`. Shows primary sensor sweep with white outline. |
| **BMS** | Battery Management System. External rAtTrax node at I2C `0x20`. |
| **Boot Button** | GPIO 0 button on ESP32-S3 boards. Used to cycle display modes (Gauge → Odo → ...). |
| **Center** | Main 4.3" dashboard display. I2C master, aggregates all data. |
| **Common** | Shared C library at `common/`. Included by all four node projects. |
| **Data Point** | A single displayable value identified by a 16-bit ID (e.g., `0x0106` = Boost Pressure). |
| **Display Mode** | A screen layout (Gauge, Odometer, etc.) that the user can cycle through. |
| **ESP-IDF** | Espressif IoT Development Framework. The build system and RTOS for ESP32-S3. |
| **FreeRTOS** | Real-time OS bundled with ESP-IDF. Manages tasks, scheduling, WDT. |
| **GPS Node** | 1.75" AMOLED unit with LC76G GNSS + QMI8658 IMU. |
| **GT911** | Capacitive touch controller IC on the 2.8" display boards. |
| **I2C** | Inter-Integrated Circuit bus. 400 kHz, shared by all four nodes. |
| **LEDC** | ESP32 LED Control peripheral. Used for backlight PWM. |
| **Left/Right** | The two 2.8" round gauge pods flanking the center display. |
| **LVGL** | Light and Versatile Graphics Library (v9.2). Renders all UI elements. |
| **NVS** | Non-Volatile Storage. ESP-IDF key-value store in flash for persistent settings. |
| **OBD2** | On-Board Diagnostics protocol. Center reads engine data via CAN. |
| **Odometer** | Trip/total distance meter. Accumulates from GPS speed, persists in NVS. |
| **Outlined Text** | Text rendered with 4 shadow copies (N/S/E/W offset) behind primary label for readability. |
| **Parachute** | GPS node feature: gyro-triggered deployment system with configurable thresholds. |
| **Pod** | Generic term for a left/right-style gauge display. Supports POD1–POD8 expansion. |
| **Primary Value** | Large numeric readout centered in the arc (XXLARGE font). |
| **PSRAM** | 8 MB pseudo-static RAM on ESP32-S3. Holds LVGL frame buffers. |
| **RGB Interface** | Parallel 16-bit (RGB565) display interface. Used after SPI init on ST7701S. |
| **Secondary Box** | Small info box in the arc gap (bottom). Shows label, value, and unit. |
| **Section** | A configurable region of any screen that displays one data point. |
| **ST7701S** | LCD controller IC on the 2.8" Waveshare boards. Inits via 3-wire SPI, then RGB parallel. |
| **TCA9554** | I2C IO expander on the 2.8" boards. Controls LCD reset and SPI CS pins. |
| **WDT** | Watchdog Timer. Reboots the system if a task starves the IDLE task. |

---

*This document is the single entry point for understanding the OpenDash project.
For detailed information on any topic, follow the links to the specific documentation files listed above.*
