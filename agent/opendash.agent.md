<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OpenDash Agent

## Identity
Specialized agent for the **OpenDash** project — a modular, multi-display racecar dashboard system built on ESP-IDF + LVGL 9 + ESP-NOW for ESP32-S3 hardware.

---

## ⚠️ CRITICAL RULES — READ BEFORE EVERY TASK ⚠️

### Rule 1: NEVER DELETE OR REPLACE FILES
- **NEVER** use `rm`, `unlink`, or create a new file to overwrite an existing one.
- **ALWAYS** edit files in-place using targeted text replacement.
- If a file seems wrong or incomplete, **ask the user first** and show evidence before changing it.
- Documentation files are maintained carefully — do not recreate, truncate, or replace them.

### Rule 2: ONLY ADD CODE — NEVER REMOVE WORKING CODE
- **NEVER** remove existing, working code from any source file without explicit user consent.
- The firmware for each node is production-tested on real hardware. Do not "clean up" or "simplify" working code.
- When adding features, **integrate alongside** existing code — preserve what works.
- If existing code conflicts with new code, explain the conflict and ask how to proceed.

### Rule 3: VERIFY BEFORE CLAIMING COMPLETE
- **NEVER** mark TODO items as complete unless the feature builds, flashes, and runs.
- A successful `idf.py build` is the minimum bar. Untested code is not complete.
- If partially done, mark `[~]` (in progress), not `[x]`.

### Rule 4: MATCH THE PROJECT'S FRAMEWORK EXACTLY
- This project uses **ESP-IDF** (v5.3/v6.1) with CMake build system.
- All nodes are **ESP32-S3** with PSRAM, LVGL 9 UI, and ESP-NOW communication.
- Do NOT use Arduino APIs. Do NOT create PlatformIO files.
- Build commands use `idf.py`, not `pio`.

### Rule 5: RESPECT THE MULTI-NODE ARCHITECTURE
- **Center** is the ESP-NOW master — it discovers and manages all other nodes.
- **Left, Right, GPS** are peripheral nodes — they respond to PINGs and send data.
- **rAtTrax-BMS** is an external peripheral node (different repo, Arduino framework).
- Never change the center's node discovery or message forwarding logic without understanding the full flow.
- All ESP-NOW messages use the OpenDash protocol frame: `SYNC(0xAA) + CMD + LEN + PAYLOAD + CHECKSUM`.

### Rule 6: DO NOT MODIFY COMMON LIBRARY LIGHTLY
- `common/include/` and `common/src/` are shared by ALL nodes.
- Changing a header or source file in common/ affects center, left, right, GPS, and potentially rAtTrax-BMS.
- Always consider all consumers before editing common code.

---

## Project Description

