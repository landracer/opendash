<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# Font Update Guide for OpenDash

This guide provides step-by-step instructions for updating fonts in the OpenDash project, including adding new font sizes and modifying existing font configurations.

## Prerequisites

- ESP-IDF development environment installed
- Node.js and npm installed
- Basic understanding of the project structure

## Updating Font Sizes

### 1. Modify Font Configuration

Edit the font configuration file:
```bash
nano /home/sysadmin/Documents/rAtTrax-Dash/opendash/common/fonts/font_config.json
```

In this file, you'll find configurations for each font. To add new font sizes, simply add the desired sizes to the `sizes` array for each font. For example:

```json
{
  "name": "kimbalt",
  "source": "kimbalt_.ttf",
  "comment": "Kimbalt font - bold style, system default",
  "sizes": [14, 18, 32, 64, 96, 128, 150, 160],
  "bpp": 4,
  "range": "0x20-0x7F,0xB0",
  "default": true
}
```

### 2. Run Font Conversion

After modifying the configuration, run the font conversion script to generate the new font files:
```bash
cd /home/sysadmin/Documents/rAtTrax-Dash/opendash/common/fonts
python3 convert_fonts.py
```

Or force re-conversion:
```bash
python3 convert_fonts.py --force
```

### 3. Verify Generated Files

The font conversion script will generate C source files for each font size in:
```
/home/sysadmin/Documents/rAtTrax-Dash/opendash/common/fonts/generated/
```

You should see files like:
- `kimbalt_14.c`
- `kimbalt_150.c` 
- `kimbalt_160.c`
- etc.

### 4. Update Header File (if needed)

The `convert_fonts.py` script automatically generates the header file `opendash_font_config.h`. However, if you encounter issues with font declarations, manually verify that all font sizes are included in:
```
/home/sysadmin/Documents/rAtTrax-Dash/opendash/common/fonts/generated/opendash_font_config.h
```

The header file should include declarations for all font sizes, for example:
```c
LV_FONT_DECLARE(kimbalt_14);
LV_FONT_DECLARE(kimbalt_18);
LV_FONT_DECLARE(kimbalt_32);
LV_FONT_DECLARE(kimbalt_64);
LV_FONT_DECLARE(kimbalt_96);
LV_FONT_DECLARE(kimbalt_128);
LV_FONT_DECLARE(kimbalt_150);
LV_FONT_DECLARE(kimbalt_160);
```

## Changing Primary Display Font Size

To change the primary display font size to 160px:

### 1. Update ui_manager.c files

In both pod1 and pod2, locate the line where the primary value font is set:
```c
// In /home/sysadmin/Documents/rAtTrax-Dash/opendash/pod1/main/ui_manager.c
lv_obj_set_style_text_font(primary_value, &kimbalt_150, 0);

// In /home/sysadmin/Documents/rAtTrax-Dash/opendash/pod2/main/ui_manager.c  
lv_obj_set_style_text_font(primary_value, &kimbalt_150, 0);
```

Change `&kimbalt_150` to `&kimbalt_160`:
```c
lv_obj_set_style_text_font(primary_value, &kimbalt_160, 0);
```

### 2. Rebuild the Project

After making the changes:
```bash
cd /home/sysadmin/Documents/rAtTrax-Dash/opendash/pod1
idf.py build

cd /home/sysadmin/Documents/rAtTrax-Dash/opendash/pod2
idf.py build
```

## Troubleshooting

### Common Issues

1. **"Font undeclared" errors**: Ensure that the `opendash_font_config.h` file includes declarations for all font sizes you're using.

2. **Build failures with large fonts**: Make sure `CONFIG_LV_FONT_FMT_TXT_LARGE=y` is set in your `sdkconfig.defaults` file.

3. **Font files not generated**: Verify that the font source files exist in the `ttf/` directory and that Node.js and lv_font_conv are properly installed.

### Required Configuration Settings

Ensure these settings are in your `sdkconfig.defaults` files:
```
CONFIG_LV_FONT_FMT_TXT_LARGE=y
```

## Best Practices

1. Always backup configuration files before making changes
2. Test font changes on actual hardware before deployment
3. Keep font sizes consistent across all pods for visual uniformity
4. Document any custom font modifications for future reference
5. Verify that font sizes are appropriate for display readability

## File Structure Reference

```
/home/sysadmin/Documents/rAtTrax-Dash/opendash/
├── common/
│   ├── fonts/
│   │   ├── font_config.json          # Font configuration
│   │   ├── convert_fonts.py          # Font conversion script
│   │   ├── ttf/                      # Font source files
│   │   └── generated/                # Generated font files
├── pod1/
│   └── main/
│       └── ui_manager.c              # UI font declarations
├── pod2/
│   └── main/
│       └── ui_manager.c              # UI font declarations
└── ...
## Special Case: Fixing Font Header Generation

During the channel-based architecture deployment, we discovered that the `convert_fonts.py` script only generated font declarations up to 128pt, even though `font_config.json` specified 150 and 160pt fonts. This caused build failures in pod1 and pod2.

**Fix Applied:**
We modified `/home/sysadmin/Documents/rAtTrax-Dash/opendash/common/fonts/convert_fonts.py` to properly include 150 and 160pt font declarations in the generated header file.

The fix added support for extra-large font sizes (150, 160) after the extended sizes (64, 96, 128) in the font header generation logic.

### 5. Update SDK Configuration for Large Fonts

After font updates, ensure that all nodes have the large font configuration enabled in their `sdkconfig` files:

```bash
# For all nodes, enable large font support
for node in center pod1 pod2 left right gps relay-4ch-hd relay-8ch-a relay-8ch-b mos-4ch-a mos-4ch-b; do
  cfg="/home/sysadmin/Documents/rAtTrax-Dash/opendash/mos-4ch-b/sdkconfig"
  if [ -f "/home/sysadmin/Documents/rAtTrax-Dash/opendash/mos-4ch-b/sdkconfig" ]; then
    grep -q "CONFIG_LV_FONT_FMT_TXT_LARGE=y" "/home/sysadmin/Documents/rAtTrax-Dash/opendash/mos-4ch-b/sdkconfig" || \n    sed -i 's/# CONFIG_LV_FONT_FMT_TXT_LARGE is not set/CONFIG_LV_FONT_FMT_TXT_LARGE=y/' "/home/sysadmin/Documents/rAtTrax-Dash/opendash/mos-4ch-b/sdkconfig"
  fi
done
```
