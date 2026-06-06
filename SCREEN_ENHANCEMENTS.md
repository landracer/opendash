<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OpenDash Center Display — Screen Enhancements

## Overview

The center display now features:

1. **Multi-Screen Support** — Two complete screen layouts with swipeable navigation
2. **Warning Boxes** — Hard solid red/orange flashing indicators on left/right edges
3. **Touch Swipe Navigation** — Swipe left/right to switch screens
4. **Screen Switching** — Boot button integration ready (can be added to hardware handler)

---

## ✨ Feature Details

### 1. Multi-Screen Layouts

#### Screen 1: Engine Metrics
- **Center Gauge:** RPM Arc (0-8000 RPM)
- **Left Column:**
  - GPS Speed (top)
  - Coolant Temperature (middle)
  - Oil Temperature (bottom)
- **Right Column:**
  - Lap Time (top)
  - Boost Pressure (middle)
  - Air-Fuel Ratio (bottom)
- **Navigation:** "Swipe Left for GPS" prompt in status bar

#### Screen 2: GPS/Telemetry Metrics  
- **Center Gauge:** Speed Arc (GPS speed)
- **Left Column:**
  - Satellite Count (top)
  - Altitude (middle)  
  - Latitude (bottom)
- **Right Column:**
  - HDOP (top)
  - Heading (middle)
  - Accuracy (bottom)
- **Navigation:** "Swipe Right for Engine" prompt in status bar

Both screens feature:
- Identical layout structure for consistency
- Hard solid color backgrounds (100% opacity) with no transparency bleed
- Status bar at bottom with screen identifier and navigation hint
- Background image at 30% opacity

---

### 2. Warning Boxes

Warning boxes appear on the **left or right side** of the screen with hard solid colors (no transparency bleed-through):

#### Severity Levels

| Level | Color | RGB Code | Use Case |
|---|---|---|---|
| **Warning (Caution)** | Orange | `0xFF6600` | Non-critical alerts |
| **Critical** | Bright Red | `0xFF0000` | Safety-critical alerts |

#### Activation

Warning boxes flash at **100ms on/off intervals** to draw attention while maintaining visibility.

#### Example Usage

```c
/* Activate critical warning on left side (continuous until cleared) */
ui_manager_warning_box_trigger(0,                           /* position: left */
                                OPENDASH_WARNING_CRITICAL,   /* severity: red */
                                "OVERHEAT",                  /* message: optional */
                                0);                          /* duration: 0=continuous */

/* Activate caution warning on right side for 5 seconds */
ui_manager_warning_box_trigger(1,                           /* position: right */
                                OPENDASH_WARNING_CAUTION,    /* severity: orange */
                                "AFR HIGH",                  /* message: optional */
                                5000);                       /* duration: 5000ms */

/* Clear the warning after it's resolved */
ui_manager_warning_box_clear(0);  /* Clear left side */
ui_manager_warning_box_clear(1);  /* Clear right side */
```

#### Visual Properties

- **Size:** 60px wide × 180px tall
- **Position:** Left/Right perimeter of screen with 5px margin
- **Alignment:** Centered vertically on screen
- **Flash Rate:** 100ms on/off (alternates every 100ms)
- **Opacity:** 100% solid (no transparency)
- **Border:** 4px rounded corners, no outline

---

## 🎮 User Navigation

### Touch Swipe Gestures

| Gesture | Action |
|---|---|
| **Swipe Left** (>100px in <500ms) | Go to next screen (Engine → GPS) |
| **Swipe Right** (>100px in <500ms) | Go to previous screen (GPS → Engine) |

The gesture system is active on all screens and responds to rapid swipe movements.

### Boot Button Integration (Ready for Hardware)

To enable boot button screen switching, add a GPIO handler in `display_init.c` or `main.c`:

```c
/* Example boot button handler */
static void boot_button_handler(void *arg)
{
    ui_manager_next_screen();
    ESP_LOGI(TAG, "Boot button pressed - switched screen");
}

/* In your GPIO initialization */
gpio_config_t boot_button_cfg = {
    .pin_bit_mask = (1ULL << GPIO_NUM_0),  /* Boot button on GPIO0 */
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ONLY,
};
gpio_config(&boot_button_cfg);

/* Install ISR service and attach handler */
gpio_install_isr_service(0);
gpio_isr_handler_add(GPIO_NUM_0, boot_button_handler, NULL);
```

