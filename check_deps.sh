#!/bin/bash
# OpenDash Build Dependencies Checker
# This script verifies that all required dependencies for building OpenDash are installed.

echo "Checking OpenDash Build Dependencies..."
echo "========================================"
echo ""

MISSING_DEPS=0
OPTIONAL_MISSING=0

# Function to check command
check_command() {
    local cmd=$1
    local name=$2
    local required=$3
    
    if command -v "$cmd" &> /dev/null; then
        echo "✅ $name: Found"
        return 0
    else
        echo "❌ $name: NOT FOUND"
        if [ "$required" = "required" ]; then
            MISSING_DEPS=$((MISSING_DEPS + 1))
        else
            OPTIONAL_MISSING=$((OPTIONAL_MISSING + 1))
        fi
        return 1
    fi
}

# ESP-IDF (Required)
echo "Checking Required Dependencies:"
echo "--------------------------------------"
if command -v idf.py &> /dev/null; then
    IDF_VERSION=$(idf.py --version 2>&1 | head -1 | cut -d' ' -f2 2>/dev/null || echo "unknown")
    echo "✅ ESP-IDF: Found (version $IDF_VERSION)"
    if [[ ! "$IDF_VERSION" == v5.3* ]] && [[ "$IDF_VERSION" != "unknown" ]]; then
        echo "   ⚠️  Warning: OpenDash requires ESP-IDF v5.3.x, found $IDF_VERSION"
    fi
else
    echo "❌ ESP-IDF: NOT FOUND"
    echo "   Install from: https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32s3/get-started/index.html"
    MISSING_DEPS=$((MISSING_DEPS + 1))
fi

echo ""
echo "Checking Font Generation Dependencies:"
echo "--------------------------------------"

# Node.js (Required for fonts)
if check_command "node" "Node.js" "required"; then
    NODE_VERSION=$(node --version)
    echo "   Version: $NODE_VERSION"
fi

# npm (Required for fonts)
if check_command "npm" "npm" "required"; then
    NPM_VERSION=$(npm --version)
    echo "   Version: $NPM_VERSION"
fi

# Check if lv_font_conv is installed locally
if [ -f "common/fonts/node_modules/.bin/lv_font_conv" ]; then
    echo "✅ lv_font_conv: Found (locally installed)"
elif command -v lv_font_conv &> /dev/null; then
    echo "✅ lv_font_conv: Found (globally installed)"
else
    echo "⚠️  lv_font_conv: NOT FOUND"
    echo "   Will be installed automatically during build"
fi

echo ""
echo "Checking Image Conversion Dependencies:"
echo "--------------------------------------"

# Python 3 (Required for images)
if check_command "python3" "Python 3" "required"; then
    PY_VERSION=$(python3 --version | cut -d' ' -f2)
    echo "   Version: $PY_VERSION"
fi

# Pillow (Required for images)
if python3 -c "import PIL" 2> /dev/null; then
    PIL_VERSION=$(python3 -c 'import PIL; print(PIL.__version__)' 2> /dev/null)
    echo "✅ Pillow: Found (version $PIL_VERSION)"
else
    echo "❌ Pillow: NOT FOUND"
    echo "   Install with: pip3 install Pillow"
    MISSING_DEPS=$((MISSING_DEPS + 1))
fi

# ImageMagick (Required for images)
if command -v magick &> /dev/null; then
    IM_VERSION=$(magick --version 2> /dev/null | head -1 | cut -d' ' -f3)
    echo "✅ ImageMagick: Found (version $IM_VERSION)"
elif command -v convert &> /dev/null; then
    IM_VERSION=$(convert --version 2> /dev/null | head -1 | cut -d' ' -f3)
    echo "✅ ImageMagick: Found (version $IM_VERSION)"
else
    echo "❌ ImageMagick: NOT FOUND"
    echo "   Install with: sudo apt-get install imagemagick (Linux)"
    echo "                 brew install imagemagick (macOS)"
    MISSING_DEPS=$((MISSING_DEPS + 1))
fi

# Summary
echo ""
echo "========================================"
echo "Summary:"
echo "--------------------------------------"

if [ $MISSING_DEPS -eq 0 ]; then
    echo "✅ All required dependencies are installed!"
    echo ""
    echo "You can now build OpenDash:"
    echo "  cd center/ && idf.py build"
    exit 0
else
    echo "❌ $MISSING_DEPS required dependencies are missing"
    echo ""
    echo "Please install missing dependencies and try again."
    echo "See BUILD_DEPENDENCIES.md for detailed installation instructions."
    exit 1
fi
