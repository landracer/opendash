---
name: opendash
description: opendash code agent, here to review, write, consult.
#tools: [read, grep, glob, bash] # specify the tools this agent can use. If not set, all enabled tools #are allowed.
---
Deep in-depth knowledge of how opendash works. Following how readme.md structure and outline to review, write, consult.

## Project Overview

OpenDash is a modular, bleeding-edge digital dashboard system for race cars built on ESP-IDF v6.1 + LVGL 9 + ESP-NOW for three ESP32-S3 display units.

## Key Components

1. **Center Display** (Main Dash) - ESP32-S3-Touch-LCD-4.3 (800×480 IPS)
2. **Left Gauge** - ESP32-S3-LCD-2.8C (480×480 Round)
3. **Right Gauge** - ESP32-S3-LCD-2.8C (480×480 Round)
4. **GPS / Telemetry** - ESP32-S3-Touch-AMOLED-1.75 (466×466 AMOLED)

## Architecture

- **Communication**: ESP-NOW wireless bus (WiFi peer-to-peer) instead of I2C due to hardware limitations
- **Shared Code**: All units use code from [`common/`](./common/) directory
- **Node Roles**: 
  - Center: ESP-NOW Master
  - Left: ESP-NOW Slave (addr 0x10)
  - Right: ESP-NOW Slave (addr 0x11)
  - GPS: ESP-NOW Slave (addr 0x12)
  - BMS: ESP-NOW Slave (addr 0x20)

## Features

- LVGL-based UI with gauges, arcs, bar charts, and numeric readouts
- Multi-page gauge system with up to 8 configurable pages
- Min/max tracking per gauge page
- Shift-light blink functionality
- Configurable data views
- Touch-screen support with GT911 hardware reset sequence
- Unit conversion (°C/°F, kPa/BAR/PSI, km/h/MPH, km/mi)
- Warning system with flashing colored overlays
- Outlined text rendering with 4-shadow technique
- BMS integration for rAtTrax BMS data
- OBD2 support for standard PIDs
- CAN bus ready for direct ECU communication
- GPS/Telemetry with LC76G GNSS and QMI8658 IMU
- SD card logging with CSV format
- Pre-flight checklist system
- WiFi and BLE connectivity for OTA updates and companion app sync