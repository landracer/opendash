# OpenDash Custom Fonts

This directory contains the font management system for OpenDash.

## Quick Start: Changing Fonts

**To change the default font system-wide:**

1. Edit `font_config.json`
2. Set `"default": true` on the font you want to use
3. Rebuild the project

That's it! All UI elements will automatically use the new font.

## Directory Structure

- `ttf/` - Place your TrueType (.ttf) or OpenType (.otf) font files here
- `generated/` - Auto-generated C files for LVGL (do not edit manually)
- `font_config.json` - Font conversion configuration

## How the Font System Works

The font system is **fully dynamic**:

1. `font_config.json` defines available fonts and marks one as "default"
2. The build system runs `convert_fonts.py` which:
   - Converts TTF fonts to LVGL C format
   - Generates `opendash_font_config.h` with declarations for the default font
3. `opendash_fonts.h` includes the generated config and provides helper functions
4. All UI code uses `opendash_set_font()` which automatically uses the default font

**No code changes needed to switch fonts!** Just update `font_config.json` and rebuild.

## Font Configuration Format

The `font_config.json` file defines which fonts to convert:

```json
{
  "fonts": [
    {
      "name": "engebold",
      "source": "engebold.ttf",
      "sizes": [14, 18, 32],
      "bpp": 4,
      "range": "0x20-0x7F",
      "default": true
    },
    {
      "name": "opensans",
      "source": "OpenSans-Regular.ttf",
      "sizes": [14, 18, 32],
      "bpp": 4,
      "range": "0x20-0x7F"
    }
  ]
}
```

### Configuration Options

- `name`: Base name for the generated font (will create `{name}_{size}`)
- `source`: Font file name in the `ttf/` directory
- `sizes`: Array of font sizes in pixels to generate (must include 14, 18, and 32)
- `bpp`: Bits per pixel (1=monochrome, 2=2-bit, 4=4-bit antialiased, 8=8-bit)
- `range`: Unicode character range (default: "0x20-0x7F" for basic ASCII)
- `default`: Set to `true` to make this the system-wide default font (only one font should have this)

### Character Ranges

Common Unicode ranges:
- `0x20-0x7F` - Basic ASCII (printable characters)
- `0x20-0x7E,0xB0,0xB1,0xB7,0x2022` - ASCII + common symbols (°, ±, ·, •)
- `0x20-0xFF` - Latin-1 Supplement (includes accented characters)
- `0x20-0x7F,0x400-0x4FF` - ASCII + Cyrillic

## Automatic Conversion

The build system automatically:
1. Checks if `lv_font_conv` is installed (installs locally via npm if missing)
2. Scans `font_config.json`
3. Converts any fonts that are missing or outdated
4. Generates `opendash_font_config.h` with the default font configuration
5. Includes all fonts in the build

**Note:** The font conversion tool installs `lv_font_conv` locally in the `fonts` directory to avoid requiring root/sudo permissions. The `node_modules` directory is automatically ignored by git.

## Manual Conversion

If you need to manually convert a font:

```bash
cd common/fonts
# Install lv_font_conv locally (no sudo required)
npm install lv_font_conv
# Run conversion
./node_modules/.bin/lv_font_conv --font ttf/YourFont.ttf --size 24 \
  --range 0x20-0x7F --format lvgl --bpp 4 \
  --output generated/your_font_24.c
```

Alternatively, you can install globally if you have permissions:
```bash
sudo npm install -g lv_font_conv
lv_font_conv --font ttf/YourFont.ttf --size 24 \
  --range 0x20-0x7F --format lvgl --bpp 4 \
  --output generated/your_font_24.c
```

## Using Fonts in Code

### Using the Default Font (Recommended)

The easiest way to use fonts is through the helper functions, which automatically use the default font from `font_config.json`:

```c
#include "opendash_fonts.h"

// Create a label
lv_obj_t *label = lv_label_create(parent);

// Set font using size category - automatically uses default font
opendash_set_font(label, OPENDASH_FONT_SIZE_SMALL);   // 14px
opendash_set_font(label, OPENDASH_FONT_SIZE_MEDIUM);  // 18px
opendash_set_font(label, OPENDASH_FONT_SIZE_LARGE);   // 32px

lv_label_set_text(label, "Hello World");
```

**When you change the default font in `font_config.json` and rebuild, all UI elements automatically use the new font!**

### Using a Specific Font (Advanced)

If you need to use a non-default font directly:

```c
// Declare the font (use the name from font_config.json + size)
LV_FONT_DECLARE(opensans_24);

// Use the font on a label
lv_obj_t *label = lv_label_create(parent);
lv_obj_set_style_text_font(label, &opensans_24, 0);
lv_label_set_text(label, "Hello World");
```

## Font Size Guidelines

For OpenDash displays:

### Center Display (800×480)
- Small: 14-16px
- Medium: 18-24px
- Large: 32-48px

### Left/Right Gauges (480×480)
- Small: 14-16px
- Medium: 24-32px
- Large: 48-64px

### GPS Display (466×466)
- Small: 14-16px
- Medium: 20-24px
- Large: 32-40px

## Troubleshooting

### "lv_font_conv: command not found"
The build system will automatically install `lv_font_conv` locally if Node.js is available.

If you need to install manually:
```bash
# Install Node.js (if not already installed)
sudo apt-get install nodejs npm

# Install lv_font_conv locally (no sudo required)
cd common/fonts
npm install lv_font_conv
```

Or install globally (requires sudo):
```bash
sudo npm install -g lv_font_conv
```

### "undefined reference to font"
Make sure to declare the font before using it:
```c
LV_FONT_DECLARE(your_font_name);
```

### Font file not found
Ensure the font file exists in `common/fonts/ttf/` and the path in `font_config.json` is correct.

### Build fails with font errors
Check the `generated/` directory for any error messages in the generated files.
