<p align="center">
  <h1 align="center">🏁 OpenDash — Universal Racecar Dashboard</h1>
  <p align="center">
    A modular, bleeding-edge digital dashboard system for race cars.<br>
    Built on <strong>ESP-IDF v6.1</strong> + <strong>LVGL 9</strong> + <strong>ESP-NOW</strong> for three ESP32-S3 display units.<br>
    <em>Licensed under Sovereign Individual License v1.0 — see LICENSE file</em>
  </p>
</p>

---

## 📋 Quick Links — Display Projects

| Display Unit | Hardware | Resolution | Directory |
|---|---|---|---|
| **Center** (Main Dash) | [ESP32-S3-Touch-LCD-4.3](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-4.3) | 800×480 IPS | [`center/`](./center/) |
| **Left Gauge** | [ESP32-S3-LCD-2.8C](https://www.waveshare.com/wiki/ESP32-S3-LCD-2.8C) | 480×480 Round | [`left/`](./left/) |
| **Right Gauge** | [ESP32-S3-LCD-2.8C](https://www.waveshare.com/wiki/ESP32-S3-LCD-2.8C) | 480×480 Round | [`right/`](./right/) |
| **GPS / Telemetry** | [ESP32-S3-Touch-AMOLED-1.75](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75) | 466×466 AMOLED | [`gps/`](./gps/) |
| **BMS Logger** *(ext)* | ESP32-DOIT-DevKit-V1 | SSD1306 128×64 OLED | External: `rAtTrax_BMS_Logger/` |

> **Shared code** lives in [`common/`](./common/) — ESP-NOW protocol, data models, OBD2 PIDs, display configuration, and the pre-flight checklist system.
>
> **Detailed project roadmap:** [`TODO.md`](./TODO.md) | **Central reference:** [`PROJECT_INDEX.md`](./PROJECT_INDEX.md)
>
> **Additional documentation:** The project includes extensive documentation in the [`wiki/`](./wiki/) directory with integration guides and technical details.

---

## 🏗️ Repository Structure

```
opendash/
├── readme.md                    ← You are here (landing page)
├── docs/                        ← Architecture, hardware, protocols, setup
│   ├── architecture.md          — System-level architecture & data flow
│   ├── hardware.md              — Hardware specifications & pin mappings
│   ├── i2c-protocol.md          — I2C inter-node communication protocol
│   ├── data-points.md           — Legend of all displayable data points
│   ├── font-system-testing.md   — Font system implementation and testing
│   └── setup-guide.md           — Development environment setup
│
├── common/                      ← Shared libraries (all units include this)
│   ├── include/                 — Public headers
│   │   ├── opendash_common.h
│   │   ├── opendash_i2c_protocol.h
│   │   ├── opendash_data_model.h
│   │   ├── opendash_obd2.h
│   │   ├── opendash_display_config.h
│   │   ├── opendash_checklist.h
│   │   └── opendash_wifi_ble.h
│   └── src/                     — Implementations
│
├── center/                      ← ESP32-S3-Touch-LCD-4.3 project
│   ├── main/
│   │   ├── main.c               — Entry point
│   │   ├── display_init.c/h     — LCD & touch initialization
│   │   ├── ui_manager.c/h       — LVGL screen/widget management
│   │   └── assets/              — Converted images (C arrays)
│   ├── CMakeLists.txt
│   ├── sdkconfig.defaults
│   └── README.md
│
├── left/                        ← Left gauge pod (ESP32-S3-LCD-2.8C)
│   ├── main/
│   │   ├── main.c               — Entry point, I2C slave (addr 0x10)
│   │   ├── display_init.c/h     — ST7701S 3-wire SPI + RGB init
│   │   └── ui_manager.c/h       — Round gauge UI
│   ├── display.ini              — Hardware pin reference
│   ├── CMakeLists.txt
│   ├── sdkconfig.defaults
│   └── README.md
│
├── right/                       ← Right gauge pod (same hardware)
│   ├── main/                    — Same code as left/, uses addr 0x11
│   ├── display.ini
│   ├── CMakeLists.txt
│   ├── sdkconfig.defaults
│   └── README.md
│
├── gps/                         ← ESP32-S3-Touch-AMOLED-1.75 project
    ├── main/
    │   ├── main.c
    │   ├── display_init.c/h     — CO5300 AMOLED init
    │   ├── ui_manager.c/h
    │   ├── gps_handler.c/h      — LC76G GNSS module
    │   ├── imu_handler.c/h      — QMI8658 6-axis IMU
    │   ├── parachute.c/h        — Gyro-triggered parachute deployment
    │   └── assets/
    ├── CMakeLists.txt
    ├── sdkconfig.defaults
    └── README.md

├── rAtTrax_BMS_Logger/          ← External ESP-NOW node (separate repo)
│   └── See: rAtTrax_BMS_Logger/docs/opendash-integration.md
```

---

## ✨ Key Features

### 🖥️ Display & UI
- **LVGL-based UI** — Gauges, arcs, bar charts, and numeric readouts with minimal CPU overhead
- **Multi-page gauge system** — Left/Right pods: up to 8 configurable gauge pages (oil, water, RPM, etc.) cycled via boot button. Same layout, different data per page.
- **Min/max tracking** — Session high/low displayed per gauge page
- **Shift-light blink** — Arc flashes red/blue when RPM exceeds configurable threshold
- **Configurable data views** — Choose which data points appear in each screen section
- **Touch-screen support** — GT911 hardware reset sequence for reliable detection
- **Unit conversion** — °C/°F, kPa/BAR/PSI, km/h/MPH, km/mi — auto-applied to all displays
- **Easy background/asset swaps** — Drop converted C-array images into `assets/` folders
- **Warning system** — Flashing colored overlays (red/orange) for critical/caution alerts
- **Outlined text rendering** — 4-shadow technique for readable text over any background

### 📡 Communication
- **ESP-NOW wireless bus** — All three displays communicate wirelessly using ESP-NOW (WiFi peer-to-peer) instead of I2C due to hardware limitations and GPIO conflicts
- **BMS integration** — ESP-NOW node for rAtTrax BMS data (cell voltages, temps, SOC)
- **OBD2 support** — Read any standard OBD2 PID (RPM, speed, coolant temp, boost, AFR, etc.)
- **CAN bus ready** — Center unit has onboard CAN for direct ECU communication

### ⚠️ Important Note
The original design intended to use I2C for inter-node communication, but due to hardware limitations and GPIO conflicts, the system was re-implemented to use ESP-NOW (WiFi peer-to-peer) for communication between nodes. This provides zero-wire communication with no GPIO conflicts and better reliability.

### 🛰️ GPS & Telemetry (GPS Unit)
- **LC76G GNSS** — Multi-constellation (GPS, GLONASS, BeiDou, Galileo) positioning
- **Predictive lap timing** — Real-time delta vs. best lap, sector-based predictions
- **QMI8658 6-axis IMU** — Accelerometer + gyroscope for g-force, orientation, motion
- **Parachute deployment** — Gyro-triggered safety system with configurable thresholds

### 📊 Data Logging
- **SD card logging** — CSV format, configurable sample rate, auto-session management
- **Per-session files** — Automatic file naming with timestamps
- **Post-session analysis** — Compatible with common data analysis tools

### 📋 Pre-Flight Checklist
- **Crew task lists** — Customizable per-team checklists before each run
- **Touch confirmation** — Tap to mark items complete on any display
- **Status sharing** — Checklist state shared across all nodes via ESP-NOW

### 📶 Connectivity
- **WiFi mode** — For OTA firmware updates and data transfer to companion app
- **BLE mode** — For low-power data sync with Android/iOS companion app
- **Individual control** — Each unit manages its own wireless independently
- **Future Android app** — Planned companion for configuration and data review

### 🔧 Customization & Extensibility
- **Data point legend** — Full list of displayable values (see [`docs/data-points.md`](./docs/data-points.md))
- **Modular sensor support** — Add custom sensors via I2C/SPI/ADC
- **Programmable alarms** — Threshold-based warnings for any data point
- **Drag-and-drop assets** — Convert images with LVGL tools, drop into `assets/`
- **Display Mode System** — Center display supports multiple cycling data views (ENGINE, GPS, custom modes) with zero memory overhead. See [`center/README.md`](center/README.md) for customization guide.
- **Future: Standard Layout Switcher** — Once multiple community-contributed layouts are available, end-users will be able to select from pre-built dashboard templates without coding. See [DISPLAY_MODE_REFACTORING.md](DISPLAY_MODE_REFACTORING.md) for technical roadmap.

---

## 🚀 Getting Started

### Quick Start

**New to OpenDash?** See the [**Quick Start Guide**](QUICKSTART.md) for a 5-minute setup!

### Prerequisites

1. **ESP-IDF v5.3** — [Installation Guide](https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32s3/get-started/index.html)
2. **Node.js + npm** — For font conversion (required)
3. **Python 3 + Pillow + ImageMagick** — For image conversion (required)
4. **Visual Studio Code** with the [ESP-IDF Extension](https://marketplace.visualstudio.com/items?itemName=espressif.esp-idf-extension) (recommended) — See [VS Code Setup Guide](docs/vscode-setup.md)
5. **USB-C cable** and target hardware

> **📦 Complete dependency installation guide:** [BUILD_DEPENDENCIES.md](BUILD_DEPENDENCIES.md)

### Build & Flash (Any Unit)

#### Command Line

```bash
# Example: Build and flash the center display
cd center/
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

#### Visual Studio Code

1. Open `opendash.code-workspace` in VS Code
2. Open a file from the project you want to build (e.g., `center/main/main.c`)
3. Press **F1** → "ESP-IDF: Set Espressif device target" → **ESP32-S3**
4. Press **F1** → "ESP-IDF: Build your project"
5. Press **F1** → "ESP-IDF: Flash your project"

> See [`docs/vscode-setup.md`](docs/vscode-setup.md) for detailed VS Code setup instructions.

> See [`docs/setup-guide.md`](./docs/setup-guide.md) for detailed setup instructions.

---

## 📖 Documentation

| Document | Description |
|---|---|
| [**PROJECT INDEX**](PROJECT_INDEX.md) | **★ Central glossary & index — maps the entire project** |
| [**Quick Start Guide**](QUICKSTART.md) | **5-minute setup guide — start here!** |
| [**Build Dependencies**](BUILD_DEPENDENCIES.md) | **Complete dependency installation guide** |
| [**Compile Errors Resolution**](docs/archived/COMPILE_ERRORS_RESOLUTION.md) | **Troubleshooting compilation issues** |
| [**Display Mode Refactoring**](DISPLAY_MODE_REFACTORING.md) | **Technical deep-dive: Center display architecture redesign** |
| [`docs/vscode-setup.md`](docs/vscode-setup.md) | Visual Studio Code configuration guide |
| [`docs/setup-guide.md`](docs/setup-guide.md) | Detailed development environment setup |
| [`docs/architecture.md`](docs/architecture.md) | System architecture, data flow, and node roles |
| [`docs/hardware.md`](docs/hardware.md) | Hardware specs, pin mappings, and wiring |
| [`docs/i2c-protocol.md`](docs/i2c-protocol.md) | ESP-NOW communication protocol between nodes |
| [`docs/data-points.md`](docs/data-points.md) | Full legend of displayable data points |
| [`docs/font-system-testing.md`](docs/font-system-testing.md) | Font system implementation and testing |
| [`center/README.md`](center/README.md) | **Center display project guide** — Display mode system, customization |
| [`left/README.md`](left/README.md) | Left gauge pod guide |
| [`right/README.md`](right/README.md) | Right gauge pod guide |
| [`gps/README.md`](gps/README.md) | GPS/Telemetry unit guide |
| [`wiki/`](wiki/) | **Wiki documentation** — Additional project documentation and integration guides |
| [`wiki/system-overview.md`](wiki/system-overview.md) | **★ End-user system guide — start here for usage** |
| [`wiki/ota-bluetooth.md`](wiki/ota-bluetooth.md) | **★ BLE OTA step-by-step guide (Linux desktop)** |
| [`wiki/ota-android-plan.md`](wiki/ota-android-plan.md) | Roadmap & options for Android-based OTA |
| [`BLE_OTA.md`](BLE_OTA.md) | BLE OTA architecture & root-cause reference |

> **Note:** The project includes extensive documentation in the [`wiki/`](wiki/) directory with additional integration guides and technical details.

---

## 🔗 Reference Links

- **ESP-IDF API Reference** — https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32s3/api-reference/index.html
- **LVGL Documentation** — https://docs.lvgl.io/master/
- **LVGL Examples** — https://docs.lvgl.io/master/examples.html
  https://github.com/lvgl/lvgl/tree/master/examples **LVGL E
- **Waveshare ESP32-S3-Touch-LCD-4.3 Wiki** — https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-4.3
- **Waveshare ESP32-S3-LCD-2.8C Wiki** — https://www.waveshare.com/wiki/ESP32-S3-LCD-2.8C
- **Waveshare ESP32-S3-Touch-AMOLED-1.75 Wiki** — https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75

---

## 🤝 Contributing

This is a proprietary project — all rights reserved. The codebase is designed for clarity and maintainability:

1. **All code is thoroughly annotated** — Every function, register write, and API call includes explanations referencing the ESP-IDF API docs
2. **Consistent structure** — All three display projects follow the same code layout
3. **Modular design** — Add new data sources, screens, or features without touching core code
4. **Documentation first** — Read the docs before diving into code

---

## 📄 License

Copyright © 2024–2026 **uknowmelast** & **Axiom** (AI Co-Architect).
All rights reserved. See [`LICENSE`](./LICENSE) for details.

---

<p align="center">
  <strong>Built for racers, by racers. 🏎️💨</strong><br>
  <sub>Designed by uknowmelast • Architected with Axiom</sub>
</p>
