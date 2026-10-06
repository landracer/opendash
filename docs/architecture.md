<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OpenDash — System Architecture

## Overview

OpenDash is a modular racecar dashboard and vehicle-control system. Twelve node
families (displays, gauge pods, relay/MOS controllers, a GPS/telemetry unit and
a PC bridge) exchange real-time engine, GPS, IMU, and battery data over an
ESP-NOW mesh, with CENTER as master.

## Node Roles

```
┌─────────────────────────────────────────────────────────────────────┐
│                        RACECAR DASH LAYOUT                         │
│                                                                     │
│  ┌──────────────┐   ┌────────────────────┐   ┌──────────────┐      │
│  │  LEFT GAUGE  │   │   CENTER DISPLAY   │   │ RIGHT GAUGE  │      │
│  │   (2.8" Rnd) │   │    (4.3" Wide)     │   │  (2.8" Rnd)  │      │
│  │  480×480 px  │   │   800×480 px       │   │  480×480 px  │      │
│  │              │   │                    │   │              │      │
│  │  ESP32-S3    │   │  ESP32-S3          │   │  ESP32-S3    │      │
│  │  LCD-2.8C    │   │  Touch-LCD-4.3     │   │  LCD-2.8C    │      │
│  └──────┬───────┘   └─────────┬──────────┘   └──────┬───────┘      │
│         │                     │                      │              │
│         └─────────────────────┼──────────────────────┘              │
│                               │  ESP-NOW (2.4 GHz radio,             │
│                               │  no wires between nodes)              │
│                    ┌──────────┴──────────┐                          │
│                    │    GPS / TELEMETRY   │                          │
│                    │   (1.75" AMOLED Rnd) │                          │
│                    │    466×466 px        │                          │
│                    │  GPS + IMU + Gyro    │                          │
│                    │  ESP32-S3-AMOLED-1.75│                          │
│                    └─────────────────────┘                          │
└─────────────────────────────────────────────────────────────────────┘
```

## Communication Architecture

## Communication Architecture

All nodes communicate over **ESP-NOW** (Wi-Fi peer-to-peer radio). There is **no
wired inter-node bus** — the historical I2C inter-node design was abandoned
(see Important Note below). The **Center** display is the logical master; every
other node is a slave that pushes data on change.

Nodes are identified by a **logical node ID** carried in every frame and mapped
to a MAC via the persistent peer table — the ID is *not* a bus address.

| Node | `opendash_node_t` | ID | Role | Description |
|---|---|---|---|---|
| Center | `OPENDASH_NODE_CENTER` | 0 | Master | Routes frames to the owning channel task, aggregates data, primary display |
| Left | `OPENDASH_NODE_LEFT` | 1 | Slave | Renders gauge data pushed by Center |
| Right | `OPENDASH_NODE_RIGHT` | 2 | Slave | Renders gauge data pushed by Center |
| GPS | `OPENDASH_NODE_GPS` | 3 | Slave | **⛔ FROZEN 2026-09-29** — LC76G module is hard-fixed at 1 Hz with no usable command channel; unusable for racing telemetry. See `wiki/GPS-LC76G-POSTMORTEM.md`. No further development. |
| BMS (ext.) | `OPENDASH_NODE_BMS` | 4 | Slave | External BMS node (rAtTrax integration) |
| POD1–POD8 | `OPENDASH_NODE_POD1..8` | 5–12 | Slave | Expansion gauge pods |
| Relay / MOS | `OPENDASH_NODE_RELAY_4CH` … `MOS_4CH_B` | 13–17 | Slave | Silent-slave relay + MOS FET controllers |

### ⚠️ Important Note
The original design intended to use I2C for inter-node communication, but due to
hardware limitations and GPIO conflicts, the system was re-implemented to use
ESP-NOW (Wi-Fi peer-to-peer). This provides zero-wire communication with no GPIO
conflicts and better reliability.

I2C remains only as a **local peripheral** bus (touch controller, IMU, GNSS
receiver). Those pins are fixed in silicon and are unrelated to inter-node
traffic. The I2C code that remains in the tree initializes those on-board
peripheral controllers (called from each node's own `display_init`/handler
bring-up) — it does not carry node traffic.

## Data Flow

```
                    ┌──────────────┐
                    │   OBD2/CAN   │
                    │  (External)  │
                    └──────┬───────┘
                           │ CAN / UART
                           ▼
┌──────────┐    ESP-NOW    ┌──────────┐    ESP-NOW    ┌──────────┐
│   LEFT   │◄─────────────►│  CENTER  │◄─────────────►│  RIGHT   │
│  Gauge   │               │  (Master)│               │  Gauge   │
└──────────┘               └────┬─────┘               └──────────┘
                                │ ESP-NOW
                                ▼
                    ┌──────────────┐
                    │  GPS / IMU   │
                    │  (Slave)     │
                    └──────┬───────┘
                           │ ESP-NOW
                           ▼
                    ┌──────────────┐
                    │  BMS Node    │
                    │  (External)  │
                    └──────────────┘
```

