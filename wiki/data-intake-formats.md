<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OpenDash Data Intake Formats

This document describes the data intake formats and callouts for interfacing with OpenDash. It provides a complete reference for other projects to integrate with OpenDash's data system.

## Overview

OpenDash supports multiple data sources through a unified data point system. Data points are identified by 16-bit IDs and can be provided via UART from Multidisplay, ESP-NOW from other nodes, or I2C from sensors.

## Data Point System

### Data Point ID Structure

All data points are identified by 16-bit IDs in the format:
- **0x0100-0x01FF**: Engine/OBD2 data
- **0x0200-0x02FF**: GPS/Navigation data  
- **0x0300-0x03FF**: IMU/Motion data
- **0x0400-0x04FF**: Battery/BMS data
- **0x0500-0x05FF**: System data
- **0x0600-0x06FF**: VESC data

### Data Point Format

Each data point consists of:
- **ID**: 16-bit unique identifier
- **Value**: 32-bit floating point value
- **Unit**: Human-readable unit (for display purposes)

## Communication Protocols

### 1. UART (Multidisplay Connection)

**Protocol**: SERIALOUT_BINARY mode (default)
**Baud Rate**: 115200
**Frame Size**: 95 bytes
**Frame Rate**: 100 Hz (10ms interval)
**Data Format**: Little-endian

#### Frame Structure
```
┌───┬───┬─────────────────────────────────────────────────────────────────────┬───┐
│STX│TAG│                           93-byte payload                         │ETX│
│0x02│0x5F│                        (93 bytes)                                 │0x03│
└───┴───┴─────────────────────────────────────────────────────────────────────┴───┘
```

#### Data Point IDs (UART)
- **0x0100-0x011B**: Engine data points (RPM, speed, temp, pressure, etc.)
- **0x0112-0x011B**: EGT channels (8 channels)
- **0x011C-0x01FF**: Reserved for future engine data

### 2. ESP-NOW (Inter-Node Communication)

**Protocol**: Custom I2C-style messages over ESP-NOW
**Data Format**: Binary message with command and payload

#### Message Structure
```
┌─────────────┬─────────────────────────────────────────────────────────────┐
│ Command     │ Payload                                                     │
│ 1 byte      │ Variable length                                             │
└─────────────┴─────────────────────────────────────────────────────────────┘
```

#### Command Types
- **0x01**: SET_DATA_POINT - Set a single data point value
- **0x02**: WARNING - Send a warning message
- **0x03**: SYSTEM - System status update
- **0x04**: LAP_DATA - Lap timing data
- **0x05**: CONFIG - Configuration update

#### SET_DATA_POINT Message Format
```
┌─────────────┬─────────────┬─────────────────────────────────────────────┐
│ Command     │ Data Point ID│ Value                                       │
│ 0x01        │ 2 bytes     │ 4 bytes (float)                           │
└─────────────┴─────────────┴─────────────────────────────────────────────┘
```

### 3. I2C (Sensor Integration)

**Protocol**: Standard I2C
**Address Range**: 0x08-0x77 (standard I2C addresses)
**Data Format**: Sensor-specific binary data

## Data Point Reference

### Engine/OBD2 Data (0x0100-0x01FF)
| ID | Name | Unit | Range | Description |
|----|------|------|-------|-------------|
| 0x0100 | RPM | rpm | 0-16,383 | Engine revolutions per minute |
| 0x0101 | Vehicle Speed | km/h | 0-255 | Vehicle speed from ECU |
| 0x0102 | Coolant Temp | °C | -40-215 | Engine coolant temperature |
| 0x0103 | Intake Air Temp | °C | -40-215 | Intake air temperature |
| 0x0104 | Engine Load | % | 0-100 | Calculated engine load |
| 0x0105 | Throttle Position | % | 0-100 | Throttle position |
| 0x0106 | Boost Pressure | kPa | 0-255 | Intake manifold pressure (MAP) |
| 0x0107 | Oil Temp | °C | -40-215 | Engine oil temperature |
| 0x0108 | Oil Pressure | kPa | 0-655 | Oil pressure (external sensor) |
| 0x0109 | Fuel Pressure | kPa | 0-765 | Fuel system pressure |
| 0x010A | AFR | ratio | 7-23 | Air-fuel ratio (wideband O2) |
| 0x010B | Lambda | ratio | 0.5-1.5 | Lambda value |
| 0x010C | EGT | °C | 0-1275 | Exhaust gas temperature |
| 0x010D | Battery Voltage | V | 0-18 | System battery voltage |
| 0x010E | Timing Advance | ° | -64-64 | Ignition timing advance |
| 0x010F | MAF Rate | g/s | 0-655 | Mass air flow rate |
| 0x0110 | Fuel Level | % | 0-100 | Fuel tank level |
| 0x0111 | Trans Temp | °C | -40-215 | Transmission fluid temp |

