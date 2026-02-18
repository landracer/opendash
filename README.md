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

## Display System Architecture

### Single-Screen, Multi-Display-Mode Design

The Center Display uses a **resource-efficient architecture** with one LVGL screen object that cycles through multiple display modes:

- ✅ **Minimal Memory Footprint:** Single screen tree = fewer LVGL objects
- ✅ **Maximum CPU Efficiency:** No object recreation on mode switch
- ✅ **Flexible Extensibility:** Add unlimited display modes without memory bloat
- ✅ **Thread-Safe:** All UI objects created once during init (on UI task)

### Display Modes

Each display mode shows the same physical layout with different data:

| Mode | Center Gauge | Left Column | Right Column |
|------|--------------|-------------|--------------|
| **ENGINE** | RPM Arc | Coolant °C, GPS Speed, Boost kPa | Oil Temp °C, Lap Time, AFR |
| **GPS** | Speed Arc | Altitude m, Sat Count, Heading ° | Latitude, HDOP, Accuracy |

**Operation:**
- Press **Boot Button (GPIO0)** to cycle between modes
- Screen shows ~2s per mode by default (customizable)
- Data updates in real-time regardless of mode
- All sections display placeholder "---" until data is available

### Default UI Layout

```
┌──────────────┬──────────────────────┬──────────────┐
│   LABEL 0    │                      │   LABEL 3    │
│              │   CENTER ARC         │              │
│   VALUE 0    │  (RPM or Speed)      │   VALUE 3    │
│   Max: ___   │                      │   Max: ___   │
├──────────────┼──────────────────────┼──────────────┤
│   LABEL 1    │                      │   LABEL 4    │
│   VALUE 1    │                      │   VALUE 4    │
│   Max: ___   │                      │   Max: ___   │
├──────────────┼──────────────────────┼──────────────┤
│   LABEL 2    │                      │   LABEL 5    │
│   VALUE 2    │                      │   VALUE 5    │
│   Max: ___   │                      │   Max: ___   │
├──────────────┴──────────────────────┴──────────────┤
│              Status Bar (Mode Indicator)           │
└─────────────────────────────────────────────────────┘
```

## Creating Custom Display Modes

### Standard Practice: Data-Only Customization

**Most common use case:** Change which data points appear in each section without modifying layout.

**Steps:**

1. **Edit `main/ui_manager.c`** — Find the `mode_configs` array:

```c
static const display_mode_config_t mode_configs[DISPLAY_MODE_COUNT] = {
    [DISPLAY_MODE_ENGINE] = {
        .section_labels = {"COOLANT °C", "GPS SPEED", "BOOST kPa", "OIL TEMP °C", "LAP TIME", "AFR"},
        .status_text = "MODE: ENGINE | Press boot button to switch"
    },
    [DISPLAY_MODE_GPS] = {
        .section_labels = {"ALTITUDE m", "SAT COUNT", "HEADING °", "LATITUDE", "HDOP", "ACCURACY"},
        .status_text = "MODE: GPS | Press boot button to switch"
    }
    /* ADD NEW MODES HERE */
};
```

2. **Add a new mode to the enum:**

```c
typedef enum {
    DISPLAY_MODE_ENGINE = 0,
    DISPLAY_MODE_GPS = 1,
    DISPLAY_MODE_CUSTOM = 2,      /* NEW MODE */
    DISPLAY_MODE_COUNT = 3         /* UPDATE THIS */
} display_mode_t;
```

3. **Add configuration for the new mode:**

```c
[DISPLAY_MODE_CUSTOM] = {
    .section_labels = {"YOUR LABEL 0", "YOUR LABEL 1", "YOUR LABEL 2", 
                       "YOUR LABEL 3", "YOUR LABEL 4", "YOUR LABEL 5"},
    .status_text = "MODE: CUSTOM | Press boot button to switch"
}
```

4. **Update `ui_manager.h`** — Change `DISPLAY_MODE_COUNT` if you modified the enum

5. **Rebuild and flash:**
```bash
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

### Advanced: Custom Layout Design

**For completely different layouts** (different section sizes, positions, gauges, etc.):

This requires **custom LVGL implementation**. The `display_init.c` provides several examples you can extend:

- `create_rpm_arc()` — Speed/RPM gauge pattern
- `create_data_section()` — Standard data box with label/value/max
- `create_screen_layout()` — Main layout structure

**Resources for custom LVGL development:**

- 📖 **LVGL 9.x Docs:** https://docs.lvgl.io/master/
- 💡 **LVGL Examples:** https://docs.lvgl.io/master/examples.html
- 🔧 **LVGL GitHub:** https://github.com/lvgl/lvgl
- 📚 **Widget Documentation:** https://docs.lvgl.io/master/widgets/index.html

**General approach:**

1. Create a new function `create_screen_custom_layout()` in `ui_manager.c`
2. Design your LVGL widgets (arcs, bars, gauges, images, etc.)
3. Store references in the `screen_layout` struct for later updates
4. Call from `ui_manager_init()` during boot
5. Update `ui_manager_update_value()` to route data to your custom widgets

**Thread Safety:** All LVGL object creation must happen on the UI task (core 1) with LVGL mutex locked. See display_init.c for examples of `display_lvgl_lock()` usage.

---

### Future: Standard Layout Switcher (TODO)

Once multiple layout templates are available from the community, implement:

- **Layout Selection Menu:** Boot-time or runtime picker
- **Layout Registry:** Define available layouts with metadata
- **Dynamic Loading:** Support multiple layout .c files
- **Configuration Storage:** Save user's layout choice to NVS

This will allow end-users to **quickly switch between professional templates** without coding.



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
