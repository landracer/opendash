# Compile Errors Resolution Guide

## Issue Summary

After the initial repository setup, users encountered compilation errors when trying to build OpenDash projects. This document explains the root causes and how they have been resolved.

## Root Causes

### 1. Missing Font Files

**Problem:**
- Font C files (`engebold_14.c`, `engebold_18.c`, etc.) were declared in `opendash_font_config.h` but not generated
- The CMakeLists.txt expects these files to exist in `common/fonts/generated/`
- Without these files, compilation fails with "file not found" errors

**Root Cause:**
- Font C files are auto-generated during build from TrueType fonts
- Generation requires Node.js + npm + lv_font_conv package
- Files are intentionally NOT committed to git (see `.gitignore`)
- Must be generated on first build or manually before building

**Resolution:**
1. Install Node.js and npm
2. Install lv_font_conv: `cd common/fonts && npm install`
3. Run font converter: `python3 convert_fonts.py`
4. Or let CMake do it automatically during build

### 2. Missing Image Files

**Problem:**
- Image C files for backgrounds/splashes are referenced but not generated
- Similar to fonts, these are auto-generated from PNG/JPG source files

**Root Cause:**
- Image conversion requires Python 3 + Pillow + ImageMagick
- Files are auto-generated during CMake configuration
- Not committed to git to keep repository size small

**Resolution:**
1. Install Python 3, Pillow, and ImageMagick
2. Run image converter: `cd common/images && python3 convert_images.py`
3. Or let CMake do it automatically during build

### 3. Missing Build Dependencies

**Problem:**
- Users may not have all required build dependencies installed
- CMake shows warnings but continues, leading to confusing errors later

**Root Cause:**
- OpenDash requires multiple external tools for build process
- Not all dependencies are obvious from ESP-IDF alone

**Resolution:**
- See [BUILD_DEPENDENCIES.md](BUILD_DEPENDENCIES.md) for complete installation guide
- Run `./check_deps.sh` to verify all dependencies

## Quick Resolution Steps

### For First-Time Build

1. **Check dependencies:**
   ```bash
   ./check_deps.sh
   ```

2. **Install missing dependencies** (see [BUILD_DEPENDENCIES.md](BUILD_DEPENDENCIES.md))

3. **Install font converter dependencies:**
   ```bash
   cd common/fonts
   npm install
   cd ../..
   ```

4. **Generate fonts and images (optional - CMake does this automatically):**
   ```bash
   cd common/fonts && python3 convert_fonts.py && cd ../..
   cd common/images && python3 convert_images.py && cd ../..
   ```

5. **Build project:**
   ```bash
   cd center  # or left-right, or gps
   idf.py set-target esp32s3
   idf.py build
   ```

## Understanding the Build Process

### CMake Configuration Phase

When you run `idf.py build`, CMake first configures the build:

1. **Find Python 3**
2. **Run font converter** (`common/fonts/convert_fonts.py`)
   - Checks for Node.js and lv_font_conv
   - Converts TrueType fonts → C arrays
   - Generates 6 font files (2 fonts × 3 sizes each)
3. **Run image converter** (`common/images/convert_images.py`)
   - Checks for ImageMagick and Pillow
   - Converts PNG/JPG → C arrays
   - Scales images for each display resolution
4. **Collect generated files** (GLOB patterns in CMakeLists.txt)
5. **Register component** with all source files

### Compilation Phase

After configuration, GCC compiles all C files including:
- Your main application code
- Generated font C files
- Generated image C files
- Common library code
- LVGL library
- ESP-IDF components

## Common Error Messages

### "fatal error: engebold_14.c: No such file or directory"

**Cause:** Font files not generated

**Solution:**
```bash
cd common/fonts
npm install
python3 convert_fonts.py
```

### "lv_font_conv: command not found"

**Cause:** lv_font_conv not installed

**Solution:**
```bash
cd common/fonts
npm install
```

### "ImageMagick is not installed"

**Cause:** ImageMagick not installed

