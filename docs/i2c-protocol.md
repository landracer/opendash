# OpenDash — I2C Inter-Node Communication Protocol

## Overview

All OpenDash nodes communicate over a shared I2C bus at **400 kHz** (Fast Mode).
The **Center** display is always the I2C master. All other nodes (Left, Right,
GPS, and external BMS) are slaves.

## Node Addresses

| Node | Address | Description |
|---|---|---|
| Left Gauge | `0x10` | Left-side 2.8" round gauge |
| Right Gauge | `0x11` | Right-side 2.8" round gauge |
| GPS / Telemetry | `0x12` | GPS + IMU unit |
| BMS (rAtTrax) | `0x20` | External BMS logger node |

> Addresses are defined in `common/include/opendash_i2c_protocol.h`

## Message Format

Every I2C message uses a fixed-header format for reliability:

```
┌──────┬──────┬────────┬────────────────┬──────────┐
│ SYNC │ CMD  │ LENGTH │    PAYLOAD     │ CHECKSUM │
│ 0xAA │ 1B   │  1B    │  0–248 bytes   │   1B     │
└──────┴──────┴────────┴────────────────┴──────────┘
```

| Field | Size | Description |
|---|---|---|
| SYNC | 1 byte | Always `0xAA` — marks start of valid message |
| CMD | 1 byte | Command identifier (see command table below) |
| LENGTH | 1 byte | Number of payload bytes (0–248) |
| PAYLOAD | 0–248 bytes | Command-specific data |
| CHECKSUM | 1 byte | XOR of all preceding bytes |

## Command Table

### Master → Slave Commands

| CMD | Name | Payload | Description |
|---|---|---|---|
| `0x01` | `SET_DATA_POINT` | `[dp_id:2][value:4]` | Send a data point value to display |
| `0x02` | `SET_SCREEN_LAYOUT` | `[section:1][dp_id:2]...` | Configure which data points to show |
| `0x03` | `SET_ALARM` | `[dp_id:2][lo:4][hi:4][flags:1]` | Set alarm thresholds |
| `0x04` | `SET_BRIGHTNESS` | `[level:1]` | Set display brightness (0–255) |
| `0x05` | `CHECKLIST_UPDATE` | `[item_id:1][status:1]` | Update checklist item status |
| `0x06` | `REQUEST_DATA` | `[dp_id:2]` | Request a data point from slave |
| `0x07` | `SYSTEM_CMD` | `[subcmd:1][params...]` | System commands (reboot, OTA, etc.) |

### Slave → Master Responses

| CMD | Name | Payload | Description |
|---|---|---|---|
| `0x81` | `DATA_RESPONSE` | `[dp_id:2][value:4][ts:4]` | Response to REQUEST_DATA |
| `0x82` | `STATUS_REPORT` | `[node_id:1][flags:2]` | Node health status |
| `0x83` | `CHECKLIST_STATUS` | `[item_id:1][status:1]` | Checklist item status from this node |
| `0x84` | `ALARM_TRIGGERED` | `[dp_id:2][value:4]` | Alarm triggered notification |
| `0xFF` | `NAK` | `[error_code:1]` | Error / not acknowledged |

## Polling Cycle

The Center (master) runs a polling loop at approximately **50 Hz**:

```
1. Poll GPS unit for position, speed, heading, lap data      (every cycle)
2. Poll GPS unit for IMU data (g-force, orientation)          (every cycle)
3. Send data point updates to Left gauge                      (every cycle)
4. Send data point updates to Right gauge                     (every cycle)
5. Poll BMS for battery data                                  (every 500ms)
6. Exchange checklist status                                  (every 1s)
7. Process alarms and warnings                                (every cycle)
```

## Error Handling

- **NAK responses** — If a slave sends NAK, master retries up to 3 times
- **Timeout** — If no response within 10ms, mark node as offline
- **Checksum failure** — Discard message, request retransmission
- **Node offline** — Display warning indicator, continue with available data
- **Bus recovery** — If bus is stuck, master performs clock recovery sequence

## Data Types

All multi-byte values are transmitted **little-endian**.

| Type | Size | Range | Example |
|---|---|---|---|
| `int16_t` | 2 bytes | -32768 to 32767 | Temperature in 0.1°C |
| `uint16_t` | 2 bytes | 0 to 65535 | RPM, data point IDs |
| `int32_t` | 4 bytes | ±2 billion | GPS coordinates (microdegrees) |
| `float` | 4 bytes | IEEE 754 | Voltage, pressure, AFR |

## Integration with rAtTrax BMS

The BMS node (address `0x20`) provides:

| Data Point | Type | Unit | Description |
|---|---|---|---|
| Pack Voltage | float | V | Total battery pack voltage |
| Pack Current | float | A | Current draw (+ discharge, - charge) |
| SOC | uint8_t | % | State of charge |
| Cell Voltages | float[16] | V | Individual cell voltages |
| Cell Temps | int16_t[8] | 0.1°C | Cell temperature probes |
| BMS Status | uint16_t | flags | Error flags, balance status |
