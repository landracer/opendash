# Visual Studio Code Setup for OpenDash

This guide provides detailed instructions for setting up Visual Studio Code to build and flash all three OpenDash display projects.

## Quick Start

1. **Install Prerequisites**
   - [Visual Studio Code](https://code.visualstudio.com/)
   - [ESP-IDF v5.3](https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32s3/get-started/index.html)

2. **Install ESP-IDF Extension**
   - Open VS Code
   - Extensions → Search "ESP-IDF" → Install "Espressif IDF"

3. **Open Workspace**
   - File → Open Workspace from File
   - Select `opendash.code-workspace`

4. **Build Any Display**
   - Open a project folder in the sidebar
   - Press **F1** → "ESP-IDF: Build your project"

---

## Detailed Setup Instructions

### Step 1: Install Visual Studio Code

Download and install VS Code from https://code.visualstudio.com/

### Step 2: Install ESP-IDF Extension

1. **Open VS Code**
2. **Go to Extensions** (Ctrl+Shift+X or Cmd+Shift+X)
3. **Search for "ESP-IDF"**
4. **Install "Espressif IDF"** by Espressif Systems
5. **Restart VS Code**

### Step 3: Configure ESP-IDF Extension

#### Option A: Express Installation (Recommended for New Users)

1. Press **F1** (or Ctrl+Shift+P)
2. Type: `ESP-IDF: Configure ESP-IDF Extension`
3. Select **Express**
4. Select ESP-IDF version: **v5.3**
5. Choose installation directory (e.g., `~/esp/esp-idf`)
6. Wait for installation to complete (~10-15 minutes)

#### Option B: Use Existing ESP-IDF Installation

If you already have ESP-IDF v5.3 installed:

1. Press **F1**
2. Type: `ESP-IDF: Configure ESP-IDF Extension`
3. Select **Use Existing Setup**
4. Point to ESP-IDF directory (e.g., `~/esp/esp-idf`)
5. Select Python executable (use the one in ESP-IDF's venv)
6. Click "Save"

### Step 4: Open the OpenDash Workspace

VS Code workspace allows you to work on all three display projects simultaneously.

1. **File → Open Workspace from File**
2. Navigate to the OpenDash repository
3. Select `opendash.code-workspace`
4. Click "Open"

You should now see all project folders in the sidebar:
- 📄 Documentation & Common
- 🖥️ Center Display (4.3" LCD)
- ⭕ Left/Right Gauges (2.8" Round)
- 🛰️ GPS / Telemetry (1.75" AMOLED)

---

## Building and Flashing

### Building a Display Project

Each display can be built independently:

1. **Navigate to Project**
   - In the VS Code sidebar, expand one of the display folders
   - Open any `.c` file in that project (this sets it as the active project)

2. **Set Target Device** (First time only)
   - Press **F1**
   - Type: `ESP-IDF: Set Espressif device target`
   - Select **ESP32-S3**

3. **Build**
   - Press **F1**
   - Type: `ESP-IDF: Build your project`
   - **OR** click the Build button (🔧) in the status bar
   - Wait for build to complete

**Build Outputs:**
- Center: `center/build/opendash_center.bin`
- Left/Right: `left-right/build/opendash_leftright.bin`
- GPS: `gps/build/opendash_gps.bin`

### Flashing to Device

1. **Connect Device**
   - Connect the ESP32-S3 board via USB-C

2. **Select Serial Port**
   - Press **F1**
   - Type: `ESP-IDF: Select port to use`
   - Select your device (e.g., `/dev/ttyUSB0`, `COM3`)

3. **Flash**
   - Press **F1**
   - Type: `ESP-IDF: Flash your project`
   - **OR** click the Flash button (⚡) in the status bar

4. **Monitor Output**
   - Press **F1**
   - Type: `ESP-IDF: Monitor device`
   - **OR** click the Monitor button (📺) in the status bar
   - Press **Ctrl+]** to exit

### All-in-One: Build, Flash, and Monitor

- Press **F1**
- Type: `ESP-IDF: Build, Flash and Monitor`
- This will do all three steps automatically

---

## Working with Multiple Displays

### Switching Between Projects

**Method 1: Change Active File**
- Simply open a `.c` file from the project you want to work on
- VS Code will automatically switch context

**Method 2: Use Terminal**
- Right-click a project folder → "Open in Integrated Terminal"
- Run command-line build:
  ```bash
  idf.py set-target esp32s3
  idf.py build
  ```

### Building All Displays at Once

Create a task file `.vscode/tasks.json` in the root:

```json
{
  "version": "2.0.0",
  "tasks": [
    {
      "label": "Build All Displays",
      "type": "shell",
      "command": "cd center && idf.py build && cd ../left-right && idf.py build && cd ../gps && idf.py build",
      "group": "build"
    }
  ]
}
```

Then: Terminal → Run Task → "Build All Displays"

---

## Status Bar Buttons

After installing ESP-IDF extension, you'll see these buttons in the status bar:

| Button | Action | Description |
|---|---|---|
| 🔧 | Build | Build the active project |
| ⚡ | Flash | Flash to device |
| 📺 | Monitor | Open serial monitor |
| 🗑️ | Clean | Clean build artifacts |
| ⚙️ | Menuconfig | Open SDK configuration |

---

## Keyboard Shortcuts

| Shortcut | Action |
|---|---|
| **Ctrl+E B** | Build project |
| **Ctrl+E F** | Flash device |
| **Ctrl+E M** | Monitor device |
| **Ctrl+E D** | Build, Flash, Monitor |

*(Mac: use Cmd instead of Ctrl)*

---

## Troubleshooting

### "ESP-IDF not found" Error

**Solution:**
1. Press **F1** → `ESP-IDF: Configure ESP-IDF Extension`
2. Re-select your ESP-IDF installation path
3. Restart VS Code

### Build Fails with "Target not set"

**Solution:**
1. Press **F1** → `ESP-IDF: Set Espressif device target`
2. Select **ESP32-S3**

### Serial Port Not Detected

**Linux:**
```bash
# Add your user to dialout group
sudo usermod -a -G dialout $USER
# Log out and log back in
```

**Windows:**
- Install USB drivers from https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers

**Mac:**
- Install drivers if needed
- Port usually appears as `/dev/cu.usbmodem*`

### Extension Not Working After Update

**Solution:**
1. Uninstall ESP-IDF extension
2. Restart VS Code
3. Reinstall ESP-IDF extension
4. Reconfigure with **F1** → `ESP-IDF: Configure ESP-IDF Extension`

### LVGL Dependency Not Found

**Solution:**
Component dependencies are auto-downloaded on first build. If missing:
```bash
cd <project-dir>
idf.py reconfigure
idf.py build
```

---

## Advanced Configuration

### Custom Build Configurations

Create `.vscode/settings.json` in each project:

```json
{
  "idf.port": "/dev/ttyUSB0",
  "idf.baudRate": "460800",
  "idf.flashType": "UART"
}
```

### IntelliSense Configuration

The ESP-IDF extension automatically configures IntelliSense. If you need to customize:

1. Press **F1** → `C/C++: Edit Configurations (JSON)`
2. Modify `c_cpp_properties.json`

### Debugging with JTAG

ESP32-S3 supports USB-JTAG debugging:

1. Connect device via USB
2. Press **F1** → `ESP-IDF: Launch JTAG debugger`
3. Set breakpoints and debug

---

## Alternative: PlatformIO

For users who prefer PlatformIO over ESP-IDF extension:

### Install PlatformIO

1. Open VS Code
2. Extensions → Search "PlatformIO"
3. Install "PlatformIO IDE"

### Create platformio.ini

Place in each project root (example for center):

```ini
[env:esp32-s3-devkitc-1]
platform = espressif32
board = esp32-s3-devkitc-1
framework = espidf
board_build.flash_size = 16MB
board_build.partitions = default_16MB.csv
monitor_speed = 115200

; Extra flags
build_flags = 
    -DBOARD_HAS_PSRAM
    -DCONFIG_SPIRAM_CACHE_WORKAROUND

; Link to common component
lib_extra_dirs = ../common
```

### Build with PlatformIO

1. Click PlatformIO icon (alien head) in sidebar
2. Select project environment
3. Click "Build" or "Upload"

---

## Summary

You now have two options for building OpenDash displays:

1. **ESP-IDF Extension (Recommended)**
   - Native ESP-IDF support
   - Better integration with ESP-IDF tools
   - Official Espressif support

2. **PlatformIO (Alternative)**
   - Unified build system
   - Good for multi-platform projects
   - More automation

Both work well — choose based on your preference!

---

**Questions?** See [`docs/setup-guide.md`](./setup-guide.md) for more information.
