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
┌────────────┬─────────────────────┬────────────┐
│ GPS SPEED  │                     │ LAP TIME   │
├────────────┤   ARC + RPM         ├────────────┤
│ COOLANT °C │    (centered)       │ BOOST kPa  │
├────────────┤                     ├────────────┤
│ OIL TEMP   │                     │ AFR        │
├────────────┴─────────────────────┴────────────┤
│                  Status Bar                   │
└───────────────────────────────────────────────┘
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

## Display Quality Tuning

The display configuration has been carefully tuned to eliminate visual artifacts and ensure crisp rendering. These settings are **critical** for a comfortable viewing experience — incorrect values can cause eye strain and headaches.

### Current Optimized Settings

| Parameter | Value | Notes |
|-----------|-------|-------|
| Pixel Clock | 16 MHz | Waveshare official library value |
| Bounce Buffer | 20 lines | Eliminates PSRAM-related artifacts |
| Font BPP | 4-bit | Best balance for RGB565 displays |
| Timing (H/V) | 4/8/8 | ST7262 datasheet typical values |

### Common Display Issues

| Symptom | Likely Cause | Solution |
|---------|-------------|----------|
| Stripe artifacts / visual "hum" | PSRAM bandwidth | Increase bounce buffer, decrease pixel clock |
| Blurry fonts | Wrong font BPP | Use 4-bit BPP (not 8-bit) |
| Missing symbols (°, ±) | Character range | Update font_config.json range to include 0xB0,0xB1 |
| Display drift on reset | Timing mismatch | Verify pclk_active_neg = true |

### Modifying Display Settings

**WARNING:** Only modify these if you understand RGB LCD timing!

1. Edit `main/display_init.c` — see the header comments for guidance
2. Key defines: `LCD_PIXEL_CLOCK_HZ`, `LCD_BOUNCE_BUFFER_SIZE`
3. Rebuild: `idf.py build && idf.py flash`

### Modifying Fonts

See [`../common/fonts/README.md`](../common/fonts/README.md) for font configuration. Key points:

```bash
cd ../common/fonts
# Edit font_config.json (change sizes, bpp, range, etc.)
python3 convert_fonts.py --force   # REQUIRED to regenerate fonts
cd ../../center
idf.py reconfigure && idf.py build
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
