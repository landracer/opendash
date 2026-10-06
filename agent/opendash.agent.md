<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OpenDash Agent

## Identity
Specialized agent for the **OpenDash** project — a modular, multi-display racecar dashboard and vehicle-control system built on ESP-IDF v6.1 + LVGL 9 + ESP-NOW across twelve ESP32 node families.

> **Last updated:** 2026-10-03 (v0.1.0 baseline — cross-checked against `opendash_protocol.h`, `opendash_common.h`, `espnow_master.c`).
> **This is the canonical agent playbook.** The IDE entry points
> (`.claude/agents/opendash.agent.md`, `agent/opendash.instructions.md`) defer to this file.
> Linked from [`readme.md`](../readme.md) → Quick Links: any task that enters via the readme finds this file first.

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
- This project uses **ESP-IDF** (v6.1 line, pinned dev snapshot — see BUILD_DEPENDENCIES.md) with CMake build system.
- All nodes are **ESP32-S3** with PSRAM, LVGL 9 UI, and ESP-NOW communication.
- Do NOT use Arduino APIs. Do NOT create PlatformIO files.
- Build commands use `idf.py`, not `pio`.

### Rule 5: RESPECT THE MULTI-NODE ARCHITECTURE
- The fleet is **12 node families** (18 logical slots, `OPENDASH_NODE_COUNT`): center (ESP-NOW master), left, right, gps, pod1, pod2, mos-4ch-a/b, relay-4ch-hd, relay-8ch-a/b, openDstream — plus the external rAtTrax-BMS logger (different repo, Arduino framework).
- **Event-driven — NO polling, NO PING loop** (`espnow_master.c` enforces this). Slaves self-announce and push data (`DATA_RESPONSE 0x81` / `DATA_BATCH 0x88`); center fans out (`SET_DATA_POINT 0x01` / `SET_DATA_BATCH 0x0C`) over 4 priority channels with per-peer quarantine/backoff; offline = data absence.
- Nodes are addressed by 48-bit WiFi MAC and identified logically by `opendash_node_t` — there are no bus addresses.
- Never change the center's peer management or message forwarding logic without understanding the full flow (see [`DATAFLOW.md`](../DATAFLOW.md)).
- All ESP-NOW messages use the OpenDash protocol frame: `SYNC(0xAA) + CMD + LEN + PAYLOAD + CHECKSUM` (XOR checksum).

### Rule 6: DO NOT MODIFY COMMON LIBRARY LIGHTLY
- `common/include/` and `common/src/` are shared by ALL nodes.
- Changing a header or source file in common/ affects every display node, the relay/MOS controllers, openDstream, and potentially rAtTrax-BMS.
- Always consider all consumers before editing common code.

---

## Project Description

