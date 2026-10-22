<p align="center">
  <h1 align="center">🏁 OpenDash — Universal Racecar Dashboard</h1>
  <p align="center">
    A modular, bleeding-edge digital dashboard system for race cars.<br>
    Built on <strong>ESP-IDF v6.1</strong> + <strong>LVGL 9</strong> + <strong>ESP-NOW</strong> across a fleet of ESP32-S3 display and controller nodes.<br>
    <em>Licensed under Sovereign Individual License v1.0 — see LICENSE file</em>
  </p>
  <p align="center">
    <a href="https://github.com/landracer/opendash/actions/workflows/build.yml"><img src="https://github.com/landracer/opendash/actions/workflows/build.yml/badge.svg" alt="build-smoke"></a>
  </p>
</p>

---

## 📋 Quick Links — Display Projects

| Display Unit | Hardware | Resolution | Directory |
|---|---|---|---|
| **Center** (Main Dash) | [ESP32-S3-Touch-LCD-4.3](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-4.3) | 800×480 IPS | [`center/`](./center/) |
| **Left Gauge** | [ESP32-S3-LCD-2.8C](https://www.waveshare.com/wiki/ESP32-S3-LCD-2.8C) | 480×480 Round | [`left/`](./left/) |
| **Right Gauge** | [ESP32-S3-LCD-2.8C](https://www.waveshare.com/wiki/ESP32-S3-LCD-2.8C) | 480×480 Round | [`right/`](./right/) |
| **Pod 1** | [ESP32-S3-Touch-AMOLED-1.75](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75) | 466×466 Round | [`pod1/`](./pod1/) |
| **Pod 2** | [ESP32-S3-Touch-AMOLED-1.75](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75) | 466×466 Round | [`pod2/`](./pod2/) |
| **GPS / Telemetry** | [ESP32-S3-Touch-AMOLED-1.75](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75) | 466×466 AMOLED | [`gps/`](./gps/) |
| **MOS-4CH-A** | ESP32-WROOM-32E | N/A | [`mos-4ch-a/`](./mos-4ch-a/) |
| **MOS-4CH-B** | ESP32-WROOM-32E | N/A | [`mos-4ch-b/`](./mos-4ch-b/) |
| **Relay-4CH-HD** | ESP32-WROOM-32E | N/A | [`relay-4ch-hd/`](./relay-4ch-hd/) |
| **Relay-8CH-A** | ESP32-WROOM-32E | N/A | [`relay-8ch-a/`](./relay-8ch-a/) |
| **Relay-8CH-B** | ESP32-WROOM-32E | N/A | [`relay-8ch-b/`](./relay-8ch-b/) |
| **BMS Logger** *(ext)* | ESP32-DOIT-DevKit-V1 | SSD1306 128×64 OLED | External: `rAtTrax_BMS_Logger/` |

> **Shared code** lives in [`common/`](./common/) — ESP-NOW protocol, data models, OBD2 PIDs, display configuration, and the pre-flight checklist system.
>
> **🤖 AI agents & new contributors — start here:** [`agent/opendash.agent.md`](./agent/opendash.agent.md) is the orientation playbook (critical rules, architecture primer, workflow, common mistakes). Editor auto-load variants — [`agent/opendash.instructions.md`](./agent/opendash.instructions.md) and [`.claude/agents/opendash.agent.md`](./.claude/agents/opendash.agent.md) — defer to that canonical file.
>
> **Detailed project roadmap:** [`TODO.md`](./TODO.md) | **Central reference:** [`PROJECT_INDEX.md`](./PROJECT_INDEX.md)
>
> **Last updated:** 2026-10-21 (GPS canon propagation: the LC76G's hand-wired native-UART command path is
> now stated in every orientation doc — it is the interface that revived the node; the I2C CASIC mirror
> remains the unchanged data-read path. Dated-header convention rollout completed across `docs/`/`wiki/`/
> node READMEs; ESP32-S3 display units enumerate `/dev/ttyACM0…5` in all examples.)
>
> **A+ improvement program:** master game-plan [`docs/A_PLUS_GAMEPLAN.md`](./docs/A_PLUS_GAMEPLAN.md),
> live line-by-line audit ledger [`docs/gameplan-audit.md`](./docs/gameplan-audit.md) (append-only — every
> program change is logged there with its evidence; file renamed from the mispelled
> `gameplan-aduit.md` on 2026-10-10 — links re-pointed, history untouched)
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
│   ├── espnow-protocol.md       — ESP-NOW inter-node protocol (no wired bus)
│   ├── POLICY_GATE.md           — Roster policy-gate spec (A+ P1.3) — RATIFIED, wired into firmware; bench proof pending
│   ├── data-points.md           — Legend of all displayable data points
│   ├── font-system-testing.md   — Font system implementation and testing
│   └── setup-guide.md           — Development environment setup
│
├── common/                      ← Shared libraries (all units include this)
│   ├── include/                 — Public headers
│   │   ├── opendash_common.h
│   │   ├── opendash_protocol.h
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
│   ├── sdkconfig
│   └── README.md
│
├── left/                        ← Left gauge pod (ESP32-S3-LCD-2.8C)
│   ├── main/
│   │   ├── main.c               — Entry point; sole MD UART/HC-05 ingest (node 1)
│   │   ├── display_init.c/h     — ST7701S 3-wire SPI + RGB init
│   │   └── ui_manager.c/h       — Round gauge UI
│   ├── display.ini              — Hardware pin reference
│   ├── CMakeLists.txt
│   ├── sdkconfig
│   └── README.md
│
├── right/                       ← Right gauge pod (same hardware)
│   ├── main/                    — Mirrors left's gauge pages (node 2); no UART — all data via Center's ESP-NOW relay
│   ├── display.ini
│   ├── CMakeLists.txt
│   ├── sdkconfig
│   └── README.md
│
├── pod1/                        ← Pod 1 display unit (ESP32-S3-Touch-AMOLED-1.75)
│   ├── main/
│   │   ├── main.c               — Entry point, ESP-NOW slave (node 5)
│   │   ├── display_init.c/h     — CO5300 AMOLED init
│   │   ├── ui_manager.c/h       — Display UI
│   │   ├── imu_handler.c/h      — QMI8658 IMU driver
│   │   └── assets/              — Converted images (C arrays)
│   ├── CMakeLists.txt
│   ├── sdkconfig
│   └── README.md
│
├── pod2/                        ← Pod 2 display unit (ESP32-S3-Touch-AMOLED-1.75)
│   ├── main/
│   │   ├── main.c               — Entry point, ESP-NOW slave (node 6)
│   │   ├── display_init.c/h     — CO5300 AMOLED init
│   │   ├── ui_manager.c/h       — Display UI
│   │   ├── imu_handler.c/h      — QMI8658 IMU driver
│   │   └── assets/              — Converted images (C arrays)
│   ├── CMakeLists.txt
│   ├── sdkconfig
│   └── README.md
│
├── gps/                         ← GPS/Telemetry unit (ESP32-S3-Touch-AMOLED-1.75) — ✅ ACTIVE sensor node;
│                                    revived 2026-10-19 by the MANDATORY MOD: LC76G native UART hand-wired
│                                    (module TXD→GPIO18, RXD→GPIO17) — uart_bridge.c $PAIR command loop →
│                                    measured ≈10 Hz (wiki/GPS-LC76G-POSTMORTEM.md §12)
    ├── main/
    │   ├── main.c
    │   ├── display_init.c/h     — CO5300 AMOLED init
    │   ├── ui_manager.c/h
    │   ├── gps_handler.c/h      — LC76G DATA path — CASIC-over-I2C mirror read (unchanged)
    │   ├── uart_bridge.c/h      — LC76G native-UART COMMAND path — the hand-wired mod that revived
    │   │                          this node: $PAIR config readout + rate ladder → 10 Hz
    │   ├── imu_handler.c/h      — QMI8658 6-axis IMU
    │   ├── sd_logger.c/h        — SD telemetry logging (the real logger; center's sd_logger is a stub)
    │   └── assets/
    ├── CMakeLists.txt
    ├── sdkconfig
    └── README.md
│
├── mos-4ch-a/                   ← Headless 4-ch MOS FET controller (node 16) — hosts the
│   └── ...                          parachute actuator + roster gate (see mos-4ch-a/README.md)
├── mos-4ch-b/                   ← Headless 4-ch MOS FET controller (node 17) — see its README.md
├── relay-4ch-hd/                ← Headless 4-ch HD relay controller (node 13) — see its README.md
├── relay-8ch-a/                 ← Headless 8-ch relay controller (node 14) — see its README.md
├── relay-8ch-b/                 ← Headless 8-ch relay controller (node 15) — see its README.md
├── openDstream/                 ← ESP-NOW → UART data-point bridge to host PC — see its README.md
├── boostcontrol-staging/        ← frozen boost-controller review baseline (boost-controller-opendash.md
│   │                              Rule 1 — do not modify; clean-room rewrite gates v1.0.0)
├── left-right/                  ← [LEGACY] unified pre-split pod firmware — DO NOT BUILD,
│   │                              historical record of the pre-ESP-NOW architecture only
├── agent/                       ← AI-agent / contributor playbooks
├── wiki/                        ← Subsystem references + postmortems (see PROJECT_INDEX tables)
├── scripts/                     ← Tooling: check_docs.sh (docs-lint), coverage_gate.sh, od-flash.py…
├── test/                        ← Host unit-test + fuzz harness (Unity/ctest, CI unit-tests job)
│
├── rAtTrax_BMS_Logger/          ← External ESP-NOW node (separate repo)
│   └── See: rAtTrax_BMS_Logger/docs/opendash-integration.md
```

---

## ✨ Key Features

### 🖥️ Display & UI
- **LVGL-based UI** — Gauges, arcs, bar charts, and numeric readouts with minimal CPU overhead
- **Multi-page gauge system** — Left/Right pods: fixed gauge-page table (`s_gauge_pages[]` — oil, water, RPM pages + odometer screen) cycled via boot button. Same layout, different data per page.
- **Domain-separated datapoints** — MD-native sensor channels travel under dedicated `MD_*` ids (0x0800 block + `MD_RPM`); vehicle-ECU/OBD2 values keep the shared engine ids. Screens bind ids, so a value can never cross-feed another domain's widgets. `obd2_present` is a UI/config capability flag, never a wire-gate.
- **Min/max tracking** — Session high/low displayed per gauge page
- **Shift-light blink** — Arc flashes red/blue when RPM exceeds configurable threshold
- **Configurable data views** — Choose which data points appear in each screen section
- **Touch-screen support** — GT911 hardware reset sequence for reliable detection
- **Unit conversion** — °C/°F, kPa/BAR/PSI, km/h/MPH, km/mi — auto-applied to all displays
- **Easy background/asset swaps** — Drop converted C-array images into `assets/` folders
- **Warning system** — Flashing colored overlays (red/orange) for critical/caution alerts
- **Outlined text rendering** — 4-shadow technique for readable text over any background

### 📡 Communication
- **ESP-NOW wireless bus** — All nodes communicate wirelessly using ESP-NOW (WiFi peer-to-peer) instead of I2C due to hardware limitations and GPIO conflicts
- **BMS integration** — ESP-NOW node for rAtTrax BMS data (cell voltages, temps, SOC)
- **OBD2 support** — Standard OBD2 PIDs delivered today as decoded values inside the MultiDisplay ECU serial stream; direct ELM327 path planned
- **CAN integration (planned)** — TWAI/CAN ingest on the roadmap: onboard CAN header on the Center unit **and** a dedicated standalone CAN node, decoding VESC STATUS 1–6 + ECU frames into data points

### 🔧 Hardware Control Nodes
- **MOS-4CH-A/B** — Headless MOS FET controllers with boost control and relay switching
- **Relay-4CH-HD, Relay-8CH-A/B** — Headless relay controllers for fan, pump, and light control
- **openDstream** — ESP32-WROOM-32 bridge node: listens on the OpenDash ESP-NOW channel and re-emits each received data point as a `DP:0x…:v.vv` line over UART0 (onboard USB-UART bridge to the host PC, multidisplay-app Qt application). See [`openDstream/README.md`](./openDstream/README.md)
- **Channel conflict safeguards** — Runtime protections for shared GPIO channels between boost, relay, and parachute systems

### ⚠️ Important Note
The original design intended to use I2C for inter-node communication, but due to hardware limitations and GPIO conflicts, the system was re-implemented to use ESP-NOW (WiFi peer-to-peer) for communication between nodes. This provides zero-wire communication with no GPIO conflicts and better reliability.

### 🔥 Safety Deployment System
- **Parachute deployment** — Gyro-triggered safety system with configurable thresholds (deploy=45°, warning=25°, sustain=200ms, rate=300°/s)
- **Distributed voting** — Multiple nodes participate in deployment decision with rollover detection
- **Channel conflict safeguards** — Shared GPIO channels (CH0=GPIO16, CH1=GPIO17, CH2=GPIO26, CH3=GPIO27) protected from conflicts between boost, relay, and parachute systems

### 🛰️ GPS & Telemetry (GPS Unit)
- **LC76G GNSS** — Multi-constellation (GPS, GLONASS, BeiDou, Galileo) positioning
- **Dual-interface LC76G** — data still arrives over the I2C CASIC mirror (read path unchanged); **command/config runs over native UART** — the 2026-10-19 hand-wired MANDATORY MOD (module TXD→GPIO18, RXD→GPIO17) that revived this node: `$PAIR051` echo (parser alive) → `$PAIR050,100` → measured ≈10 Hz (`gps/main/uart_bridge.c`)
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
- **Future: Standard Layout Switcher** — Once multiple community-contributed layouts are available, end-users will be able to select from pre-built dashboard templates without coding. Not yet implemented — no design doc exists yet.

---

## 🚀 Getting Started

### Quick Start

**New to OpenDash?** See the [**Quick Start Guide**](QUICKSTART.md) for a 5-minute setup!

### Prerequisites

1. **ESP-IDF v6.1** — [Installation Guide](https://docs.espressif.com/projects/esp-idf/en/release-v6.1/esp32s3/get-started/index.html)
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
| [**Compile Errors Resolution**](docs/history/COMPILE_ERRORS_RESOLUTION.md) | **Troubleshooting compilation issues** |
| [`docs/vscode-setup.md`](docs/vscode-setup.md) | Visual Studio Code configuration guide |
| [`docs/setup-guide.md`](docs/setup-guide.md) | Detailed development environment setup |
| [`docs/architecture.md`](docs/architecture.md) | System architecture, data flow, and node roles |
| [`docs/hardware.md`](docs/hardware.md) | Hardware specs, pin mappings, and wiring |
| [`docs/espnow-protocol.md`](docs/espnow-protocol.md) | ESP-NOW communication protocol between nodes |
| [`docs/data-points.md`](docs/data-points.md) | Full legend of displayable data points |
| [`DATAFLOW.md`](DATAFLOW.md) | **End-to-end data path: MD UART → LEFT ingest → Center fan-out → pods (canon)** |
| [`FEATURES_AND_SENSORS.md`](FEATURES_AND_SENSORS.md) | Sensor sources & feature status matrix |
| [`UART_CONNECTION.md`](UART_CONNECTION.md) | MultiDisplay ECU UART/HC-05 link into LEFT |
| [`boost-controller-opendash.md`](boost-controller-opendash.md) | Boost controller master doc (clean-room rewrite gates v1.0.0) |
| [`PER_NODE_DISPLAY_CONFIG_SPEC.md`](PER_NODE_DISPLAY_CONFIG_SPEC.md) | Per-node layout/unit config spec (shipped) |
| [`docs/font-system-testing.md`](docs/font-system-testing.md) | Font system implementation and testing |
| [`center/README.md`](center/README.md) | **Center display project guide** — Display mode system, customization |
| [`left/README.md`](left/README.md) | Left gauge pod guide |
| [`right/README.md`](right/README.md) | Right gauge pod guide |
| [`pod1/README.md`](pod1/README.md) | Pod 1 display + safety-deployment unit (CO5300 AMOLED, QMI8658 IMU votes) |
| [`pod2/README.md`](pod2/README.md) | Pod 2 display + safety-deployment unit (same hardware as pod1) |
| [`gps/README.md`](gps/README.md) | GPS/Telemetry unit guide |
| [`mos-4ch-a/README.md`](mos-4ch-a/README.md) | MOS-4CH-A headless controller — roster gate LIVE, parachute actuator host |
| [`mos-4ch-b/README.md`](mos-4ch-b/README.md) | MOS-4CH-B headless controller — same firmware shape as MOS-4CH-A |
| [`relay-4ch-hd/README.md`](relay-4ch-hd/README.md) | Relay-4CH-HD headless relay controller (pre-P1.3 latch — gate rewrite pending) |
| [`relay-8ch-a/README.md`](relay-8ch-a/README.md) | Relay-8CH-A headless relay controller (pre-P1.3 latch — gate rewrite pending) |
| [`relay-8ch-b/README.md`](relay-8ch-b/README.md) | Relay-8CH-B headless relay controller (pre-P1.3 latch — gate rewrite pending) |
| [`wiki/`](wiki/) | **Wiki documentation** — Additional project documentation and integration guides |
| [`wiki/system-overview.md`](wiki/system-overview.md) | **★ End-user system guide — start here for usage** |
| [`wiki/relay-mos-controllers.md`](wiki/relay-mos-controllers.md) | **Relay & MOS FET controller documentation** |
| [`wiki/boost-controller.md`](wiki/boost-controller.md) | **Boost controller implementation details** |
| [`wiki/pod1-pod2.md`](wiki/pod1-pod2.md) | **Pod 1 and Pod 2 documentation** |
| [`wiki/safety-deployment-system.md`](wiki/safety-deployment-system.md) | **Safety deployment system documentation** |
| [`wiki/ota-bluetooth.md`](wiki/ota-bluetooth.md) | **★ BLE OTA step-by-step guide (Linux desktop)** |
| [`wiki/ota-android-plan.md`](wiki/ota-android-plan.md) | Roadmap & options for Android-based OTA |
| [`BLE_OTA.md`](BLE_OTA.md) | BLE OTA architecture & root-cause reference |
| [`BLUETOOTH_PAIRING.md`](BLUETOOTH_PAIRING.md) | BLE pairing/bonding design behind ENTER_BT_OTA |
| [`QUICK_REFERENCE.md`](QUICK_REFERENCE.md) | Center warning-box + screen API quick reference |
| [`TEARING.md`](TEARING.md) | Center RGB tearing investigation (CLOSED — residual shimmer is physical) |

> **Note:** The project includes extensive documentation in the [`wiki/`](wiki/) directory with additional integration guides and technical details.

---

## 🔗 Reference Links

- **ESP-IDF API Reference** — https://docs.espressif.com/projects/esp-idf/en/release-v6.1/esp32s3/api-reference/index.html
- **LVGL Documentation** — https://docs.lvgl.io/master/
- **LVGL Examples** — https://docs.lvgl.io/master/examples.html
  https://github.com/lvgl/lvgl/tree/master/examples
- **Waveshare ESP32-S3-Touch-LCD-4.3 Wiki** — https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-4.3
- **Waveshare ESP32-S3-LCD-2.8C Wiki** — https://www.waveshare.com/wiki/ESP32-S3-LCD-2.8C
- **Waveshare ESP32-S3-Touch-AMOLED-1.75 Wiki** — https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75

---

## 🏷️ Versioning

Public versioning starts at **v0.1.0 (2026-10-02)** — the first true baseline.
Everything before it was internal *beta codenames* (`v0.2.0-beta` … `v0.9.0-beta`),
never publicly versioned — all folded into the baseline (see
[`CHANGELOG.md`](CHANGELOG.md) mapping table).

- **Single source of truth:** the `OPENDASH_VERSION_*` defines in
  [`common/include/opendash_common.h`](common/include/opendash_common.h). The version
  string is *derived* from them — boot banners, the center splash, and the BLE OTA
  version response all read that one define. Bump the numbers there, nowhere else.
- **Scheme:** semver — minor = features, patch = fixes. 0.x is the public beta line.
- **v1.0.0 (GA) gates:** boost-controller clean-room rewrite (GPL-3.0 lineage —
  required before public distribution) + full-fleet BLE OTA hardening (TODO §1.4).

---

## 🤝 Contributing

This is a proprietary project distributed **source-available / private** under the
[Sovereign Individual License v1.0](./LICENSE) — all rights reserved; it is **not** a
public open-source release (see the release-gate notice below). The codebase is designed
for clarity and maintainability:

1. **All code is thoroughly annotated** — Every function, register write, and API call includes explanations referencing the ESP-IDF API docs
2. **Consistent structure** — Every node project (center, left, right, gps, pods, relay/MOS controllers) follows the same code layout
3. **Modular design** — Add new data sources, screens, or features without touching core code
4. **Documentation first** — Read the docs before diving into code

---

## 📄 License

Copyright © 2024–2026 **OpenDash** & **landracer**.
All rights reserved. See [`LICENSE`](./LICENSE) for details.

> **Release-gate notice:** the boost-controller subsystem descends from GPL-3.0
> lineage code and **requires a clean-room rewrite before any public
> distribution** (tracked in TODO §6.0; blocks v1.0.0). Until then this project
> stays private / source-available — do not publish or redistribute it as-is.

---

<p align="center">
  <strong>Built for racers, by racers. 🏎️💨</strong>
</p>
