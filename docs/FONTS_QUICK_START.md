# Quick Start: Using Custom Fonts in OpenDash

## TL;DR

1. Add your `.ttf` file to `common/fonts/ttf/`
2. Edit `common/fonts/font_config.json`
3. Run `idf.py build` - fonts convert automatically
4. Use in code: `LV_FONT_DECLARE(fontname_size);`

## Example: Adding Roboto Font

### 1. Get the Font
Download Roboto-Regular.ttf and place it in `common/fonts/ttf/`

### 2. Configure
Edit `common/fonts/font_config.json`:

```json
{
  "fonts": [
    {
      "name": "roboto",
      "source": "Roboto-Regular.ttf",
      "sizes": [16, 24, 32],
      "bpp": 4,
      "range": "0x20-0x7F"
    }
  ]
}
```

### 3. Build
```bash
cd center  # or any display project
idf.py build
```

### 4. Use in Code
In your `ui_manager.c`:

```c
// Declare the fonts you want to use
LV_FONT_DECLARE(roboto_16);
LV_FONT_DECLARE(roboto_24);
LV_FONT_DECLARE(roboto_32);

// Use them on labels
lv_obj_t *label = lv_label_create(parent);
lv_obj_set_style_text_font(label, &roboto_24, 0);
lv_label_set_text(label, "Hello Roboto!");
```

## Configuration Options

### Font Name
```json
"name": "myfont"
```
Creates: `myfont_16`, `myfont_24`, etc.

### Sizes
```json
"sizes": [14, 18, 24, 32, 48]
```
Generate multiple sizes at once.

### Bits Per Pixel (Antialiasing)
```json
"bpp": 4  // Options: 1, 2, 4, 8
```
- `1`: Monochrome (smallest, no smoothing)
- `2`: 4-level grayscale (small, slight smoothing)
- `4`: 16-level grayscale (recommended - good quality)
- `8`: 256-level grayscale (largest, best quality)

### Character Range
```json
"range": "0x20-0x7F"  // Basic ASCII
```

Common ranges:
- `0x20-0x7F` - ASCII printable characters
- `0x20-0xFF` - ASCII + Latin-1 (accents, symbols)
- `0x20-0x7E,0xB0,0xB1,0xB7` - ASCII + degree (°), plus-minus (±), middle dot (·)

## Using the Font Helper

Instead of direct font references, you can use the helper:

```c
#include "opendash_fonts.h"

// Use abstract sizes
opendash_set_font(label, OPENDASH_FONT_SIZE_SMALL);   // 14px
opendash_set_font(label, OPENDASH_FONT_SIZE_MEDIUM);  // 18px
opendash_set_font(label, OPENDASH_FONT_SIZE_LARGE);   // 32px
```

To customize these, edit `common/include/opendash_fonts.h`:

```c
static inline const lv_font_t* opendash_get_font(opendash_font_size_t size)
{
    switch (size) {
        case OPENDASH_FONT_SIZE_SMALL:
            return &roboto_16;  // Change from montserrat to roboto
        case OPENDASH_FONT_SIZE_MEDIUM:
            return &roboto_24;
        case OPENDASH_FONT_SIZE_LARGE:
            return &roboto_32;
        default:
            return &lv_font_montserrat_14;
    }
}
```

## Manual Font Conversion

If you prefer manual control:

```bash
cd common/fonts

# Convert a font manually
lv_font_conv \
  --font ttf/MyFont.ttf \
  --size 24 \
  --bpp 4 \
  --format lvgl \
  --range 0x20-0x7F \
  --output generated/myfont_24.c
```

## Troubleshooting

### "lv_font_conv: command not found"
```bash
npm install -g lv_font_conv
```

### Font looks pixelated
- Increase `bpp` to 4 or 8
- Check you're using the right size

### Build fails
```bash
cd center
idf.py fullclean
python3 ../common/fonts/convert_fonts.py --force
idf.py build
```

### Font too large
- Reduce `bpp` (try 2 instead of 4)
- Limit character range
- Use smaller sizes
- Only include sizes you need

## Font Sources

Free fonts for commercial use:
- **Google Fonts**: https://fonts.google.com/
- **Font Squirrel**: https://www.fontsquirrel.com/
- **Open Font Library**: https://fontlibrary.org/

Popular choices for dashboards:
- **Roboto** - Clean, modern, highly legible
- **Open Sans** - Friendly, professional
- **Lato** - Lightweight, elegant
- **Montserrat** - Geometric, strong
- **Source Sans Pro** - Excellent readability

## Best Practices

### ✅ DO
- Use 4bpp for smooth text
- Test on actual hardware (fonts look different than on PC)
- Limit character range to what you need
- Use consistent font family across display
- Generate multiple sizes from same font

### ❌ DON'T
- Don't include every Unicode character unless needed
- Don't use 8bpp unless quality is critical (wastes space)
- Don't mix too many font families (2-3 max)
- Don't use decorative fonts for data values
- Don't forget to declare fonts before using

## Size Guidelines

### Center Display (800×480)
- **Labels**: 14-16px
- **Values**: 18-24px
- **Primary data**: 32-40px
- **Status text**: 12-14px

### Round Displays (480×480 and 466×466)
- **Labels**: 14-16px
- **Primary value**: 48-64px
- **Secondary value**: 20-24px
- **Small text**: 12-14px

## Performance Tips

1. **Preload fonts**: Convert during build, not runtime
2. **Cache fonts**: Reuse font objects across widgets
3. **Limit sizes**: Only generate what you use
4. **Use built-ins**: LVGL's Montserrat is already optimized

## Need Help?

See detailed documentation:
- `common/fonts/README.md` - Complete font system guide
- `docs/font-system-testing.md` - Testing procedures
- `docs/FONT_SYSTEM_IMPLEMENTATION.md` - Technical details
