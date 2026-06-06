<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OpenDash — Left/Right Gauges (2.8" Round LCD)

This is the Left and Right Gauge Pods project for OpenDash, running on the Waveshare ESP32-S3-LCD-2.8C hardware.

## Hardware Specifications

- **Board:** Waveshare ESP32-S3-LCD-2.8C
- **Display:** 480×480 Round IPS LCD (ST7701 controller)
- **MCU:** ESP32-S3 dual-core @ 240MHz
- **Flash:** 16MB
- **PSRAM:** 8MB Octal SPI
- **Role:** I2C Slave, displays data from Center unit

## Features

- **Professional Round Layout:** Circular arc gauge with primary and secondary data displays
- **Configurable Data Points:** Display any two data points from the system
- **I2C Slave:** Receives display commands from the Center unit
- **Low Power:** Efficient rendering optimized for gauge display

## Building & Flashing

### Prerequisites

1. **ESP-IDF v5.3** installed
2. **Visual Studio Code** with ESP-IDF extension (recommended)
3. **USB-C cable** for programming and power

### Command Line Build

```bash
cd left-right/
source ~/esp/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
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
├── sdkconfig.defaults            # ESP-IDF configuration defaults
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

## Left vs. Right Node

The same firmware can be used for both Left and Right gauge pods. The node ID is determined by:

1. **GPIO Configuration:** Set a GPIO pin high/low to select Left vs. Right
2. **NVS Configuration:** Store the node ID in NVS
3. **I2C Address:** Left uses 0x10, Right uses 0x11

## Configuration

Display configuration is stored in NVS and can be modified via:
- Center unit (over I2C)
- Companion app (over WiFi/BLE)
- Direct flashing with custom configuration

## Next Steps

- **Add I2C Slave Implementation:** Respond to commands from Center unit
- **Add Node ID Detection:** Auto-detect Left vs. Right from GPIO
- **Add Dynamic Data Updates:** Update display values from I2C data

## License

See main repository README for license information.

---

**OpenDash Left/Right Gauges** — Built for racers, by racers. 🏎️💨
