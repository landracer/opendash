<p align="center">
  <h1 align="center">🏁 OpenDash — Universal Racecar Dashboard</h1>
  <p align="center">
    An open-source, modular, bleeding-edge digital dashboard system for race cars.<br>
    Built on <strong>ESP-IDF v5.3</strong> + <strong>LVGL</strong> for three ESP32-S3 display units.
  </p>
</p>

---

## 📋 Quick Links — Display Projects

| Display Unit | Hardware | Resolution | Directory |
|---|---|---|---|
| **Center** (Main Dash) | [ESP32-S3-Touch-LCD-4.3](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-4.3) | 800×480 IPS | [`center/`](./center/) |
| **Left & Right** (Gauge Pods) | [ESP32-S3-LCD-2.8C](https://www.waveshare.com/wiki/ESP32-S3-LCD-2.8C) | 480×480 Round | [`left-right/`](./left-right/) |
| **GPS / Telemetry** | [ESP32-S3-Touch-AMOLED-1.75](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75) | 466×466 AMOLED | [`gps/`](./gps/) |

> **Shared code** lives in [`common/`](./common/) — I2C protocol, data models, OBD2 PIDs, display configuration, and the pre-flight checklist system.

---

## 🏗️ Repository Structure

```
opendash/
├── README.md                    ← You are here (landing page)
├── docs/                        ← Architecture, hardware, protocols, setup
│   ├── architecture.md          — System-level architecture & data flow
│   ├── hardware.md              — Hardware specifications & pin mappings
│   ├── i2c-protocol.md          — I2C inter-node communication protocol
│   ├── data-points.md           — Legend of all displayable data points
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
├── left-right/                  ← ESP32-S3-LCD-2.8C project
│   ├── main/
│   │   ├── main.c
│   │   ├── display_init.c/h     — ST7701 round LCD init
│   │   ├── ui_manager.c/h
│   │   └── assets/
│   ├── CMakeLists.txt
│   ├── sdkconfig.defaults
│   └── README.md
│
└── gps/                         ← ESP32-S3-Touch-AMOLED-1.75 project
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
```

---

## ✨ Key Features

### 🖥️ Display & UI
- **LVGL-based UI** — Gauges, arcs, bar charts, and numeric readouts with minimal CPU overhead
- **Configurable data views** — Choose which data points appear in each screen section
- **Touch-screen interaction** — Swipe between screens, tap to configure
- **Easy background/asset swaps** — Drop converted C-array images into `assets/` folders
- **Compartmentalized layout** — Each screen section is independently configurable

### 📡 Communication
- **I2C inter-node bus** — All three displays share data as I2C nodes
- **BMS integration** — I2C node for rAtTrax BMS data (cell voltages, temps, SOC)
- **OBD2 support** — Read any standard OBD2 PID (RPM, speed, coolant temp, boost, AFR, etc.)
- **CAN bus ready** — Center unit has onboard CAN for direct ECU communication

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
- **Status sharing** — Checklist state shared across all nodes via I2C

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

---

## 🚀 Getting Started

### Prerequisites

1. **ESP-IDF v5.3** — [Installation Guide](https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32s3/get-started/index.html)
2. **Visual Studio Code** with the [ESP-IDF Extension](https://marketplace.visualstudio.com/items?itemName=espressif.esp-idf-extension) (recommended)
3. **USB-C cable** and target hardware

### Build & Flash (Any Unit)

```bash
# Example: Build and flash the center display
cd center/
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

> See [`docs/setup-guide.md`](./docs/setup-guide.md) for detailed setup instructions.

---

## 📖 Documentation

| Document | Description |
|---|---|
| [`docs/architecture.md`](./docs/architecture.md) | System architecture, data flow, and node roles |
| [`docs/hardware.md`](./docs/hardware.md) | Hardware specs, pin mappings, and wiring |
| [`docs/i2c-protocol.md`](./docs/i2c-protocol.md) | I2C communication protocol between nodes |
| [`docs/data-points.md`](./docs/data-points.md) | Full legend of displayable data points |
| [`docs/setup-guide.md`](./docs/setup-guide.md) | Development environment setup |

---

## 🔗 Reference Links

- **ESP-IDF API Reference** — https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32s3/api-reference/index.html
- **LVGL Documentation** — https://docs.lvgl.io/master/
- **LVGL Examples** — https://docs.lvgl.io/master/examples.html
- **Waveshare ESP32-S3-Touch-LCD-4.3 Wiki** — https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-4.3
- **Waveshare ESP32-S3-LCD-2.8C Wiki** — https://www.waveshare.com/wiki/ESP32-S3-LCD-2.8C
- **Waveshare ESP32-S3-Touch-AMOLED-1.75 Wiki** — https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75

---

## 🤝 Contributing

This project is designed so that anyone can learn from, understand, and extend the code:

1. **All code is thoroughly annotated** — Every function, register write, and API call includes explanations referencing the ESP-IDF API docs
2. **Consistent structure** — All three display projects follow the same code layout
3. **Modular design** — Add new data sources, screens, or features without touching core code
4. **Documentation first** — Read the docs before diving into code

---

## 📄 License

This project is open source. See individual display project READMEs for specific details.

---

<p align="center">
  <strong>Built for racers, by racers. 🏎️💨</strong>
</p>