---

## 🔧 API Reference

### Warning Box Functions

#### `ui_manager_warning_box_trigger()`
```c
esp_err_t ui_manager_warning_box_trigger(
    uint8_t position,                          /* 0=left, 1=right */
    opendash_warning_level_t level,            /* OPENDASH_WARNING_CAUTION or CRITICAL */
    const char *message,                       /* Optional message (unused in current version) */
    uint32_t flash_ms                          /* Duration in ms, 0=continuous */
);
```

**Returns:** `ESP_OK` on success

**Parameters:**
- `position`: 0 for left side, 1 for right side
- `level`: `OPENDASH_WARNING_CAUTION` (orange) or `OPENDASH_WARNING_CRITICAL` (red)
- `message`: Optional warning text (reserved for future use)
- `flash_ms`: Flash duration in milliseconds (0 for continuous until cleared)

---

#### `ui_manager_warning_box_clear()`
```c
esp_err_t ui_manager_warning_box_clear(uint8_t position);
```

**Returns:** `ESP_OK` on success

**Parameters:**
- `position`: 0 for left side, 1 for right side

Immediately stops flashing and removes the warning box.

---

### Screen Management Functions

#### `ui_manager_next_screen()`
```c
esp_err_t ui_manager_next_screen(void);
```

**Returns:** `ESP_OK` on success

Transitions to the next screen in sequence. Wraps around after the last screen.

---

#### `ui_manager_get_current_screen()`
```c
uint8_t ui_manager_get_current_screen(void);
```

**Returns:** Current screen index (0 = Engine metrics, 1 = GPS metrics)

---

## 🎨 Styling & Colors

All warning box colors are defined in [opendash_ui_styles.h](common/include/opendash_ui_styles.h):

```c
#define OPENDASH_COLOR_WARNING_BOX_RED      0xFF0000    /* Bright red */
#define OPENDASH_COLOR_WARNING_BOX_ORANGE   0xFF6600    /* Orange */
#define OPENDASH_COLOR_WARNING_BOX_BG       0x000000    /* Black background */
```

To customize colors, edit these defines and rebuild.

---

## 📊 Data Points Available

When implementing data updates, you can use any data point from [data-points.md](docs/data-points.md).

### Popular Engine Data Points

| ID | Data Point | Unit | Range |
|---|---|---|---|
| `0x0100` | RPM | rpm | 0–16,383 |
| `0x0102` | Coolant Temp | °C | -40–215 |
| `0x0107` | Oil Temp | °C | -40–215 |
| `0x0106` | Boost Pressure | kPa | 0–255 |
| `0x010A` | AFR | ratio | 7–23 |
| `0x0200` | GPS Speed | km/h | 0–500 |
| `0x0208` | Lap Time | ms | 0–600000 |

### Popular GPS Data Points

| ID | Data Point | Unit | Range |
|---|---|---|---|
| `0x0200` | GPS Speed | km/h | 0–500 |
| `0x0201` | GPS Heading | ° | 0–360 |
| `0x0204` | Altitude | m | -500–9000 |
| `0x0205` | Satellite Count | count | 0–40 |
| `0x0206` | HDOP | ratio | 0–50 |

---

## 🔌 Integration Examples

### Example 1: Trigger Engine Overheat Warning

```c
/* In your data processing task */
float coolant_temp = read_obd2(0x0102);  /* Read coolant temp */

if (coolant_temp > 100) {
    ui_manager_warning_box_trigger(
        0,                              /* Left side */
        OPENDASH_WARNING_CRITICAL,      /* Red flash */
        "COOLANT",                      /* Message */
        0                               /* Continuous until cleared */
    );
} else {
    ui_manager_warning_box_clear(0);
}
```

### Example 2: AFR Warning (15 Second Duration)

```c
float afr = read_obd2(0x010A);  /* Read AFR */

if (afr > 14.0 || afr < 12.0) {
    ui_manager_warning_box_trigger(
        1,                              /* Right side */
        OPENDASH_WARNING_CAUTION,       /* Orange flash */
        "AFR OUT",                      /* Message */
        15000                           /* Flash for 15 seconds */
    );
}
```

### Example 3: Low Fuel Warning 

