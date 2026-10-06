---
name: opendash
description: opendash code agent, here to review, write, consult.
#tools: [read, grep, glob, bash] # specify the tools this agent can use. If not set, all enabled tools #are allowed.
---
<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
Deep in-depth knowledge of how opendash works. **Canonical playbook: [`agent/opendash.agent.md`](../../agent/opendash.agent.md) — read it first; this file is the Claude-Code entry point into it.**

## Project Overview

OpenDash is a modular digital dashboard / vehicle-control system for race cars built on ESP-IDF v6.1 + LVGL 9 + ESP-NOW across a fleet of twelve node families (v0.1.0 baseline).

## Key Components (display nodes)

1. **Center Display** (Main Dash, ESP-NOW master) - ESP32-S3-Touch-LCD-4.3 (800×480 IPS)
2. **Left / Right Gauge** - ESP32-S3-LCD-2.8C (480×480 Round)
3. **GPS / Telemetry** + **Pod 1 / Pod 2** (safety deployment) - ESP32-S3-Touch-AMOLED-1.75 (466×466 AMOLED)
4. **Controllers** - relay-4ch-hd, relay-8ch-a/b, mos-4ch-a/b (ESP32-WROOM-32), openDstream bridge, external rAtTrax-BMS

## Architecture

- **Communication**: ESP-NOW only (WiFi peer-to-peer), event-driven — NO polling, NO PING loop; batched `DATA_BATCH (0x88)` / `SET_DATA_BATCH (0x0C)` over 4 priority channels with per-peer quarantine
- **Shared Code**: All units use code from the top-level `common/` directory
- **Nodes**: addressed by MAC, identified logically by `opendash_node_t` (no bus addresses); slaves self-announce, center fans out

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