OpenDash is a 4-node (optionally 5 with rAtTrax-BMS) racecar dashboard system:
- **Center** (4.3" IPS LCD): Main dashboard display, ESP-NOW master, data hub
- **Left** (2.8" round LCD): Gauge pod, receives data from center
- **Right** (2.8" round LCD): Gauge pod, identical to left
- **GPS** (1.75" AMOLED): GPS/IMU sensor node, active data broadcaster
- **BMS** (external): rAtTrax BMS Logger, battery/motor telemetry node

Data flows: MultiDisplay (VR6 ECU) → HC-05 Bluetooth → Left pod (UART RX) → ESP-NOW → Center → Left/Right/GPS

---

## Architecture

### Build System
- **ESP-IDF** v5.3 (left/right/GPS) or v6.1 (center)
- CMake-based with `idf_component.yml` managed components
- Shared common library via `EXTRA_COMPONENT_DIRS`
- Target: ESP32-S3, 240 MHz, 16MB flash, 8MB PSRAM

### Build Commands (per node)
```bash
cd /home/sysadmin/Documents/rAtTrax-Dash/opendash/{center|left|right|gps}
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor    # Flash + serial monitor
```

### Source Layout
```
opendash/
├── common/              ← Shared by ALL nodes (headers + sources)
│   ├── include/         ← Public API headers
│   │   ├── opendash_common.h        — Node types, error codes
│   │   ├── opendash_data_model.h    — ALL data point ID definitions
│   │   ├── opendash_i2c_protocol.h  — Message format (frame structure)
│   │   ├── opendash_espnow.h        — ESP-NOW transport layer
│   │   ├── opendash_uart.h          — MultiDisplay UART parser
│   │   └── ...
│   └── src/             ← Implementations
├── center/main/         ← Center node firmware
│   ├── main.c           — Entry point, init sequence
│   ├── espnow_master.c  — ESP-NOW master: discover, receive, forward
│   ├── ui_manager.c     — LVGL screens and widgets
│   └── display_init.c   — LCD hardware init
├── left/main/           ← Left gauge pod firmware
│   ├── main.c           — Entry point, ESP-NOW receiver, odometer
│   ├── ui_manager.c     — Round gauge UI (arc, labels, min/max)
│   └── display_init.c   — ST7701S display init
├── right/main/          ← Right gauge pod (identical code to left, addr 0x11)
├── gps/main/            ← GPS/IMU sensor node
│   ├── main.c           — Entry point, ESP-NOW broadcaster (5 Hz)
│   ├── gps_handler.c    — LC76G GNSS via I2C CASIC protocol
│   ├── imu_handler.c    — QMI8658 6-axis IMU
│   ├── sd_logger.c      — SD card logging (5 Hz mixed CSV)
│   └── display_init.c   — CO5300 AMOLED init
└── docs/                ← Architecture, protocol specs, hardware docs
```

### ESP-NOW Mesh Topology
```
                    ┌──────────┐
    PING broadcast  │  CENTER  │  Forwards data to
    ────────────────│  MASTER  │────────────────────
    │               └──────────┘               │
    │                    │                     │
    ▼                    ▼                     ▼
┌────────┐        ┌──────────┐          ┌──────────┐
│  LEFT  │        │   GPS    │          │  RIGHT   │
│ 0x10   │        │  0x12    │          │  0x11    │
│ gauge  │        │ sensor   │          │  gauge   │
└────────┘        └──────────┘          └──────────┘
    ▲                                        
    │  UART (HC-05 BT)                       
┌────────────┐                          ┌──────────┐
│MultiDisplay│                          │rAtTrax   │
│  (VR6 ECU) │                          │BMS Logger│
└────────────┘                          │ 0x20 ext │
                                        └──────────┘
                                    (ESP-NOW broadcast)
```

### Protocol Frame Format
Every ESP-NOW payload is an OpenDash protocol message:
```
| SYNC (0xAA) | CMD (1B) | LENGTH (1B) | PAYLOAD (0-248B) | CHECKSUM (1B) |
```
Checksum = XOR of SYNC, CMD, LENGTH, and all PAYLOAD bytes.

### Key Commands
| CMD | Hex | Direction | Purpose |
|-----|-----|-----------|---------|
| SET_DATA_POINT | 0x01 | Master→Slave | Push gauge/display values |
| DATA_RESPONSE | 0x81 | Slave→Master | Send sensor data to center |
| STATUS_REPORT | 0x82 | Slave→Master | PING response (node discovery) |
| SYSTEM | 0x07 | Master→Slave | PING, reboot, time sync |
| SET_ALARM | 0x03 | Master→Slave | Warning thresholds |

### Data Point ID Ranges
| Range | Source | Description |
|-------|--------|------------|
| 0x0001–0x00FF | MultiDisplay/ECU | RPM, boost, lambda, EGTs, temps, pressures |
| 0x0200–0x02FF | GPS node | Speed, heading, coordinates, satellites |
| 0x0300–0x03FF | IMU | G-forces, pitch, roll, yaw |
| 0x0400–0x04FF | rAtTrax BMS | Pack V/I, SOC, cell voltages, temp, power |
| 0x0500–0x05FF | System | CPU temp, heap, RSSI, uptime |
| 0x0600–0x06FF | VESC (via BMS) | eRPM, current, duty, temps, fault |
| 0x0620–0x0624 | RPM (via BMS) | 4-wheel RPM + avg speed |

---

## Node-Specific Details

### Center (espnow_master.c)
- Broadcasts PING at ~50 Hz to discover nodes
- Receives DATA_RESPONSE → updates LVGL UI + forwards to gauge pods as SET_DATA_POINT
- Logs all data points to SD card
- Pushes demo data when no real engine data detected (auto-halts on real data)
- Tracks 3 node slots (expandable): LEFT, RIGHT, GPS
- **rAtTrax-BMS node**: Center will auto-discover if BMS sends STATUS_REPORT in response to PING

### Left/Right (gauge pods)
- Passive receivers — wait for SET_DATA_POINT from center
- Respond to PING with STATUS_REPORT
- Capture GPS speed for odometer accumulation
- Left pod additionally receives MultiDisplay UART data and relays to center
- Multi-page gauge system with configurable data point display

### GPS (active sensor node)
- **5 Hz data broadcast task**: GPS + IMU data proactively sent
- Responds to REQUEST_DATA with specific sensor values
- GPS via LC76G I2C CASIC protocol (not UART)
- IMU via QMI8658 6-axis
- SD logging at 5 Hz (mixed CSV format)
- TIME_SYNC broadcast every 2 seconds (when GPS fix valid)

---

## Documentation
| File | Content |
|------|---------|
| `TODO.md` | **Check FIRST** — comprehensive project roadmap |
| `PROJECT_INDEX.md` | Central navigation reference |
| `docs/architecture.md` | System-level architecture |
| `docs/hardware.md` | Pin maps, board specs |
| `docs/i2c-protocol.md` | Full protocol specification |
| `docs/data-points.md` | Data point ID legend |
| `UART_CONNECTION.md` | MultiDisplay serial protocol |
| `BLUETOOTH_PAIRING.md` | HC-05/HC-06 pairing guide |
| `FEATURES_AND_SENSORS.md` | Sensor capability matrix |
| `CHANGELOG.md` | Release history |
| Each node `README.md` | Node-specific documentation |

### Related rAtTrax-BMS Files (cross-reference)
| File | Content |
|------|---------|
| `rAtTrax_BMS_Logger/docs/opendash-integration.md` | BMS-side ESP-NOW integration guide |
| `rAtTrax_BMS_Logger/TODO.md` | BMS project roadmap (§11 = OpenDash integration) |

---

## Common Mistakes to Avoid
1. Editing `common/` headers without checking impact on ALL nodes
2. Changing ESP-NOW channel — must be 1 on ALL nodes including rAtTrax-BMS
3. Using Arduino APIs — this is ESP-IDF, use `esp_*` functions
4. Forgetting `vTaskDelay()` yields in tight loops (WDT will fire on CPU0)
5. Assuming I2C bus availability — multiple devices share buses, check for conflicts
6. Changing the protocol frame format — ALL nodes (including external BMS) must match
7. Modifying `sdkconfig.defaults` without understanding cascading effects on WiFi/BT/PSRAM

---

## Workflow for Any Task
1. Read `TODO.md` to understand where this task fits in the roadmap
2. Read relevant source files before making any changes
3. Check if the change affects `common/` — if so, consider all 4 nodes
4. Make targeted, minimal edits
5. Build with `idf.py build` after every change
6. If modifying ESP-NOW, verify against the protocol docs and test with all active nodes
7. Update `TODO.md` honestly
8. Never touch rAtTrax-BMS files without switching to that project's context
