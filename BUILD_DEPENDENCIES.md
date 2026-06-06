<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OpenDash Build Dependencies Guide

This document explains all the dependencies required to build OpenDash and how to install them on your local machine.

## Overview

OpenDash uses ESP-IDF v5.3 as the main build system. Additionally, the build process automatically converts:
- **TrueType fonts** → LVGL C format (requires Node.js + npm)
- **Images (PNG/JPG)** → LVGL C format (requires Python + Pillow + ImageMagick)

These conversions happen automatically during the CMake configuration phase. If dependencies are missing, the build will show warnings but continue (fonts/images won't be available).

---

## Required Dependencies

### 1. ESP-IDF v5.3 (Required)

**What it is:** The Espressif IoT Development Framework for ESP32-S3.

**Installation:**

Follow the official guide: https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32s3/get-started/index.html

**Quick install (Linux/macOS):**
```bash
# Install prerequisites
sudo apt-get install git wget flex bison gperf python3 python3-pip python3-venv cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0

# Clone ESP-IDF
mkdir -p ~/esp
cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git -b release/v5.3
cd esp-idf

# Install tools
./install.sh esp32s3

# Source environment (add this to your ~/.bashrc or ~/.zshrc)
. $HOME/esp/esp-idf/export.sh
```

**Windows:** Use the ESP-IDF Windows Installer from the official guide.

---

### 2. Node.js + npm (Required for Fonts)

**What it is:** JavaScript runtime needed to run `lv_font_conv` (LVGL font converter).

**Why needed:** OpenDash uses custom TrueType fonts that must be converted to LVGL C format during build.

**Installation:**

**Ubuntu/Debian:**
```bash
sudo apt-get update
sudo apt-get install nodejs npm
```

**macOS:**
```bash
brew install node
```

**Windows:**
Download from: https://nodejs.org/

**Verify installation:**
```bash
node --version   # Should show v18+ or newer (LTS)
npm --version    # Should show 9+ or newer
```

**Install lv_font_conv (automatically handled):**
The build system automatically runs `npm install` in `common/fonts/` to install `lv_font_conv` locally.

---

### 3. Python 3 + Pillow (Required for Images)

**What it is:** Python with PIL (Pillow) library for image processing.

**Why needed:** OpenDash converts background images and splash screens to LVGL C arrays.

**Installation:**

**Ubuntu/Debian:**
```bash
sudo apt-get install python3 python3-pip
pip3 install Pillow
```

**macOS:**
```bash
brew install python3
pip3 install Pillow
```

**Windows:**
```bash
# Python 3 should already be installed with ESP-IDF
pip install Pillow
```

**Verify installation:**
```bash
python3 --version   # Should show 3.8+ or newer
python3 -c "import PIL; print(PIL.__version__)"  # Should show Pillow version
```

---

### 4. ImageMagick (Required for Images)

**What it is:** Command-line image processing tool.

**Why needed:** Used by the image converter to resize/scale images for different displays.

**Installation:**

**Ubuntu/Debian:**
```bash
sudo apt-get install imagemagick
```

**macOS:**
```bash
brew install imagemagick
```

**Windows:**
Download from: https://imagemagick.org/script/download.php#windows

**Verify installation:**
```bash
magick --version   # or 'convert --version' on older versions
```

---

## Manual Pre-Build Steps (Optional)

If you prefer to generate fonts/images manually before building (not required):

### Generate Fonts
```bash
cd common/fonts/
npm install
python3 convert_fonts.py
```

This creates:
- `generated/engebold_14.c`
- `generated/engebold_18.c`
- `generated/engebold_32.c`
- `generated/montserrat_14.c`
- `generated/montserrat_18.c`
- `generated/montserrat_32.c`

### Generate Images
```bash
cd common/images/
python3 convert_images.py
```

This converts PNG/JPG images from `source/` to C arrays in `generated/`.

---

## Build Process Summary

When you run `idf.py build` (or use VS Code ESP-IDF extension), the following happens:

1. **CMake Configuration Phase:**
   - Checks for Python 3
   - Runs `common/fonts/convert_fonts.py` (converts TrueType → C)
   - Runs `common/images/convert_images.py` (converts PNG/JPG → C)
   - Shows warnings if dependencies are missing

2. **Compilation Phase:**
   - Compiles all C files (including generated fonts/images)
   - Links with LVGL and ESP-IDF libraries
   - Produces binary firmware file

---

## Troubleshooting

### "lv_font_conv: command not found"

**Solution:** Install Node.js and npm, then run:
```bash
cd common/fonts/
npm install
```

### "ImageMagick is not installed"

**Solution:** Install ImageMagick:
```bash
# Ubuntu/Debian
sudo apt-get install imagemagick

# macOS
brew install imagemagick
```

### "No module named 'PIL'"

**Solution:** Install Pillow:
```bash
pip3 install Pillow
```

### "Font files not found during compilation"

**Cause:** Font generation failed or was skipped.

**Solution:**
1. Check that Node.js and npm are installed
2. Manually run font converter:
   ```bash
   cd common/fonts/
   npm install
   python3 convert_fonts.py
   ```
3. Check for error messages
4. Re-run `idf.py build`

### "Image files not found during compilation"

**Cause:** Image generation failed or was skipped.

**Solution:**
1. Check that Python 3, Pillow, and ImageMagick are installed
2. Manually run image converter:
   ```bash
   cd common/images/
   python3 convert_images.py
   ```
3. Check for error messages
4. Re-run `idf.py build`

---

## Quick Dependency Check

Run the automated dependency checker:

```bash
./check_deps.sh
```

This script verifies all dependencies and shows you what's missing. See the script source for implementation details.

---

## Platform-Specific All-In-One Installation

### Ubuntu/Debian
```bash
# Install all dependencies
sudo apt-get update
sudo apt-get install -y git wget flex bison gperf \
    python3 python3-pip python3-venv python3-setuptools \
    cmake ninja-build ccache libffi-dev libssl-dev \
    dfu-util libusb-1.0-0 nodejs npm imagemagick

# Install Python packages
pip3 install Pillow

# Install ESP-IDF (follow official guide for latest steps)
# Then source the environment: . $HOME/esp/esp-idf/export.sh
```

### macOS
```bash
# Install Homebrew if not already installed
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# Install all dependencies
brew install cmake ninja dfu-util ccache node imagemagick python3

# Install Python packages
pip3 install Pillow

# Install ESP-IDF (follow official guide for latest steps)
```

### Windows
1. Install ESP-IDF using Windows installer
2. Install Node.js from https://nodejs.org/
3. Install ImageMagick from https://imagemagick.org/
4. Run in ESP-IDF PowerShell:
   ```powershell
   pip install Pillow
   ```

---

## Summary

**Minimum required to build:**
- ESP-IDF v5.3

**For full functionality (fonts + images):**
- ESP-IDF v5.3
- Node.js + npm
- Python 3 + Pillow
- ImageMagick

All font and image conversions happen automatically during the CMake configuration phase. If you have all dependencies installed, simply run `idf.py build` and everything will work!
