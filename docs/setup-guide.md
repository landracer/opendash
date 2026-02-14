# OpenDash — Development Environment Setup Guide

## Prerequisites

### 1. Install ESP-IDF v5.3

OpenDash targets **ESP-IDF v5.3** for the ESP32-S3. Follow the official guide:

- **Linux/macOS:** https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32s3/get-started/linux-macos-setup.html
- **Windows:** https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32s3/get-started/windows-setup.html

Quick install (Linux/macOS):

```bash
mkdir -p ~/esp
cd ~/esp
git clone -b v5.3 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32s3
source export.sh
```

### 2. Install VS Code + ESP-IDF Extension (Recommended)

1. Install [Visual Studio Code](https://code.visualstudio.com/)
2. Install the [ESP-IDF Extension](https://marketplace.visualstudio.com/items?itemName=espressif.esp-idf-extension)
3. Configure the extension to point to your ESP-IDF installation

### 3. Clone the Repository

```bash
git clone https://github.com/landracer/opendash.git
cd opendash
```

---

## Building a Display Project

Each display project is a standalone ESP-IDF application. Navigate to the
desired project directory and build:

### Center Display (4.3" LCD)

```bash
cd center/
source ~/esp/esp-idf/export.sh    # If not already sourced
idf.py set-target esp32s3
idf.py build
```

### Left/Right Gauges (2.8" Round LCD)

```bash
cd left-right/
source ~/esp/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
```

### GPS / Telemetry (1.75" AMOLED)

```bash
cd gps/
source ~/esp/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
```

---

## Flashing

Connect the target device via USB-C and flash:

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

> **Note:** On macOS, the port is typically `/dev/cu.usbmodem*`.
> On Windows, check Device Manager for the COM port number.

Press `Ctrl+]` to exit the serial monitor.

---

## Adding Custom Background Images

OpenDash uses C-array images for backgrounds and assets. To convert an image:

### Using LVGL Online Converter

1. Go to https://lvgl.io/tools/imageconverter
2. Upload your image (PNG, JPG, BMP)
3. Set output format to **C array**
4. Set color format to match your display:
   - Center (4.3"): **RGB565**
   - Left/Right (2.8"): **RGB565**
   - GPS (1.75" AMOLED): **RGB565** or **RGB888**
5. Download the generated `.c` file
6. Place it in the `main/assets/` directory of the target project
7. Add the file to `main/CMakeLists.txt` SRCS list
8. Reference the image in your UI code:

```c
/* Declare the image (defined in the generated .c file) */
LV_IMG_DECLARE(my_background);

/* Use it as a background */
lv_obj_t *bg = lv_img_create(lv_scr_act());
lv_img_set_src(bg, &my_background);
```

### Using LVGL Offline Tool (lvgl_image_converter)

```bash
pip install pillow
python3 lvgl_image_converter.py my_image.png -f true_color -o my_image.c
```

---

## Project Dependencies

Each project uses the ESP-IDF component manager to pull LVGL and other
dependencies. Dependencies are declared in `main/idf_component.yml`:

```yaml
dependencies:
  lvgl/lvgl: "~9.2"
  esp_lvgl_port: "~2.4"
```

On first build, `idf.py build` automatically downloads managed components
into the `managed_components/` directory (git-ignored).

---

## OTA (Over-The-Air) Updates

Each unit supports OTA firmware updates over WiFi:

1. Build the firmware: `idf.py build`
2. Enable WiFi mode on the target device (via touch menu or BLE command)
3. The device starts a local HTTP server
4. Upload the firmware binary via the companion app or `curl`:

```bash
curl -X POST http://<device-ip>/ota \
     -F "firmware=@build/opendash_center.bin"
```

---

## Debugging

### Serial Monitor

```bash
idf.py -p /dev/ttyUSB0 monitor
```

### JTAG Debugging

The ESP32-S3 supports USB-JTAG debugging. Connect via the USB-C port and
use OpenOCD:

```bash
idf.py openocd
# In another terminal:
idf.py gdb
```

### Heap & Stack Analysis

```bash
idf.py size           # Show firmware size breakdown
idf.py size-components  # Size per component
```

---

## Useful Commands Reference

| Command | Description |
|---|---|
| `idf.py set-target esp32s3` | Set build target |
| `idf.py build` | Build the project |
| `idf.py flash` | Flash to device |
| `idf.py monitor` | Open serial monitor |
| `idf.py flash monitor` | Flash and open monitor |
| `idf.py menuconfig` | Open configuration menu |
| `idf.py fullclean` | Clean all build artifacts |
| `idf.py size` | Show binary size info |
