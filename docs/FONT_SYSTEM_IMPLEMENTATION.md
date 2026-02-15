# OpenDash Font System and Display Alignment Updates

## Summary

This update addresses three key requirements:
1. **Code consistency** across all three displays (Center, Left-Right, GPS)
2. **Fixed center display alignment** - content was shifted left, now properly centered
3. **Automatic TrueType font conversion** - integrated lv_font_conv into the build system

## Changes Made

### 1. Center Display Alignment Fix

**Problem**: The center display (800×480) had its 6-section grid shifted to the left with:
- Left margin: 20px
- Right margin: 10px (unbalanced)

**Solution**: Calculated proper centered positioning:
- Grid width: 770px (3 sections × 250px + 2 gaps × 10px)
- Centered start_x: (800 - 770) / 2 = 15px
- Now has balanced 15px margins on both sides

**File**: `center/main/ui_manager.c`

### 2. Code Consistency Across Displays

**Review**: All three displays already follow consistent patterns:
- Same initialization sequence (NVS → config → display → UI → tasks)
- Similar UI manager structure with `ui_manager_init()` and `ui_manager_start()`
- Consistent use of LVGL components and styling
- All use the same data model and display configuration system

**Files**:
- `center/main/main.c` - Center display entry point
- `left-right/main/main.c` - Left/Right gauge entry point
- `gps/main/main.c` - GPS display entry point

### 3. Automatic Font Conversion System

**Goal**: Allow users to add TrueType fonts without manual conversion.

**Implementation**:

#### Directory Structure
```
common/fonts/
├── README.md              # Comprehensive documentation
├── font_config.json       # Font configuration file
├── convert_fonts.py       # Python conversion script
├── ttf/                   # Source TTF/OTF files (user-added)
└── generated/             # Auto-generated C files (git-ignored)
```

#### Components

**a) Font Converter Script** (`convert_fonts.py`)
- Checks for Node.js and lv_font_conv
- Auto-installs lv_font_conv if missing
- Reads `font_config.json` for font specifications
- Converts fonts only if needed (change detection)
- Generates LVGL-compatible C source files
- Provides colored terminal output for easy debugging

**b) Configuration File** (`font_config.json`)
```json
{
  "fonts": [
    {
      "name": "montserrat",
      "source": "built-in",
      "sizes": [14, 18, 24, 32],
      "bpp": 4,
      "range": "0x20-0x7F"
    }
  ]
}
```

**c) CMake Integration** (`common/CMakeLists.txt`)
- Finds Python3 interpreter
- Runs `convert_fonts.py` as a pre-build step
- Collects generated `.c` files with `file(GLOB)`
- Includes them in component sources
- Adds `fonts/generated` to include directories
- Displays converter output and warnings

**d) Font Helper Header** (`opendash_fonts.h`)
- Provides `opendash_font_size_t` enum (SMALL, MEDIUM, LARGE)
- Helper function `opendash_get_font()` for size-based font selection
- Helper function `opendash_set_font()` for easy font application
- Currently maps to LVGL built-in Montserrat fonts
- Ready for custom font integration

**e) Updated UI Managers**
All three displays now use the font helper:
```c
#include "opendash_fonts.h"

// Instead of:
lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);

// Now use:
opendash_set_font(label, OPENDASH_FONT_SIZE_SMALL);
```

**Files Updated**:
- `center/main/ui_manager.c`
- `left-right/main/ui_manager.c`
- `gps/main/ui_manager.c`

### 4. Documentation

**New Documentation Files**:
- `common/fonts/README.md` - Font system user guide
- `docs/font-system-testing.md` - Testing and verification guide

**Updated Files**:
- None (QUICKSTART.md and other docs still accurate)

## How It Works

### Build Process Flow

```
1. User runs: idf.py build
2. CMake configures project
   ↓
3. CMake runs convert_fonts.py
   ↓
4. Script checks if lv_font_conv installed
   ├─ If not: installs via npm
   └─ If yes: continues
   ↓
5. Script reads font_config.json
   ↓
6. For each font:
   ├─ Check if TTF exists
   ├─ Check if conversion needed (change detection)
   ├─ If needed: run lv_font_conv
   └─ Generate .c file in generated/
   ↓
7. CMake collects generated/*.c files
   ↓
8. Build continues with fonts included
```

