<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# Font System Testing Guide

This guide explains how to test the new automatic font conversion system in OpenDash.

## Prerequisites

- ESP-IDF v5.3 installed and configured
- Node.js and npm installed
- Python 3 installed

## Quick Test

### 1. Test Font Converter Directly

```bash
cd common/fonts
python3 convert_fonts.py --check
```

This checks if all dependencies are installed.

### 2. Convert Fonts Manually

```bash
cd common/fonts
python3 convert_fonts.py
```

This will:
- Install lv_font_conv if needed
- Convert any fonts defined in font_config.json
- Generate C files in the generated/ directory

### 3. Build a Project

```bash
cd center  # or left-right, or gps
idf.py build
```

The build system will automatically:
1. Run the font converter as a pre-build step
2. Include any generated fonts in the build
3. Compile the project with the new fonts

## Testing the Alignment Fix

The center display grid should now be properly centered:

### Expected Layout
- Display width: 800px
- Grid total width: 770px (3 sections × 250px + 2 gaps × 10px)
- Left margin: 15px
- Right margin: 15px

### Before Fix
- Left margin: 20px
- Right margin: 10px
- Result: Content appeared shifted left

### After Fix
- Left margin: 15px
- Right margin: 15px
- Result: Content is properly centered

## Adding a Custom Font

### Step 1: Add Font File
```bash
# Copy your TTF file to the fonts directory
cp ~/Downloads/YourFont.ttf common/fonts/ttf/
```

### Step 2: Configure Font
Edit `common/fonts/font_config.json`:

```json
{
  "fonts": [
    {
      "name": "montserrat",
      "source": "built-in",
      "comment": "Built-in LVGL font",
      "sizes": [14, 18, 24, 32],
      "bpp": 4,
      "range": "0x20-0x7F"
    },
    {
      "name": "yourfont",
      "source": "YourFont.ttf",
      "sizes": [16, 24, 32],
      "bpp": 4,
      "range": "0x20-0x7F"
    }
  ]
}
```

### Step 3: Convert Font
```bash
cd common/fonts
python3 convert_fonts.py
```

### Step 4: Use in Code
Edit your ui_manager.c:

```c
// Declare the font
LV_FONT_DECLARE(yourfont_24);

// Use the font
lv_obj_t *label = lv_label_create(parent);
lv_obj_set_style_text_font(label, &yourfont_24, 0);
lv_label_set_text(label, "Hello with custom font!");
```

Or use the helper function:
```c
#include "opendash_fonts.h"

// This still uses built-in fonts, but the infrastructure is ready
lv_obj_t *label = lv_label_create(parent);
opendash_set_font(label, OPENDASH_FONT_SIZE_LARGE);
lv_label_set_text(label, "Hello!");
```

### Step 5: Build
```bash
cd center  # or left-right, or gps
idf.py build
```

## Verification Steps

### 1. Font Conversion
After running `convert_fonts.py`, check:
```bash
ls -lh common/fonts/generated/
```

You should see generated `.c` files for each font size.

### 2. Build Output
During build, you should see:
```
Font Converter Output:
OpenDash Font Converter
============================================================
[SUCCESS] Node.js is installed
[SUCCESS] lv_font_conv is installed
[SUCCESS] Loaded font configuration with X font(s)
...
```

### 3. Runtime
When you flash and run the firmware:
- Center display should show properly centered layout
- All text should be readable with the configured fonts
- No font-related errors in logs

## Troubleshooting

### Font converter fails
```bash
# Install dependencies manually
sudo apt-get install nodejs npm
npm install -g lv_font_conv
```

### Build fails with font errors
```bash
# Clean build and retry
cd center
idf.py fullclean
idf.py build
```

### Generated fonts not found
```bash
# Manually run font converter
cd common/fonts
python3 convert_fonts.py --force
```

### Font looks wrong
- Check the bpp value (4 is recommended for antialiased)
- Check the size value
- Verify the Unicode range includes your characters

## Performance Notes

- Font conversion runs once during CMake configuration
- If fonts haven't changed, conversion is skipped
- Use `--force` flag to force re-conversion
- Generated C files are cached in `generated/` directory

## Size Optimization

Fonts can be large. To minimize size:
1. Only convert sizes you actually use
2. Limit Unicode range to needed characters
3. Use lower bpp for simple displays (1 or 2 instead of 4)
4. Consider using built-in LVGL fonts when possible

Example minimal config:
```json
{
  "name": "compact",
  "source": "YourFont.ttf",
  "sizes": [14],
  "bpp": 2,
  "range": "0x20-0x7E"
}
```

This generates only one 14px font with 2-bit antialiasing for basic ASCII.