OpenDash is a modular racecar dashboard + vehicle-control system (v0.1.0 baseline), twelve node families (optionally + external rAtTrax-BMS):
- **Center** (4.3" IPS LCD): Main dashboard display, ESP-NOW master, data aggregator, layout/boost authoring
- **Left** (2.8" round LCD): Gauge pod + sole MultiDisplay UART ingest (MD/OBD domain split — see DATAFLOW.md)
- **Right** (2.8" round LCD): Gauge pod — everything it shows is relayed by center
- **GPS** (1.75" AMOLED): GNSS + IMU + SD logging — CLOSED 2026-09-29 as a read-only data pipe by design (wiki/GPS-LC76G-POSTMORTEM.md)
- **Pod 1 / Pod 2** (1.75" AMOLED): display pods + parachute/rollover safety-deployment (IMU votes, interlocks)
- **Relay/MOS controllers** (ESP32-WROOM-32): relay-4ch-hd, relay-8ch-a/b, mos-4ch-a/b — boost loop staged (GPL-lineage heritage; clean-room rewrite gates v1.0.0, TODO §6.0)
- **openDstream**: desktop-side ESP-NOW↔UART bridge (incl. rAtTrax-BMS DP bridge)
- **BMS** (external): rAtTrax BMS Logger, battery/motor telemetry node

Data flow: MultiDisplay (VR6 ECU) → HC-05 Bluetooth-serial (~5 frames/s) → Left pod (UART RX) → `DATA_BATCH` on CH1 → Center → re-batched `SET_DATA_BATCH` relay to BOTH gauge pods. GPS, BMS, and relay/MOS feed in on the same channel architecture with their own producers.

---

## Architecture

### Build System
- **ESP-IDF** v6.1 (dev snapshot `v6.1-dev-2441-gffb63db38b`) — fleet-wide
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
│   │   ├── opendash_protocol.h  — Message format (frame structure)
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

### ESP-NOW Topology (event-driven — NO PING, NO POLL)
```
                                   ┌───────────────────┐
   LEFT: MD UART → DATA_BATCH ────▶│      CENTER       │──── SET_DATA_BATCH 0x0C ───▶ LEFT / RIGHT
   GPS / BMS push DATA_RESPONSE ──▶│      MASTER       │──── SET_DATA_POINT 0x01 ───▶ POD1 / POD2
   Pods announce STATUS_REPORT ───▶│  4 priority ch.   │──── SET_RELAY 0x08 ────────▶ RELAY / MOS
                                   └───────────────────┘     per-peer quarantine +
                                                             offline = data absence
   MultiDisplay ──HC-05 BT──▶ LEFT pod ──ESP-NOW──▶ CENTER ──re-batch──▶ every consumer
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
| SET_DATA_POINT | 0x01 | Master→Slave | Push single gauge/display value |
| SET_DATA_BATCH | 0x0C | Master→Slave | Fan-out batch (center → pods, `[count][id:2][f32]×N`) |
| DATA_RESPONSE | 0x81 | Slave→Master | Single sensor reading |
| DATA_BATCH | 0x88 | Slave→Master | Batched frame (one MD UART frame = one packet) |
| STATUS_REPORT | 0x82 | Slave→Master | Self-announce on first contact (no PING needed) |
| SYSTEM | 0x07 | Master→Slave | Time sync, reboot (PING subcmd defined but unused for discovery) |
| SET_ALARM | 0x03 | Master→Slave | Warning thresholds |
| SET_RELAY | 0x08 | Master→Slave | Relay/MOS channel state |

39 opcodes total — canonical list in `common/include/opendash_protocol.h`; full spec in `docs/espnow-protocol.md`.

### Data Point ID Ranges (MD/OBD domain split — v0.1.0)
| Range | Source | Description |
|-------|--------|------------|
| 0x0100–0x01FF | ECU/OBD2 domain | Standard PID values (coolant, speed, EGTs, trims, DTC flags…). Note: `MD_RPM` 0x0117 lives here but is **MD-domain** (predates the split) |
| 0x0800–0x08FF | **MD domain** | MultiDisplay-native channels (`MD_LAMBDA`, `MD_BOOST`, `MD_OIL_TEMP/PRESS`, `MD_GEAR`…) — cross-domain feed is structurally impossible via id-based widget binding |
| 0x0200–0x02FF | GPS node | Speed, heading, coordinates, satellites |
| 0x0300–0x03FF | IMU | G-forces, pitch, roll, yaw |
| 0x0400–0x04FF | rAtTrax BMS | Pack V/I, SOC, cell voltages, temp, power |
| 0x0500–0x05FF | System | CPU temp, heap, RSSI, uptime |
| 0x0600–0x06FF | VESC (via BMS) | eRPM, current, duty, temps, fault |
| 0x0620–0x0624 | RPM (via BMS) | 4-wheel RPM + avg speed |

---

## Node-Specific Details

### Center (espnow_master.c)
- Event-driven master — **NO PING, NO POLLING** ("Starting channel-based tasks — NO PING, NO POLL"); nodes self-identify on first contact and are auto-registered into the peer table
- Receives DATA_RESPONSE / DATA_BATCH → channel queues → per-datapoint: SD log + LVGL lock + UI update
- Re-batches and relays SET_DATA_BATCH to BOTH gauge pods (LEFT included — its ECU-domain widgets, e.g. `COOLANT_TEMP` on the WATER page, only ever arrive via that relay)
- Logs all data points to SD card
- Pushes demo data when no real engine data detected (auto-halts on real data)
- 18 logical node slots (`OPENDASH_NODE_COUNT`); per-peer quarantine/backoff; node-offline detection = data absence, not failed polls
- **rAtTrax-BMS node**: registers automatically when BMS self-announces with STATUS_REPORT

### Left/Right (gauge pods)
- Receivers handle BOTH `SET_DATA_POINT` (single) and `SET_DATA_BATCH` (fan-out) — a slave that only dispatches SET_DATA_POINT displays nothing (RIGHT regression, fixed v0.9.0-beta)
- Self-announce with STATUS_REPORT on first contact (no PING required)
- Capture GPS speed for odometer accumulation
- Left pod additionally ingests MultiDisplay UART data and relays it to center as two DATA_BATCH frames per UART frame
- Multi-page gauge system with configurable data point display (LEFT/RIGHT `s_gauge_pages[]` are identical by design)

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
| `DATAFLOW.md` | **As-built data path** — UART → batch → master → fan-out; the fastest way to understand the running system |
| `agent/opendash.agent.md` | This file — canonical playbook (linked from `readme.md` Quick Links) |
| `agent/opendash.instructions.md` | Editor auto-load digest (defers to this file) |
| `docs/architecture.md` | System-level architecture |
| `docs/hardware.md` | Pin maps, board specs |
| `docs/espnow-protocol.md` | Full protocol specification |
| `docs/data-points.md` | Data point ID legend |
| `docs/safety-deployment-test-checklist.md` | **Bench/track verification checklist — parachute/rollover deployment (run before any live-squib use)** |
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
3. Check if the change affects `common/` — if so, consider all node families that consume it (display nodes, relay/MOS, openDstream, external BMS)
4. Make targeted, minimal edits
5. Build with `idf.py build` after every change
6. If modifying ESP-NOW, verify against the protocol docs and test with all active nodes
7. Update `TODO.md` honestly
8. Never touch rAtTrax-BMS files without switching to that project's context