### GPS/Navigation Data (0x0200-0x02FF)
| ID | Name | Unit | Range | Description |
|----|------|------|-------|-------------|
| 0x0200 | GPS Speed | km/h | 0-500 | Speed from GNSS |
| 0x0201 | GPS Heading | ° | 0-360 | Heading (true north) |
| 0x0202 | Latitude | ° | ±90 | GPS latitude |
| 0x0203 | Longitude | ° | ±180 | GPS longitude |
| 0x0204 | Altitude | m | -500-9000 | GPS altitude |
| 0x0205 | Satellite Count | count | 0-40 | Number of satellites locked |
| 0x0206 | HDOP | ratio | 0-50 | Horizontal dilution of precision |
| 0x0207 | Lap Number | count | 0-999 | Current lap number |
| 0x0208 | Lap Time | ms | 0-600000 | Current lap time |
| 0x0209 | Best Lap Time | ms | 0-600000 | Best lap time this session |
| 0x020A | Lap Delta | ms | ±60000 | Delta vs. best lap (+/-) |
| 0x020B | Sector Time | ms | 0-120000 | Current sector time |
| 0x020C | Predictive Lap | ms | 0-600000 | Predicted lap time |

### IMU/Motion Data (0x0300-0x03FF)
| ID | Name | Unit | Range | Description |
|----|------|------|-------|-------------|
| 0x0300 | G-Force Lateral | g | ±8 | Side-to-side acceleration |
| 0x0301 | G-Force Longitudinal | g | ±8 | Forward/backward acceleration |
| 0x0302 | G-Force Vertical | g | ±8 | Up/down acceleration |
| 0x0303 | Yaw Rate | °/s | ±2000 | Rotation around vertical axis |
| 0x0304 | Pitch Rate | °/s | ±2000 | Rotation around lateral axis |
| 0x0305 | Roll Rate | °/s | ±2000 | Rotation around longitudinal axis |
| 0x0306 | Pitch Angle | ° | ±90 | Vehicle pitch |
| 0x0307 | Roll Angle | ° | ±180 | Vehicle roll |

### Battery/BMS Data (0x0400-0x04FF)
| ID | Name | Unit | Range | Description |
|----|------|------|-------|-------------|
| 0x0400 | Pack Voltage | V | 0-100 | Total battery pack voltage |
| 0x0401 | Pack Current | A | ±500 | Pack current (+ = discharge) |
| 0x0402 | SOC | % | 0-100 | State of charge |
| 0x0403 | Cell V Min | V | 0-5 | Lowest cell voltage |
| 0x0404 | Cell V Max | V | 0-5 | Highest cell voltage |
| 0x0405 | Cell V Delta | mV | 0-500 | Max cell voltage difference |
| 0x0406 | BMS Temp Max | °C | -40-125 | Highest BMS temperature |
| 0x0407 | Pack Power | W | ±50000 | Pack power (V × A) |
| 0x0408 | Energy Used | Wh | 0-99999 | Cumulative energy this session |
| 0x0409 | SOH | % | 0-100 | State of health |
| 0x040A | BMS IC Temp | °C | -40-125 | BMS IC temperature |
| 0x040B | Balance Status | - | - | Cell balancing status |
| 0x040C | Charging State | - | - | Charging/discharging status |
| 0x040D | Energy Charged | Wh | 0-99999 | Energy charged this session |
| 0x0410-0x0415 | Cell V 1-6 | V | 0-5 | Individual cell voltages |

### System Data (0x0500-0x05FF)
| ID | Name | Unit | Range | Description |
|----|------|------|-------|-------------|
| 0x0500 | CPU Temp | °C | -40-125 | ESP32 die temperature |
| 0x0501 | Free Heap | KB | 0-8192 | Available heap memory |
| 0x0502 | WiFi RSSI | dBm | -100-0 | WiFi signal strength |
| 0x0503 | Uptime | s | 0-86400 | Time since boot |
| 0x0504 | SD Card Free | MB | 0-32768 | SD card remaining space |
| 0x0505 | Log Session | count | 0-9999 | Current logging session number |

