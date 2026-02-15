# OpenDash — Quick Start Guide

Get up and running with OpenDash in 5 minutes!

## What You Need

### Hardware (Choose One to Start)
- **Center Display:** Waveshare ESP32-S3-Touch-LCD-4.3 (800×480)
- **Left/Right Gauge:** Waveshare ESP32-S3-LCD-2.8C (480×480 round)
- **GPS/Telemetry:** Waveshare ESP32-S3-Touch-AMOLED-1.75 (466×466 round)
- **USB-C cable** for programming

### Software
- **Visual Studio Code** — [Download](https://code.visualstudio.com/)
- **ESP-IDF v5.3** — [Installation Guide](https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32s3/get-started/index.html)
- **Node.js + npm** — For font conversion
- **Python 3 + Pillow + ImageMagick** — For image conversion

> **📦 Full dependency details:** See [BUILD_DEPENDENCIES.md](BUILD_DEPENDENCIES.md) for complete installation instructions

---

## 5-Minute Setup

### 1. Install VS Code + ESP-IDF Extension

```bash
# Install VS Code, then:
# 1. Open VS Code
# 2. Go to Extensions (Ctrl+Shift+X)
# 3. Search for "ESP-IDF"
# 4. Install "Espressif IDF" extension
```

### 2. Install Build Dependencies

See [BUILD_DEPENDENCIES.md](BUILD_DEPENDENCIES.md) for detailed instructions.

**Quick install (Ubuntu/Debian):**
```bash
# From the opendash repository root directory:
sudo apt-get install nodejs npm python3 python3-pip imagemagick
pip3 install Pillow
cd common/fonts && npm install && cd ../..
```

### 3. Clone the Repository

```bash
git clone https://github.com/landracer/opendash.git
cd opendash
```

### 4. Open in VS Code

```bash
code opendash.code-workspace
```

Or: File → Open Workspace from File → Select `opendash.code-workspace`

### 5. Build Your First Display

**Center Display:**
```bash
cd center/
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

**Left/Right Gauge:**
```bash
cd left-right/
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

**GPS/Telemetry:**
```bash
cd gps/
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

**Replace `/dev/ttyUSB0` with your serial port:**
- Linux: `/dev/ttyUSB0` or `/dev/ttyACM0`
- macOS: `/dev/cu.usbmodem*`
- Windows: `COM3`, `COM4`, etc.

### 6. See It Run!

After flashing, you should see:
- **Center:** RPM gauge + 6 data sections + status bar
- **Left/Right:** Circular gauge with primary/secondary data
- **GPS:** Speed + lap timing + g-force visualization

Press **Ctrl+]** to exit the serial monitor.

---

## Using VS Code GUI (Alternative)

1. **Open project folder** in VS Code sidebar
2. Press **F1** → "ESP-IDF: Set Espressif device target" → **ESP32-S3**
3. Press **F1** → "ESP-IDF: Build your project"
4. Press **F1** → "ESP-IDF: Select port to use" → Select your device
5. Press **F1** → "ESP-IDF: Flash your project"
6. Press **F1** → "ESP-IDF: Monitor device"

Or use the status bar buttons: 🔧 Build | ⚡ Flash | 📺 Monitor

---

## Project Structure

```
opendash/
├── center/           ← Center display (4.3" LCD)
├── left-right/       ← Left/Right gauges (2.8" round)
├── gps/              ← GPS/Telemetry (1.75" AMOLED)
├── common/           ← Shared code (I2C, data models, etc.)
└── docs/             ← Documentation
```

Each display is an independent ESP-IDF project that can be built and flashed separately.

---

## Next Steps

### Customize Your Display

Each display shows configurable data points. Default layouts:

**Center Display (800×480):**
- Top: RPM arc gauge
- Grid: Coolant, Speed, Boost, Oil Temp, Lap Time, AFR
- Bottom: Status bar

**Left/Right Gauge (480×480 round):**
- Primary: Oil Temp (large)
- Secondary: Boost (smaller)
- Arc: Surrounding gauge bar

**GPS Unit (466×466 round):**
- Top: GPS Speed
- Middle: Lap time + delta
- Bottom: G-force circle + sat count

### Connect Multiple Displays

1. Build and flash all three displays
2. Wire I2C bus between units:
   - SDA: Connect all SDA pins together
   - SCL: Connect all SCL pins together
   - GND: Common ground
3. Center unit acts as I2C master
4. Data is shared across all displays

See [`docs/hardware.md`](docs/hardware.md) for wiring details.

### Add OBD2 Data

Connect an OBD2 adapter to the Center unit's CAN bus:
- CAN-H → Center display CAN-H pin
- CAN-L → Center display CAN-L pin
- Power and ground

See [`docs/hardware.md`](docs/hardware.md) for pin mappings.

---

## Troubleshooting

### Build fails: "Target not set"
```bash
idf.py set-target esp32s3
```

### Serial port not found
- **Linux:** `sudo usermod -a -G dialout $USER` (then log out/in)
- **Windows:** Install [USB drivers](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers)
- **macOS:** Look for `/dev/cu.usbmodem*` instead of `/dev/ttyUSB*`

### Display not turning on
- Check USB-C power connection
- Verify ESP32-S3 is properly powered (LED should light up)
- Check serial output: `idf.py monitor`

### Clean build
```bash
idf.py fullclean
idf.py build
```

---

## Documentation

| Document | Description |
|---|---|
| [`readme.md`](readme.md) | Main project overview |
| [`docs/setup-guide.md`](docs/setup-guide.md) | Detailed setup instructions |
| [`docs/vscode-setup.md`](docs/vscode-setup.md) | VS Code configuration guide |
| [`docs/architecture.md`](docs/architecture.md) | System architecture |
| [`docs/hardware.md`](docs/hardware.md) | Hardware specs and wiring |
| [`docs/data-points.md`](docs/data-points.md) | Available data points |
| [`center/README.md`](center/README.md) | Center display guide |
| [`left-right/README.md`](left-right/README.md) | Gauge pods guide |
| [`gps/README.md`](gps/README.md) | GPS/telemetry guide |

---

## Getting Help

- **Documentation:** Check the `docs/` folder
- **Issues:** [GitHub Issues](https://github.com/landracer/opendash/issues)
- **Hardware:** [Waveshare Wiki](https://www.waveshare.com/)
- **ESP-IDF:** [Espressif Docs](https://docs.espressif.com/projects/esp-idf/en/release-v5.3/)

---

**Ready to race? 🏎️💨**

Build your displays, connect them together, and hit the track!