### User Workflow for Custom Fonts

1. Copy `MyFont.ttf` to `common/fonts/ttf/`
2. Edit `common/fonts/font_config.json`:
   ```json
   {
     "name": "myfont",
     "source": "MyFont.ttf",
     "sizes": [16, 24],
     "bpp": 4,
     "range": "0x20-0x7F"
   }
   ```
3. Build project: `idf.py build`
4. Use in code:
   ```c
   LV_FONT_DECLARE(myfont_24);
   lv_obj_set_style_text_font(label, &myfont_24, 0);
   ```

## Technical Details

### Font Conversion Tool

**lv_font_conv** is the official LVGL font converter:
- Developed by the LVGL team
- Converts TTF/OTF/WOFF to LVGL C format
- Supports custom Unicode ranges
- Configurable bits-per-pixel (antialiasing)
- Generates optimized bitmap fonts

**Why not font-to-c.py?**
- font-to-c generates generic C bitmaps, not LVGL format
- lv_font_conv generates fonts compatible with LVGL 9.2
- lv_font_conv is the standard tool in LVGL ecosystem
- Better integration with LVGL's rendering engine

### LVGL Font Format

Generated fonts include:
- Glyph bitmaps (character pixel data)
- Glyph descriptors (width, height, advance)
- Font metadata (baseline, line height)
- Kerning tables (optional)
- Unicode mapping tables

### Font Sizes

**Recommended sizes per display:**

| Display | Small | Medium | Large |
|---------|-------|--------|-------|
| Center (800×480) | 14-16px | 18-24px | 32-48px |
| Left/Right (480×480) | 14-16px | 24-32px | 48-64px |
| GPS (466×466) | 14-16px | 20-24px | 32-40px |

### Memory Considerations

Font size in flash depends on:
- **Font size in pixels**: Larger = more data
- **Character range**: More characters = more data
- **Bits per pixel**: 4bpp ≈ 4× larger than 1bpp
- **Font complexity**: Detailed fonts = more data

Example sizes (approximate):
- 14px, 4bpp, ASCII (0x20-0x7F): ~8 KB
- 24px, 4bpp, ASCII: ~15 KB
- 32px, 4bpp, ASCII: ~25 KB
- 48px, 4bpp, ASCII: ~50 KB

**Optimization tips:**
- Use only needed sizes
- Limit character range
- Use 2bpp for simple displays
- Share fonts across projects

## Testing

### Font Converter Test
```bash
cd common/fonts
python3 convert_fonts.py --check  # Check setup
python3 convert_fonts.py          # Convert fonts
```

### Build Test
```bash
cd center  # or left-right, or gps
idf.py build
```

### Expected Output
```
-- Found Python3: /usr/bin/python3 (found version "3.x.x")
-- OpenDash Font Converter
-- ============================================================
-- [SUCCESS] Node.js is installed
-- [SUCCESS] lv_font_conv is installed
-- [SUCCESS] Loaded font configuration with 1 font(s)
-- [INFO] Skipping built-in font: montserrat
-- ============================================================
-- [SUCCESS] Font conversion completed successfully!
```

## Backward Compatibility

✅ **Fully backward compatible**
- Uses LVGL built-in fonts by default
- Font conversion only runs if configured
- No changes required to existing code
- Can still use hardcoded font references

## Future Enhancements

Possible future improvements:
1. Web-based font configuration tool
2. Font preview generator
3. Automatic font optimization (subset generation)
4. Support for icon fonts (Material Icons, FontAwesome)
5. Font caching across builds
6. Custom font fallback chains

## References

- **LVGL Documentation**: https://docs.lvgl.io/9.2/overview/font.html
- **lv_font_conv**: https://github.com/lvgl/lv_font_conv
- **FreeType**: https://freetype.org/
- **ESP-IDF Build System**: https://docs.espressif.com/projects/esp-idf/en/v5.3/esp32s3/api-guides/build-system.html

## Credits

- Font conversion uses lv_font_conv by the LVGL team
- Inspired by the font-to-c repository for the integration approach
- Built for the OpenDash ESP32-S3 dashboard system
