<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# [LEGACY] OpenDash — Left/Right Gauges (2.8" Round LCD)

> **LEGACY — DO NOT BUILD.** This is the unified pre-split pod firmware; it was
> superseded by the separate [`left/`](../left/README.md) and
> [`right/`](../right/README.md) projects (which carry all current features:
> batched ESP-NOW, MD/OBD domain split, BLE OTA recipe). Kept only as the
> historical record of the pre-split architecture.
>
> **Last updated:** 2026-10-21 (audited — every inter-node claim below is now written in PAST tense on
> purpose: this generation predates the ESP-NOW transport. The current system is ESP-NOW-only; I2C is
> only ever a local peripheral bus inside one enclosure. See [`docs/espnow-protocol.md`](../docs/espnow-protocol.md).)

This is the Left and Right Gauge Pods project for OpenDash, running on the Waveshare ESP32-S3-LCD-2.8C hardware.

## Hardware Specifications

- **Board:** Waveshare ESP32-S3-LCD-2.8C
- **Display:** 480×480 Round IPS LCD (ST7701 controller)
- **MCU:** ESP32-S3 dual-core @ 240MHz
- **Flash:** 16MB
- **PSRAM:** 8MB Octal SPI
- **Role (of that era):** display-only slave — data was presented from Center unit pushes
- **Inter-node transport (of that era):** a wired I2C bus — **retired design**; the
  fleet has since moved to ESP-NOW (wireless), and no wired inter-node bus exists today

## Features

- **Professional Round Layout:** Circular arc gauge with primary and secondary data displays
- **Configurable Data Points:** Display any two data points from the system
- **Legacy bus:** display commands arrived from the Center unit over the then-wired I2C bus (historical)
- **Low Power:** Efficient rendering optimized for gauge display

## Building & Flashing

### Prerequisites

1. **ESP-IDF v6.1** installed
2. **Visual Studio Code** with ESP-IDF extension (recommended)
3. **USB-C cable** for programming and power

### Command Line Build

```bash
cd left-right/
source ~/esp/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

### Visual Studio Code Build

1. Open this folder in VS Code
2. Press **F1** → **ESP-IDF: Set Espressif device target** → **ESP32-S3**
3. Press **F1** → **ESP-IDF: Build your project**
4. Press **F1** → **ESP-IDF: Flash your project**
5. Press **F1** → **ESP-IDF: Monitor device**

## Project Structure

```
left-right/
├── CMakeLists.txt                # Main project CMake file
├── sdkconfig                     # Committed build truth (defaults mechanism retired 2026-10-08)
├── README.md                     # This file
└── main/
    ├── CMakeLists.txt            # Main component CMake file
    ├── idf_component.yml         # Component dependencies
    ├── main.c                    # Application entry point
    ├── display_init.c/h          # Display hardware initialization
    └── ui_manager.c/h            # LVGL UI management
```

## Default UI Layout

```
        ┌──────────────────┐
       ╱    Section A       ╲
      │   (Primary Value)    │
      │   Large numeric      │
      │      OIL TEMP        │
      │         90°C         │
      │                      │
      │ ┌──────────────────┐ │
      │ │   Section B      │ │
      │ │ (Secondary Value)│ │
      │ │    BOOST         │ │
      │ │    95 kPa        │ │
      │ └──────────────────┘ │
       ╲   Arc Gauge Bar    ╱
        └──────────────────┘
```

## Left vs. Right Node (pre-split design — historical)

In this legacy generation the same firmware served both pods, and node identity was determined by:

1. **GPIO Configuration (of the era):** a GPIO pin was wired high/low to select Left vs. Right
2. **NVS Configuration (of the era):** the node ID was stored in NVS
3. **I2C address (retired bus design):** Left was 0x10, Right was 0x11 — the wired-bus
   addressing was retired; current nodes are addressed by 48-bit WiFi MAC and identified
   logically by `opendash_node_t` (see [`docs/espnow-protocol.md`](../docs/espnow-protocol.md))

## Configuration (of that era)

Display configuration was stored in NVS and could be modified via:
- Center unit (over the then-wired inter-node bus — today this is ESP-NOW)
- Companion app (over WiFi/BLE)
- Direct flashing with custom configuration

## Next Steps (as they stood at freeze time — historical record)

These are what this pre-split firmware still lacked when it was superseded; the
successor `left/` + `right/` ESP-NOW lines are where that work actually landed.

- **Add inter-node command handling:** respond to pushes from Center unit
- **Add node ID detection:** auto-detect Left vs. Right from GPIO
- **Add dynamic data updates:** update display values from inbound data

## License

See main repository README for license information.

---

**OpenDash Left/Right Gauges** — Built for racers, by racers. 🏎️💨