**Solution:**
```bash
# Ubuntu/Debian
sudo apt-get install imagemagick

# macOS
brew install imagemagick
```

### "No module named 'PIL'"

**Cause:** Pillow not installed

**Solution:**
```bash
pip3 install Pillow
```

### "idf.py: command not found"

**Cause:** ESP-IDF not installed or environment not sourced

**Solution:**
```bash
# Install ESP-IDF first, then source environment
. $HOME/esp/esp-idf/export.sh
```

## Why Not Commit Generated Files?

### Fonts (*.c files)

**NOT committed because:**
- Large file size (300KB total)
- Binary format (not useful for code review)
- Can be regenerated from TrueType sources
- Reduces repository size and clone time

**Committed instead:**
- TrueType font files (`common/fonts/ttf/*.ttf`)
- Font configuration (`common/fonts/font_config.json`)
- Conversion script (`common/fonts/convert_fonts.py`)
- npm package spec (`common/fonts/package.json`)

### Images (*.c files)

**NOT committed because:**
- Very large files (can be MBs)
- Generated for each display resolution
- Can be regenerated from PNG/JPG sources

**Committed instead:**
- Source images (`common/images/source/*.png`)
- Scaled PNG files (for quick preview)
- Conversion script (`common/images/convert_images.py`)

## Automated Build Integration

### ESP-IDF Integration

The build system integrates seamlessly with ESP-IDF:

```cmake
# In common/CMakeLists.txt
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    execute_process(
        COMMAND ${Python3_EXECUTABLE} convert_fonts.py
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/fonts
    )
endif()
```

This ensures fonts/images are generated automatically during CMake configuration.

### VS Code Integration

When using the ESP-IDF VS Code extension:
1. Press F1 → "ESP-IDF: Build your project"
2. CMake runs automatically
3. Font/image converters run automatically
4. Build proceeds with generated files

### CI/CD Integration

For automated builds (GitHub Actions, etc.):
```yaml
- name: Install dependencies
  run: |
    sudo apt-get install nodejs npm imagemagick
    pip3 install Pillow
    
- name: Build OpenDash
  run: |
    cd center
    idf.py build
```

## Troubleshooting Tips

### 1. Clean Build

If you're getting strange errors, try a clean build:
```bash
cd center  # or whichever project
rm -rf build
idf.py build
```

### 2. Regenerate Fonts

If fonts seem corrupted:
```bash
cd common/fonts
rm -rf generated/*.c
python3 convert_fonts.py
```

### 3. Regenerate Images

If images aren't displaying correctly:
```bash
cd common/images
rm -rf generated/*.c generated/*.h
python3 convert_images.py
```

### 4. Check CMake Output

Look for warnings in the CMake configuration output:
```
[WARNING] Font conversion failed or skipped
[WARNING] Image conversion failed
```

These indicate missing dependencies.

### 5. Verify Generated Files

Check that files were actually generated:
```bash
ls -la common/fonts/generated/*.c
ls -la common/images/generated/*.c
```

Should show 6 font files and several image files.

## Summary

**The key insight:** OpenDash uses a build-time code generation system for fonts and images. This is a deliberate design choice to:
- Keep repository size small
- Support easy customization (just drop in new fonts/images)
- Maintain clean separation between sources and generated code
- Follow LVGL best practices

**The trade-off:** Users must install additional build dependencies (Node.js, ImageMagick, etc.) beyond just ESP-IDF.

**The solution:** Follow [BUILD_DEPENDENCIES.md](BUILD_DEPENDENCIES.md) and run `./check_deps.sh` to ensure everything is properly installed.

---

## Next Steps

1. ✅ Verify all dependencies: `./check_deps.sh`
2. ✅ Install missing dependencies: See [BUILD_DEPENDENCIES.md](BUILD_DEPENDENCIES.md)
3. ✅ Build your first project: `cd center && idf.py build`
4. ✅ Flash to hardware: `idf.py -p <PORT> flash monitor`

If you still encounter issues, please check the [troubleshooting section](#troubleshooting-tips) above or open an issue on GitHub.