```c
float fuel_level = read_obd2(0x0110);  /* Read fuel % */

if (fuel_level < 10) {
    ui_manager_warning_box_trigger(
        0,                              /* Left side */
        OPENDASH_WARNING_CRITICAL,      /* Red flash */
        "LOW FUEL",                     /* Message */
        0                               /* Continuous */
    );
}
```

---

## 🏗️ Implementation Notes

### Performance
- **Screen transitions:** Instant load with no LVGL animation delays
- **Warning flash rate:** 100ms per frame (10 flash cycles per second) for maximum visibility
- **Memory footprint:** ~15KB additional heap for multi-screen structs and timers
- **CPU:** Warning box flashing runs on the UI task (core 1), does not impact data processing

### Touch Responsiveness
- Touch events are processed on every LVGL timer tick (20ms interval)
- Swipe detection requires >100px movement and <500ms duration to prevent accidental triggers
- Both screens support touch events independently

### Compatibility
- LVGL v9.2+ API (uses `lv_timer_get_user_data()`, `lv_obj_add_flag()`, etc.)
- ESP-IDF v5.3+ (uses FreeRTOS task APIs)
- Works with all three display nodes (center, left, right) — inherit from common

---

## ⚙️ Configuration

### Button Sensitivity (Swipe Detection)

Edit [ui_manager.c](center/main/ui_manager.c) line ~526:

```c
/* Current settings */
if (swipe_duration < 500 && swipe_distance < -100) {  /* Swipe left */
```

Adjust:
- **`500`** — Maximum swipe time in milliseconds (default: 500ms)
- **`100`** — Minimum swipe distance in pixels (default: 100px)

### Flash Rate

Edit [ui_manager.c](center/main/ui_manager.c) line ~38:

```c
#define WARNING_BOX_FLASH_MS    100  /* Change flash rate here */
```

Default is 100ms (10 flashes/second). Higher values = slower flash.

### Warning Box Size

Edit [ui_manager.c](center/main/ui_manager.c) lines ~36-37:

```c
#define WARNING_BOX_WIDTH       60    /* Adjust box width */
#define WARNING_BOX_HEIGHT      180   /* Adjust box height */
```

---

## 🐛 Troubleshooting

### Warning Box Not Flashing
- **Check:** Is `ui_manager_warning_box_trigger()` being called?
- **Check:** Is the timeout greater than 0ms? (0ms = continuous, requires explicit clear)
- **Solution:** Verify data source is reading correctly

### Swipe Not Working
- **Check:** Did you update the code in [ui_manager.c](center/main/ui_manager.c)?
- **Check:** Is the swipe distance >100px and duration <500ms?
- **Solution:** Test with slower, longer swipes first

### Compilation Error: `undefined reference`
- **Solution:** Rebuild with `idf.py fullclean && idf.py build`
- **Check:** LVGL v9.x is installed (check `managed_components` folder)

---

## 📝 Code Locations

| Feature | File | Lines |
|---|---|---|
| Screen layouts | [ui_manager.c](center/main/ui_manager.c) | 270–470 |
| Warning boxes | [ui_manager.c](center/main/ui_manager.c) | 550–610 |
| Touch handler | [ui_manager.c](center/main/ui_manager.c) | 510–540 |
| API functions | [ui_manager.h](center/main/ui_manager.h) | 40–100 |
| Color defines | [opendash_ui_styles.h](common/include/opendash_ui_styles.h) | 36–42 |

---

## 🚀 Future Enhancements

Potential additions (not yet implemented):

- [ ] Animation on screen transitions (fade/slide)
- [ ] Boot button handler integration
- [ ] Warning message display in boxes (currently unused parameter)
- [ ] Custom warning box layouts (size, position, shape)
- [ ] Theme switching (dark mode / custom colors)
- [ ] Screen lock (prevent accidental swipe-switching during hard acceleration)

---

## 📚 Related Documentation

- [data-points.md](docs/data-points.md) — Full list of displayable data points
- [architecture.md](docs/architecture.md) — System architecture and data flow
- [i2c-protocol.md](docs/i2c-protocol.md) — Inter-node communication
- [LVGL Documentation](https://docs.lvgl.io/master/) — LVGL API reference
- [ESP-IDF API Reference](https://docs.espressif.com/projects/esp-idf/) — ESP32 APIs

---

**Last Updated:** February 17, 2026  
**Version:** 0.2.0 (Multi-Screen & Warning Boxes)
