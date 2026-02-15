# Display Codebase Synchronization

This document explains how the three display projects (center, left-right, gps) stay synchronized and share common code.

## Architecture Overview

OpenDash uses a **shared component architecture** where common functionality is centralized:

```
opendash/
├── common/              ← Shared component (fonts, protocols, data models)
│   ├── fonts/          ← Font system (shared by all displays)
│   ├── include/        ← Shared headers
│   └── src/            ← Shared implementations
├── center/             ← Center display project
├── left-right/         ← Left/Right gauge projects
└── gps/                ← GPS/Telemetry project
```

## How Code Stays Synchronized

### 1. Common Component System

All three display projects include the `common` directory via `EXTRA_COMPONENT_DIRS`:

**center/CMakeLists.txt, left-right/CMakeLists.txt, gps/CMakeLists.txt:**
```cmake
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../common")
```

This means:
- ✅ Any update to `common/` is automatically available to ALL displays
- ✅ Fonts are converted once and used by all displays
- ✅ Data structures, protocols, and utilities are shared

### 2. Font System (Dynamic & Shared)

The font system is **fully shared** across all displays:

**How it works:**
1. `common/fonts/font_config.json` defines available fonts and the default
2. During build, `common/CMakeLists.txt` runs `convert_fonts.py`
3. Fonts are converted to C files in `common/fonts/generated/`
4. `opendash_font_config.h` is auto-generated with default font declarations
5. All displays include `opendash_fonts.h` and use `opendash_set_font()` helpers

**To change fonts for ALL displays:**
```bash
# 1. Edit the config
vim common/fonts/font_config.json  # Set "default": true on desired font

# 2. Rebuild ANY display project - all will pick up the change
cd center && idf.py build
cd ../left-right && idf.py build
cd ../gps && idf.py build
```

### 3. Image System (Auto-Scaling & Shared)

The image system is **fully automated** with proportional scaling:

**How it works:**
1. Place images in `common/images/source/` with naming: `background_<display>.jpg`, `splash_<display>.png`
2. During build, `common/CMakeLists.txt` runs `convert_images.py`
3. Images are automatically scaled proportionally to fit display resolution (no stretching/skewing)
4. Images are converted to RGB565 C arrays in `common/images/generated/`
5. Displays can include generated headers and use images in LVGL

**Auto-scaling features:**
- ✅ Maintains aspect ratio (no distortion)
- ✅ Centers image if dimensions don't match
- ✅ Fills empty space with black
- ✅ Skip scaling if image is exact resolution

**To add images for displays:**
```bash
# Add a background for center display
cp my-bg.jpg common/images/source/background_center.jpg

# Add splash screens
cp logo.png common/images/source/splash_center.png
cp gps-logo.png common/images/source/splash_gps.png

# Rebuild - images auto-convert and include
cd center && idf.py build
```

**Supported image types:**
- `background_center.jpg` / `background_center.png` - Center display background (800×480)
- `background_leftright.jpg` / `background_leftright.png` - Left/Right gauge background (480×480)
- `background_gps.jpg` / `background_gps.png` - GPS display background (466×466)
- `splash_<display>.jpg` / `splash_<display>.png` - Boot splash screens

### 4. Shared Code Elements

All displays share:

| Component | Location | Purpose |
|-----------|----------|---------|
| **Font System** | `common/fonts/` | TTF conversion, font declarations |
| **Font Headers** | `common/include/opendash_fonts.h` | Font helper functions |
| **Image System** | `common/images/` | JPG/PNG conversion, auto-scaling |
| **I2C Protocol** | `common/src/opendash_i2c_protocol.c` | Communication between displays |
| **Data Models** | `common/src/opendash_data_model.c` | Shared data structures |
| **Display Config** | `common/src/opendash_display_config.c` | Display configuration |
| **Checklist** | `common/src/opendash_checklist.c` | Pre-start checklist logic |

### 4. Display-Specific Code

Each display has its own:

