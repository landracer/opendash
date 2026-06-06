<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OpenDash — Displayable Data Points Legend

## Overview

This document lists every data point that can be displayed on any OpenDash screen.
Each data point has a unique 16-bit ID, a human-readable name, unit, and range.
Screen sections can be configured to show any data point from this list.

## Data Point Categories

### 🔧 Engine / OBD2 (0x0100 – 0x01FF)

| ID | Name | Unit | Range | OBD2 PID | Description |
|---|---|---|---|---|---|
| `0x0100` | RPM | rpm | 0–16,383 | `0x0C` | Engine revolutions per minute |
| `0x0101` | Vehicle Speed | km/h | 0–255 | `0x0D` | Vehicle speed from ECU |
| `0x0102` | Coolant Temp | °C | -40–215 | `0x05` | Engine coolant temperature |
| `0x0103` | Intake Air Temp | °C | -40–215 | `0x0F` | Intake air temperature |
| `0x0104` | Engine Load | % | 0–100 | `0x04` | Calculated engine load |
| `0x0105` | Throttle Position | % | 0–100 | `0x11` | Throttle position |
| `0x0106` | Boost Pressure | kPa | 0–255 | `0x0B` | Intake manifold pressure (MAP) |
| `0x0107` | Oil Temp | °C | -40–215 | `0x5C` | Engine oil temperature |
| `0x0108` | Oil Pressure | kPa | 0–655 | N/A | Oil pressure (external sensor) |
| `0x0109` | Fuel Pressure | kPa | 0–765 | `0x0A` | Fuel system pressure |
| `0x010A` | AFR | ratio | 7–23 | N/A | Air-fuel ratio (wideband O2) |
| `0x010B` | Lambda | ratio | 0.5–1.5 | `0x24` | Lambda value |
| `0x010C` | EGT | °C | 0–1275 | N/A | Exhaust gas temperature |
| `0x010D` | Battery Voltage | V | 0–18 | `0x42` | System battery voltage |
| `0x010E` | Timing Advance | ° | -64–64 | `0x0E` | Ignition timing advance |
| `0x010F` | MAF Rate | g/s | 0–655 | `0x10` | Mass air flow rate |
| `0x0110` | Fuel Level | % | 0–100 | `0x2F` | Fuel tank level |
| `0x0111` | Trans Temp | °C | -40–215 | `0x1F` | Transmission fluid temp |

### 🛰️ GPS / Navigation (0x0200 – 0x02FF)

| ID | Name | Unit | Range | Description |
|---|---|---|---|---|
| `0x0200` | GPS Speed | km/h | 0–500 | Speed from GNSS |
| `0x0201` | GPS Heading | ° | 0–360 | Heading (true north) |
| `0x0202` | Latitude | ° | ±90 | GPS latitude |
| `0x0203` | Longitude | ° | ±180 | GPS longitude |
| `0x0204` | Altitude | m | -500–9000 | GPS altitude |
| `0x0205` | Satellite Count | count | 0–40 | Number of satellites locked |
| `0x0206` | HDOP | ratio | 0–50 | Horizontal dilution of precision |
| `0x0207` | Lap Number | count | 0–999 | Current lap number |
| `0x0208` | Lap Time | ms | 0–600000 | Current lap time |
| `0x0209` | Best Lap Time | ms | 0–600000 | Best lap time this session |
| `0x020A` | Lap Delta | ms | ±60000 | Delta vs. best lap (+/-) |
| `0x020B` | Sector Time | ms | 0–120000 | Current sector time |
| `0x020C` | Predictive Lap | ms | 0–600000 | Predicted lap time |

### 📐 IMU / Motion (0x0300 – 0x03FF)