### Data Flow Steps

1. **Center** unit acts as the ESP-NOW master and system coordinator
2. **GPS unit** (⛔ frozen — read-only 1 Hz data pipe) reads GNSS and IMU
   data, stores latest readings
3. **GPS unit** pushes position, speed, and g-force (no polling — see
   `docs/espnow-protocol.md` §4)
4. **Engine data arrives via LEFT**: the LEFT pod ingests the MultiDisplay
   serial frame and forwards it as two `DATA_BATCH` frames (MD-domain +
   OBD-domain ids) to Center. Direct CAN (ECU/VESC/OBD2-ELM327) is
   *planned*, not built — see `wiki/vesc-integration.md` and `DATAFLOW.md`
5. **Center** distributes relevant data to Left and Right gauge pods
   (re-batched as `SET_DATA_BATCH`)
6. **Left/Right** render their configured data points
7. **BMS node** pushes battery data on change
8. **SD card logging** happens on the Center unit (primary) and GPS unit (backup)

## Software Architecture (Per Node)

Each node follows the same layered architecture:

```
┌─────────────────────────────────────┐
│           Application Layer          │
│  (UI Manager, Screen Logic, Config)  │
├─────────────────────────────────────┤
│           Service Layer              │
│  (Data Model, Checklist, Alarms)     │
├─────────────────────────────────────┤
│          Communication Layer         │
│ (ESP-NOW transport, channel router,  │
│   node_health, protocol codec, OBD2) │
├─────────────────────────────────────┤
│           Driver Layer               │
│  (Display, Touch, GNSS, IMU, SD,     │
│   RTC — local peripheral I2C only)   │
├─────────────────────────────────────┤
│           ESP-IDF / FreeRTOS         │
│  (Tasks, Timers, GPIO, SPI, I2C      │
│   peripheral controllers)            │
└─────────────────────────────────────┘
```

## FreeRTOS Task Structure

Values below are read from the code, not planned. Priorities are FreeRTOS
priority numbers (higher = more urgent).

### Center (master) — see `center/main/espnow_master.c`

| Task | Priority | Core | Description |
|---|---|---|---|
| `ch_control` | 6 | 0 | Control channel — commands, highest priority ("commands must not wait") |
| `ch_critical` | 5 | 0 | Critical channel (GPS/BMS/engine class data) |
| `espnow_dispatch` | 4 | 0 | Routes raw ESP-NOW frames into the per-channel queues |
| `ch_medium` | 4 | 0 | Medium channel (pod displays, relay feedback) |
| `ch_low` | 3 | 1 | Low channel (diagnostics, config) — off-cored to keep core 0 responsive |
| `ota_serial_cmd_task` | — | — | UART OTA ingest (`od-serial-ota-req`) |

A health-evaluation timer runs at 2 Hz (500 ms); `node_health_evaluate()` checks
internally whether the window has elapsed.

### Gauge pods (left / right / POD1–POD8)

| Task | Priority | Core | Description |
|---|---|---|---|
| `ui_task` | 5 | 1 | LVGL rendering loop (core 1 for smooth UI) |
| `touch_rd` | 3 | 0 | Touch controller polling |
| `boot_btn` | 2 | 0 | Boot/GPIO0 button reader |

### GPS unit (frozen)

| Task | Priority | Core | Description |
|---|---|---|---|
| `gps_task` | 8 | 0 | GNSS read/parse — above touch/IMU/UI so GPS is never starved |
| `imu_task` | 5 | 0 | QMI8658 motion sampling |
| `gps_broadcast` | 4 | 0 | Reads sensors + handles inbound ESP-NOW messages |

The parachute/squib actuator task does **not** run on the GPS unit — actuation
lives on the MOS-4CH nodes (`mos-4ch-a/b`, which host
`opendash_parachute_actuator.c`); pod1/pod2 only contribute IMU votes.

## Configuration System

Display configuration is stored in NVS (Non-Volatile Storage) on each node.
This allows persistent settings that survive power cycles:

- **Screen layouts** — Which data points appear in which screen section
- **Alarm thresholds** — Warning/critical limits for each data point
- **Checklist items** — Pre-flight checklist entries
- **Network settings** — WiFi SSID/password, BLE device name
- **Display preferences** — Brightness, theme, units (metric/imperial)