| File | Purpose |
|------|---------|
| `main.c` | Entry point and initialization |
| `display_init.c/h` | Hardware-specific display setup |
| `ui_manager.c/h` | Display-specific UI layout |

**GPS display also has:**
- `gps_handler.c/h` - LC76G GPS module interface
- `imu_handler.c/h` - QMI8658 IMU sensor interface

## Verification Checklist

When making changes to the common codebase, verify synchronization:

### ✅ Font System Changes
- [ ] Update `common/fonts/font_config.json`
- [ ] Run `python3 common/fonts/convert_fonts.py` to test
- [ ] Verify `common/fonts/generated/opendash_font_config.h` is generated
- [ ] All displays will auto-update on next build

### ✅ Image System Changes
- [ ] Place images in `common/images/source/` with correct naming
- [ ] Run `python3 common/images/convert_images.py` to test
- [ ] Verify `.h` and `.c` files are generated in `common/images/generated/`
- [ ] All displays will auto-update on next build

### ✅ Header Changes
- [ ] Update header in `common/include/`
- [ ] If adding new functions, update all display `ui_manager.c` files if needed
- [ ] All displays automatically include updated headers

### ✅ Source Changes
- [ ] Update source in `common/src/`
- [ ] Verify no breaking changes to function signatures
- [ ] All displays automatically link to updated code

### ✅ Building All Displays
```bash
# Verify all displays still compile
cd center && idf.py build && cd ..
cd left-right && idf.py build && cd ..
cd gps && idf.py build && cd ..
```

## Current Status

### Font System Status
- ✅ **All displays use shared font system**
- ✅ **All displays include `opendash_fonts.h`**
- ✅ **All displays use `opendash_set_font()` helper functions**
- ✅ **Default font: engebold (14px, 18px, 32px)**
- ✅ **Available fonts: engebold, montserrat**

### Usage in Each Display

**Center Display** (800×480):
```c
opendash_set_font(rpm_label, OPENDASH_FONT_SIZE_SMALL);      // 14px
opendash_set_font(value_label, OPENDASH_FONT_SIZE_MEDIUM);   // 18px
opendash_set_font(status_label, OPENDASH_FONT_SIZE_SMALL);   // 14px
```

**Left/Right Gauges** (480×480 round):
```c
opendash_set_font(primary_label, OPENDASH_FONT_SIZE_SMALL);     // 14px
opendash_set_font(primary_value, OPENDASH_FONT_SIZE_LARGE);     // 32px
opendash_set_font(secondary_value, OPENDASH_FONT_SIZE_MEDIUM);  // 18px
```

**GPS Display** (466×466 round AMOLED):
```c
opendash_set_font(speed_label, OPENDASH_FONT_SIZE_LARGE);    // 32px
opendash_set_font(laptime_label, OPENDASH_FONT_SIZE_MEDIUM); // 18px
opendash_set_font(sat_label, OPENDASH_FONT_SIZE_SMALL);      // 14px
```

## Best Practices for Copilot

When modifying OpenDash code:

1. **Always update `common/` first** if changes affect multiple displays
2. **Test font changes** by running the converter: `cd common/fonts && python3 convert_fonts.py`
3. **Check all display projects** compile after common changes
4. **Document breaking changes** in this file
5. **Keep display-specific code in display directories** - only shared code in `common/`

## Quick Reference

**To add a new font:**
1. Copy `.ttf` file to `common/fonts/ttf/`
2. Add entry to `common/fonts/font_config.json`
3. Rebuild - all displays get the new font

**To change default font:**
1. Edit `common/fonts/font_config.json`
2. Set `"default": true` on desired font
3. Rebuild - all displays use new default

**To verify synchronization:**
```bash
# Check all displays use the same headers
grep -r "opendash_fonts.h" */main/*.c

# Check all displays use the same functions
grep -r "opendash_set_font" */main/*.c

# Build all projects
for dir in center left-right gps; do
  cd $dir && idf.py build && cd ..
done
```

---

**Last Updated:** 2026-02-15  
**Status:** All displays synchronized and using dynamic font system
