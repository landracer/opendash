# OpenDash — Center Display (4.3" LCD)

This is the Center Display project for OpenDash, running on the Waveshare ESP32-S3-Touch-LCD-4.3 hardware.

## Hardware Specifications

- **Board:** Waveshare ESP32-S3-Touch-LCD-4.3
- **Display:** 800×480 IPS LCD (ST7262 controller)
- **Touch:** Capacitive touch (GT911)
- **MCU:** ESP32-S3 dual-core @ 240MHz
- **Flash:** 16MB
- **PSRAM:** 8MB Octal SPI
- **Role:** I2C Master, main display coordinator

## Features

- **Professional Layout:** RPM arc gauge, 6-section data grid, status bar
- **Configurable Data Points:** Each section can display any data point
- **Touch Interface:** Tap to configure, swipe to navigate
- **I2C Master:** Coordinates communication with Left, Right, and GPS displays
- **OBD2/CAN Support:** Direct connection to vehicle ECU

## Building & Flashing

### Prerequisites

1. **ESP-IDF v5.3** installed
2. **Visual Studio Code** with ESP-IDF extension (recommended)
3. **USB-C cable** for programming and power

### Command Line Build

```bash
cd center/
source ~/esp/esp-idf/export.sh   # Set up ESP-IDF environment
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

### Visual Studio Code Build

1. Open this folder in VS Code
2. Press **F1** and select **ESP-IDF: Set Espressif device target**
3. Select **ESP32-S3**
4. Press **F1** and select **ESP-IDF: Build your project**
5. Press **F1** and select **ESP-IDF: Flash your project**
6. Press **F1** and select **ESP-IDF: Monitor device**

## Project Structure

```
center/
├── CMakeLists.txt                # Main project CMake file
├── sdkconfig.defaults            # ESP-IDF configuration defaults
├── README.md                     # This file
└── main/
    ├── CMakeLists.txt            # Main component CMake file
    ├── idf_component.yml         # Component dependencies (LVGL, etc.)
    ├── main.c                    # Application entry point
    ├── display_init.c/h          # Display hardware initialization
    └── ui_manager.c/h            # LVGL UI management
```

## Default UI Layout

```
┌──────────────────────────────────────────────────────────┐
│  RPM Bar — Full Width Arc (Top Section)                  │
├──────────────┬───────────────────────┬───────────────────┤
│  Section A   │     Section B         │    Section C      │
│  Coolant °C  │     SPEED (GPS)       │    Boost kPa      │
│              │    (Large numeric)    │                   │
├──────────────┼───────────────────────┼───────────────────┤
│  Section D   │     Section E         │    Section F      │
│  Oil Temp    │    Lap Time/Delta     │    AFR            │
│              │                       │                   │
├──────────────┴───────────────────────┴───────────────────┤
│  Status Bar — Warnings, Alarms, Checklist Status         │
└──────────────────────────────────────────────────────────┘
```

## Configuration

Display configuration is stored in NVS (Non-Volatile Storage) and can be modified:

1. **Via Touch Interface:** Tap and hold a section to reconfigure it
2. **Via Companion App:** Connect over WiFi/BLE to modify settings
3. **Via Code:** Modify default layout in `opendash_config_reset_defaults()`

## Data Points

Each section can display any data point from the system. See [`../docs/data-points.md`](../docs/data-points.md) for the full list of available data points.

## Troubleshooting

### Display not turning on

- Check USB-C power connection
- Verify ESP32-S3 is properly powered
- Check serial output for initialization errors

### Touch not responding

- Ensure GT911 touch controller is properly initialized
- Check I2C connections to touch controller
- Verify touch interrupt GPIO is configured

### Build errors

```bash
# Clean build artifacts
idf.py fullclean

# Reconfigure and rebuild
idf.py set-target esp32s3
idf.py build
```

## Next Steps

- **Add I2C Master Implementation:** Poll Left, Right, and GPS displays
- **Add OBD2/CAN Interface:** Read live engine data
- **Add Touch Handling:** Implement touch gestures for navigation
- **Add Configuration Menu:** Allow on-device configuration changes
- **Add WiFi/BLE:** Enable OTA updates and companion app connectivity

## License

See main repository README for license information.

---

**OpenDash Center Display** — Built for racers, by racers. 🏎️💨
