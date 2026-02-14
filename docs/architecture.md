# OpenDash — System Architecture

## Overview

OpenDash is a three-node racecar dashboard system. Each node is an ESP32-S3 device
running ESP-IDF v5.3 with LVGL for display rendering. The nodes communicate over a
shared I2C bus, with each node having a unique address.

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
│                               │  I2C Bus                            │
│                    ┌──────────┴──────────┐                          │
│                    │    GPS / TELEMETRY   │                          │
│                    │   (1.75" AMOLED Rnd) │                          │
│                    │    466×466 px        │                          │
│                    │  GPS + IMU + Gyro    │                          │
│                    │  ESP32-S3-AMOLED-1.75│                          │
│                    └─────────────────────┘                          │
└─────────────────────────────────────────────────────────────────────┘
```

## I2C Bus Architecture

All four devices connect to a shared I2C bus. The **Center** display acts as the
I2C master, with all other nodes acting as slaves.

| Node | I2C Address | Role | Description |
|---|---|---|---|
| Center | Master | I2C Master | Polls slaves, aggregates data, primary display |
| Left | `0x10` | I2C Slave | Receives display data from master |
| Right | `0x11` | I2C Slave | Receives display data from master |
| GPS | `0x12` | I2C Slave | Provides GPS/IMU data, receives display commands |
| BMS (ext.) | `0x20` | I2C Slave | External BMS node (rAtTrax integration) |

## Data Flow

```
                    ┌──────────────┐
                    │   OBD2/CAN   │
                    │  (External)  │
                    └──────┬───────┘
                           │ CAN / UART
                           ▼
┌──────────┐    I2C    ┌──────────┐    I2C    ┌──────────┐
│   LEFT   │◄─────────►│  CENTER  │◄─────────►│  RIGHT   │
│  Gauge   │           │  (Master)│           │  Gauge   │
└──────────┘           └────┬─────┘           └──────────┘
                            │ I2C
                            ▼
                    ┌──────────────┐
                    │  GPS / IMU   │
                    │  (Slave)     │
                    └──────┬───────┘
                           │ I2C
                           ▼
                    ┌──────────────┐
                    │  BMS Node    │
                    │  (External)  │
                    └──────────────┘
```

### Data Flow Steps

1. **Center** unit acts as the I2C master and system coordinator
2. **GPS unit** continuously reads GNSS and IMU data, stores latest readings
3. **Center** polls GPS unit for position, speed, g-force data
4. **Center** reads OBD2/CAN data directly (onboard CAN transceiver)
5. **Center** distributes relevant data to Left and Right gauge pods
6. **Left/Right** render their configured data points
7. **BMS node** provides battery data when polled
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
│  (I2C Protocol, WiFi/BLE, OBD2)     │
├─────────────────────────────────────┤
│           Driver Layer               │
│  (Display, Touch, GPS, IMU, SD)      │
├─────────────────────────────────────┤
│           ESP-IDF / FreeRTOS         │
│  (Tasks, Timers, GPIO, SPI, I2C)     │
└─────────────────────────────────────┘
```

## FreeRTOS Task Structure

Each node runs the following tasks:

| Task | Priority | Core | Description |
|---|---|---|---|
| `ui_task` | 5 | 1 | LVGL rendering loop (runs on core 1 for smooth UI) |
| `comms_task` | 4 | 0 | I2C communication (master polling or slave responses) |
| `data_task` | 3 | 0 | Data processing, alarm checking, logging |
| `wifi_ble_task` | 2 | 0 | WiFi/BLE management (only when active) |

### GPS Unit Additional Tasks

| Task | Priority | Core | Description |
|---|---|---|---|
| `gps_task` | 5 | 0 | GNSS data reading and parsing |
| `imu_task` | 5 | 0 | IMU data reading (accelerometer + gyro) |
| `parachute_task` | 6 | 0 | Safety monitor — highest priority for immediate response |

## Configuration System

Display configuration is stored in NVS (Non-Volatile Storage) on each node.
This allows persistent settings that survive power cycles:

- **Screen layouts** — Which data points appear in which screen section
- **Alarm thresholds** — Warning/critical limits for each data point
- **Checklist items** — Pre-flight checklist entries
- **Network settings** — WiFi SSID/password, BLE device name
- **Display preferences** — Brightness, theme, units (metric/imperial)
