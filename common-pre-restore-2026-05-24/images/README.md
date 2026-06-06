<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OpenDash Image Management System

This directory contains the image management system for OpenDash displays.

## Quick Start: Adding Images

**To add a background or splash screen:**

1. Place your image in `source/` with the naming convention:
   - `background_<display>.jpg` or `background_<display>.png`
   - `splash_<display>.jpg` or `splash_<display>.png`
2. Rebuild the project

That's it! Images are automatically scaled and converted.

## Directory Structure

- `source/` - Place your original image files here (JPG/PNG)
- `generated/` - Auto-generated C files (do not edit manually)
- `convert_images.py` - Conversion script (runs automatically during build)

## Supported Displays

| Display | Resolution | Shape | Image Names |
|---------|-----------|-------|-------------|
| **center** | 800×480 | Rectangular | `background_center.jpg`, `splash_center.png` |
| **leftright** | 480×480 | Round | `background_leftright.jpg`, `splash_leftright.png` |
| **gps** | 466×466 | Round | `background_gps.jpg`, `splash_gps.png` |

## Image Types

### Background Images
Optional background images displayed behind UI elements.

**Usage:**
```c
#include "background_center.h"

// Use in LVGL
lv_obj_t *bg = lv_img_create(lv_scr_act());
lv_img_set_src(bg, (const void*)background_center);
```

### Splash Screens
Boot splash screens displayed during initialization.

**Usage:**
```c
#include "splash_center.h"

// Display splash during boot
lv_obj_t *splash = lv_img_create(lv_scr_act());
lv_img_set_src(splash, (const void*)splash_center);
```

## Auto-Scaling Feature

The build system automatically scales images to fit display resolution:

### How It Works
1. **Proportional Scaling**: Images are scaled to fit within display dimensions while maintaining aspect ratio
2. **No Stretching**: Aspect ratio is always preserved - no distortion
3. **Center Alignment**: If image doesn't fill entire screen, it's centered with black bars
4. **RGB565 Conversion**: Images are converted to 16-bit color format for efficient storage

### Example Scenarios

**Scenario 1: Exact Size Image**
```
Input:  background_center.jpg (800×480)
Output: Direct conversion, no scaling needed
```

**Scenario 2: Larger Image**
```
Input:  background_center.jpg (1920×1080)
Output: Scaled to 800×450, centered vertically with 15px black bars top/bottom
```

**Scenario 3: Different Aspect Ratio**
```
Input:  splash_gps.jpg (1000×500, 2:1 ratio)
Output: Scaled to 466×233, centered vertically with black bars
```

### Manual Sizing (Optional)

For pixel-perfect control without scaling:
- Provide images at exact display resolution
- Conversion is direct without any scaling
- Recommended for production to minimize conversion artifacts

**Exact resolutions:**
```
center:    800×480 pixels
leftright: 480×480 pixels
gps:       466×466 pixels
```

## Image Format Recommendations

### File Format
- **JPG**: Best for photographs, smaller file size
- **PNG**: Best for graphics with transparency, larger but lossless

### Color Depth
- Source images can be any color depth
- Converted to RGB565 (16-bit, 65K colors)
- Sufficient for display quality

### File Size Considerations

Generated C files can be large:
```
800×480 image = 384,000 pixels × 2 bytes = 768 KB
480×480 image = 230,400 pixels × 2 bytes = 460 KB
466×466 image = 217,156 pixels × 2 bytes = 434 KB
```

**Tips to reduce size:**
1. Use exact resolution (avoids scaling artifacts)
2. Use JPG for photos (converts to same RGB565 anyway)
3. Consider simpler images for backgrounds
4. Use splash screens only if needed

## Automatic Conversion

The build system automatically:
1. Checks if ImageMagick is installed
2. Scans `source/` for images
3. Scales images proportionally to fit display
4. Converts to RGB565 format
5. Generates `.h` and `.c` files in `generated/`
6. Includes them in the build

**Note:** Conversion runs during CMake configuration, before compilation.

## Manual Conversion

If you need to manually convert images:

```bash
cd common/images
python3 convert_images.py              # Convert all images
python3 convert_images.py --force      # Force re-conversion
python3 convert_images.py --check      # Check dependencies
```

## Using Images in Code

### Include the Header

```c
#include "background_center.h"
#include "splash_center.h"
```

### Display Background

```c
void ui_manager_set_background(void)
{
    // Create image object
    lv_obj_t *bg = lv_img_create(lv_scr_act());
    
    // Set image data
    lv_img_set_src(bg, (const void*)background_center);
    
    // Position at origin
    lv_obj_align(bg, LV_ALIGN_TOP_LEFT, 0, 0);
    
    // Send to back
    lv_obj_move_background(bg);
}
```

### Display Splash Screen

```c
void show_splash_screen(void)
{
    // Create fullscreen splash
    lv_obj_t *splash = lv_img_create(lv_scr_act());
    lv_img_set_src(splash, (const void*)splash_center);
    lv_obj_center(splash);
    
    // Update display
    lv_task_handler();
    
    // Wait 2 seconds
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // Remove splash
    lv_obj_del(splash);
}
```

## Troubleshooting

### "ImageMagick is not installed"
```bash
sudo apt-get update
sudo apt-get install imagemagick
```

### "No images found to process"
Check your image file names:
- Must be in `source/` directory
- Must follow naming: `background_<display>.ext` or `splash_<display>.ext`
- Extensions: `.jpg`, `.jpeg`, or `.png`

### "Failed to scale image"
- Check image file is valid and not corrupted
- Ensure sufficient disk space
- Try converting image manually with ImageMagick first

### Large generated files
- Use JPG instead of PNG for photos
- Provide images at exact resolution to skip scaling
- Consider simpler backgrounds
- Only use splash screens where needed

### Build errors
```bash
# Clean and rebuild
cd center
idf.py fullclean
idf.py build
```

## Examples

### Center Display (800×480)
```bash
# Add a background
cp my-dashboard.jpg common/images/source/background_center.jpg

# Add a splash screen
cp logo.png common/images/source/splash_center.png

# Rebuild
cd center && idf.py build
```

### GPS Display (466×466 Round)
```bash
# Add a circular background
cp gps-background.png common/images/source/background_gps.png

# Rebuild
cd gps && idf.py build
```

## Best Practices

1. **Test on actual hardware** - Emulators may not show true colors
2. **Use dark backgrounds** - Saves power on OLED/AMOLED displays
3. **Keep it simple** - Complex images increase flash usage
4. **Provide exact sizes** - Avoids scaling artifacts
5. **Use splash sparingly** - Boot time impact

## Size Guidelines

Recommended maximum file sizes for source images:
- **Backgrounds**: < 500 KB (uncompressed in flash as RGB565)
- **Splash screens**: < 500 KB (temporary, removed after boot)

Remember: Source JPG/PNG size doesn't matter - final RGB565 size is based on resolution only.

---

**See also:**
- `DISPLAY_SYNCHRONIZATION.md` - How displays share resources
- `fonts/README.md` - Font management system
- LVGL documentation on images: https://docs.lvgl.io/master/widgets/img.html