### VESC Data (0x0600-0x06FF)
| ID | Name | Unit | Range | Description |
|----|------|------|-------|-------------|
| 0x0600 | VESC eRPM | rpm | 0-16,383 | Motor revolutions per minute |
| 0x0601 | VESC Current | A | ±500 | Motor current |
| 0x0602 | VESC Duty | % | 0-100 | Motor duty cycle |
| 0x0603 | VESC Ah | Ah | 0-1000 | Ah consumed |
| 0x0604 | VESC Ah Charged | Ah | 0-1000 | Ah charged |
| 0x0605 | VESC Wh | Wh | 0-100000 | Wh consumed |
| 0x0606 | VESC Wh Charged | Wh | 0-100000 | Wh charged |
| 0x0607 | VESC FET Temp | °C | -40-125 | FET temperature |
| 0x0608 | VESC Motor Temp | °C | -40-125 | Motor temperature |
| 0x0609 | VESC Current In | A | ±500 | Input current |
| 0x060A | VESC PID Position | - | - | PID position |
| 0x060B | VESC Tacho | - | - | Tacho value |
| 0x060C | VESC V In | V | 0-100 | Input voltage |
| 0x060D | VESC ADC1 | - | - | ADC channel 1 |
| 0x060E | VESC ADC2 | - | - | ADC channel 2 |
| 0x060F | VESC ADC3 | - | - | ADC channel 3 |
| 0x0610 | VESC PPM | - | - | PPM value |
| 0x0611 | VESC RPM | rpm | 0-16,383 | RPM |
| 0x0612 | VESC Power In | W | ±50000 | Input power |
| 0x0613 | VESC Power Motor | W | ±50000 | Motor power |
| 0x0614 | VESC Fault | - | - | Fault code |
| 0x0620 | Wheel RPM FL | rpm | 0-16,383 | Front left wheel RPM |
| 0x0621 | Wheel RPM FR | rpm | 0-16,383 | Front right wheel RPM |
| 0x0622 | Wheel RPM RL | rpm | 0-16,383 | Rear left wheel RPM |
| 0x0623 | Wheel RPM RR | rpm | 0-16,383 | Rear right wheel RPM |
| 0x0624 | Wheel Speed Avg | km/h | 0-500 | Average wheel speed |

## Integration Guidelines

### For UART Integration (Multidisplay)
1. Configure HC-05 Bluetooth module to connect to multidisplay's HC-06 slave
2. Connect UART RX to GPIO20 (J9 DP pin)
3. Use `opendash_uart.h` for initialization and data parsing
4. Parse binary frames at 100 Hz rate
5. Extract data points from payload using little-endian field extraction

### For ESP-NOW Integration (Other Nodes)
1. Initialize ESP-NOW as a peer node
2. Use `opendash_espnow.h` for communication functions
3. Send data points using `OPENDASH_CMD_SET_DATA_POINT` command
4. Format data as `[command:1][dp_id:2][value:4]`
5. Broadcast at appropriate rates (5-10 Hz for most data)

### For I2C Integration (Sensors)
1. Connect sensors to I2C bus (GPIO15/16 for left/right, GPIO7/8 for GPS)
2. Use standard I2C addressing
3. Implement sensor-specific parsing functions
4. Convert sensor data to OpenDash data point format
5. Send data via ESP-NOW or UART to OpenDash

## Error Handling

### UART Errors
- **Bad STX/ETX**: Frame synchronization lost, parser re-scans for STX
- **Bad ETX**: 15-20% of frames affected, not timing-related (electrical noise)
- **Frame timeout**: Connection lost, reconnect attempt initiated

### ESP-NOW Errors
- **Message drops**: Queue overflow, retry mechanism in place
- **Peer loss**: Node disconnected, automatic reconnection
- **Invalid data**: Malformed data point, ignored with warning

## Best Practices

1. **Data Rate Management**: Most data points are sent at 5-10 Hz to reduce network load
2. **Data Validation**: Always validate data ranges and plausibility
3. **Error Recovery**: Implement graceful degradation when data sources are lost
4. **Memory Efficiency**: Use fixed-size buffers and avoid dynamic allocation
5. **Timing Consistency**: Maintain consistent update rates across all data sources