| ID | Name | Unit | Range | Description |
|---|---|---|---|---|
| `0x0300` | G-Force Lateral | g | ±8 | Side-to-side acceleration |
| `0x0301` | G-Force Longitudinal | g | ±8 | Forward/backward acceleration |
| `0x0302` | G-Force Vertical | g | ±8 | Up/down acceleration |
| `0x0303` | Yaw Rate | °/s | ±2000 | Rotation around vertical axis |
| `0x0304` | Pitch Rate | °/s | ±2000 | Rotation around lateral axis |
| `0x0305` | Roll Rate | °/s | ±2000 | Rotation around longitudinal axis |
| `0x0306` | Pitch Angle | ° | ±90 | Vehicle pitch |
| `0x0307` | Roll Angle | ° | ±180 | Vehicle roll |

### 🔋 Battery / BMS (0x0400 – 0x04FF)

| ID | Name | Unit | Range | Description |
|---|---|---|---|---|
| `0x0400` | Pack Voltage | V | 0–100 | Total battery pack voltage |
| `0x0401` | Pack Current | A | ±500 | Pack current (+ = discharge) |
| `0x0402` | SOC | % | 0–100 | State of charge |
| `0x0403` | Cell V Min | V | 0–5 | Lowest cell voltage |
| `0x0404` | Cell V Max | V | 0–5 | Highest cell voltage |
| `0x0405` | Cell V Delta | mV | 0–500 | Max cell voltage difference |
| `0x0406` | BMS Temp Max | °C | -40–125 | Highest BMS temperature |
| `0x0407` | Pack Power | W | ±50000 | Pack power (V × A) |
| `0x0408` | Energy Used | Wh | 0–99999 | Cumulative energy this session |
| `0x0410`–`0x041F` | Cell V 1–16 | V | 0–5 | Individual cell voltages |

### ⚙️ System (0x0500 – 0x05FF)

| ID | Name | Unit | Range | Description |
|---|---|---|---|---|
| `0x0500` | CPU Temp | °C | -40–125 | ESP32 die temperature |
| `0x0501` | Free Heap | KB | 0–8192 | Available heap memory |
| `0x0502` | WiFi RSSI | dBm | -100–0 | WiFi signal strength |
| `0x0503` | Uptime | s | 0–86400 | Time since boot |
| `0x0504` | SD Card Free | MB | 0–32768 | SD card remaining space |
| `0x0505` | Log Session | count | 0–9999 | Current logging session number |

---

## Screen Section Configuration

Each display is divided into configurable sections. Assign any data point
ID to any section via the configuration system or companion app.

### Center Display (800×480) — Default Layout

```
┌──────────────────────────────────────────────────────────┐
│  [RPM Bar — Full Width Arc]                              │
├──────────────┬───────────────────────┬───────────────────┤
│  Section A   │     Section B         │    Section C      │
│  Coolant °C  │     SPEED (GPS)       │    Boost kPa      │
│              │                       │                   │
├──────────────┼───────────────────────┼───────────────────┤
│  Section D   │     Section E         │    Section F      │
│  Oil Temp    │    Lap Time/Delta     │    AFR            │
│              │                       │                   │
├──────────────┴───────────────────────┴───────────────────┤
│  [Status Bar — Warnings, Alarms, Checklist Status]       │
└──────────────────────────────────────────────────────────┘
```

### Left/Right Gauge (480×480 Round) — Default Layout

```
        ┌──────────────────┐
       ╱    Section A       ╲
      │   (Primary Value)    │
      │   Large numeric      │
      │                      │
      │ ┌──────────────────┐ │
      │ │   Section B      │ │
      │ │ (Secondary Value)│ │
      │ └──────────────────┘ │
       ╲   Arc / Gauge Bar  ╱
        └──────────────────┘
```

### GPS Unit (466×466 Round AMOLED) — Default Layout

```
        ┌──────────────────┐
       ╱    GPS Speed       ╲
      │    (Large numeric)   │
      │                      │
      │ ┌──────────────────┐ │
      │ │   Lap Time       │ │
      │ │   Lap Delta      │ │
      │ │   G-Force Circle │ │
      │ └──────────────────┘ │
       ╲ Sat Count  Heading ╱
        └──────────────────┘
```
