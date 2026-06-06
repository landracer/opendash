<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OpenDash — Features & Sensor Capabilities

> Complete matrix of sensor data available in OpenDash and the source systems
> that provide it. Updated March 2026.

---

## Sensor Data Sources

OpenDash aggregates data from multiple sources into a unified display.
Each source connects to a different ESP32-S3 node.

| Source | Connection | Node | Status |
|---|---|---|---|
| **MultiDisplay (MD)** | HC-05 Bluetooth → UART1 @ 115200 | Left gauge pod | **LIVE** |
| **GPS (LC76G)** | I2C CASIC @ 100 kHz | GPS/Telemetry unit | **Driver ready** |
| **IMU (QMI8658)** | I2C @ 0x6B | GPS/Telemetry unit | Planned |
| **OBD2 / ELM327** | CAN bus or via MD serial | Center display | Planned |
| **VESC ESC** | CAN @ 500 kbps | rAtTrax BMS Logger | Planned |
| **rAtTrax BMS** | ESP-NOW from BMS Logger | Center display | Planned |
| **Demo generator** | Internal software | Center display | **Active (auto-halts on real data)** |

---

## MultiDisplay Sensor Matrix

MultiDisplay is the primary external sensor package. It runs on a Seeeduino
Mega (ATmega1280/2560) and streams all data over Bluetooth at ~100 Hz in a
95-byte binary frame.

### Currently Received by OpenDash (LIVE)

| Sensor | Data Point ID | Unit | Scaling | MD Byte Offset | Notes |
|---|---|---|---|---|---|
| Engine RPM | `DP_RPM` (0x0100) | RPM | Raw uint16 | 1-2 | Via tach signal |
| Boost / MAP | `DP_BOOST_PRESSURE` (0x0106) | kPa | ÷100 | 3-4 | int16 ×100 in frame |
| Throttle Position | `DP_THROTTLE_POS` (0x0105) | % | Raw uint16 | 5-6 | 0-100% |
| Lambda / O2 | `DP_LAMBDA` (0x010B) | ratio | ÷100 | 7-8 | Narrow or wideband |
| Mass Air Flow (LMM) | `DP_MAF_RATE` (0x010F) | g/s | ÷100 | 9-10 | |
| Case Temperature | — | °C | ÷100 | 11-12 | MD board internal temp |
| Battery Voltage | `DP_BATTERY_VOLTAGE` (0x010D) | V | ÷100 | 13-14 | int16 ×100 |
| EGT 1 | `DP_EGT1` (0x0112) | °C | Raw int16 | 15-16 | Type K thermocouple |
| EGT 2 | `DP_EGT2` (0x0113) | °C | Raw int16 | 17-18 | Type K thermocouple |
| EGT 3 | `DP_EGT3` (0x0114) | °C | Raw int16 | 19-20 | Type K thermocouple |
| EGT 4 | `DP_EGT4` (0x0115) | °C | Raw int16 | 21-22 | Type K thermocouple |
| EGT 5 | `DP_EGT5` (0x0118) | °C | Raw int16 | 23-24 | Type K thermocouple |
| EGT 6 | `DP_EGT6` (0x0119) | °C | Raw int16 | 25-26 | Type K thermocouple |
| EGT 7 | `DP_EGT7` (0x011A) | °C | Raw int16 | 27-28 | Type K thermocouple |
| EGT 8 | `DP_EGT8` (0x011B) | °C | Raw int16 | 29-30 | Type K thermocouple |
| VDO Pressure 1 | `DP_OIL_PRESSURE` (0x0108) | kPa | ÷10 | 31-32 | Oil pressure |
| VDO Pressure 2 | — | kPa | ÷10 | 33-34 | Fuel pressure |
| VDO Pressure 3 | — | kPa | ÷10 | 35-36 | Brake pressure |
| VDO Temperature 1 | `DP_OIL_TEMP` (0x0107) | °C | ÷10 | 37-38 | Oil temp |
| VDO Temperature 2 | — | °C | ÷10 | 39-40 | Coolant / trans temp |
| VDO Temperature 3 | — | °C | ÷10 | 41-42 | Aux temp |
| Vehicle Speed | `DP_VEHICLE_SPEED` (0x0101) | km/h | ÷100 | 43-44 | From ECU or GPS |
| Gear Position | — | gear# | Raw uint8 | 45 | Computed from RPM+speed |
| N75 Duty Cycle | — | % | Raw uint8 | 46 | Boost solenoid PWM |
| Requested Boost | — | kPa | ÷100 | 47-48 | PID target |
| EFR Turbo Speed | — | RPM | Raw uint32 | 49-52 | BorgWarner EFR sensor |
| Knock Sensor | — | raw | Raw uint16 | 53-54 | |
| *(VR6 padding)* | — | — | — | 55-89 | 35 bytes zeros (non-VR6 builds) |

> **Byte offsets are relative to the payload** (after TAG byte).
> See `UART_CONNECTION.md` and `SERIAL_PROTOCOL.md` for authoritative byte map.

### Derived Values (Computed by OpenDash)

| Value | Data Point ID | Source | Algorithm |
|---|---|---|---|
| Max EGT | `DP_EGT` (0x010C) | EGT 1-8 | `max(egt[0..7])` |
| O2/Lambda duplicate | `DP_O2_LAMBDA` (0x0116) | Lambda | Direct copy |
| MD RPM (secondary) | `DP_MD_RPM` (0x0117) | RPM | Direct copy for dual-source tracking |

---

## GPS Sensor Matrix

Provided by the LC76G GNSS module on the GPS/Telemetry display unit.

| Sensor | Data Point ID | Unit | Source | Status |
|---|---|---|---|---|
| GPS Speed | `DP_GPS_SPEED` (0x0200) | km/h | RMC sentence | Driver ready |
| Heading | `DP_GPS_HEADING` (0x0201) | degrees | RMC sentence | Driver ready |
| Latitude | `DP_LATITUDE` (0x0202) | degrees | GGA sentence | Driver ready |
| Longitude | `DP_LONGITUDE` (0x0203) | degrees | GGA sentence | Driver ready |
| Altitude | `DP_ALTITUDE` (0x0204) | meters | GGA sentence | Driver ready |
| Satellite Count | `DP_SAT_COUNT` (0x0205) | count | GGA sentence | Driver ready |
| HDOP | `DP_HDOP` (0x0206) | ratio | GGA sentence | Driver ready |
| GPS Fix | `DP_GPS_FIX` (0x020D) | 0/1 | GGA sentence | Driver ready |

### Planned GPS Features

| Feature | Data Point ID | Status |
|---|---|---|
| Lap Number | `DP_LAP_NUMBER` (0x0207) | Planned (§10.5) |
| Lap Time | `DP_LAP_TIME` (0x0208) | Planned |
| Best Lap Time | `DP_BEST_LAP_TIME` (0x0209) | Planned |
| Lap Delta | `DP_LAP_DELTA` (0x020A) | Planned |
| Sector Time | `DP_SECTOR_TIME` (0x020B) | Planned |
| Predictive Lap | `DP_PREDICTIVE_LAP` (0x020C) | Planned |

---

## IMU Sensor Matrix (Planned)

QMI8658 6-axis IMU on the GPS/Telemetry display unit.

| Sensor | Data Point ID | Unit | Status |
|---|---|---|---|
| Lateral G-Force | `DP_GFORCE_LAT` (0x0300) | G | Planned |
| Longitudinal G-Force | `DP_GFORCE_LONG` (0x0301) | G | Planned |
| Vertical G-Force | `DP_GFORCE_VERT` (0x0302) | G | Planned |
| Yaw Rate | `DP_YAW_RATE` (0x0303) | °/s | Planned |
| Pitch Rate | `DP_PITCH_RATE` (0x0304) | °/s | Planned |
| Roll Rate | `DP_ROLL_RATE` (0x0305) | °/s | Planned |
| Pitch Angle | `DP_PITCH_ANGLE` (0x0306) | ° | Planned |
| Roll Angle | `DP_ROLL_ANGLE` (0x0307) | ° | Planned |

---

## rAtTrax BMS Sensor Matrix (Planned)

ESP-NOW integration from the rAtTrax BMS Logger (ESP32 + BQ76952).

| Sensor | Data Point ID | Unit | Status |
|---|---|---|---|
| Pack Voltage | `DP_PACK_VOLTAGE` (0x0400) | V | Planned |
| Pack Current | `DP_PACK_CURRENT` (0x0401) | A | Planned |
| State of Charge | `DP_SOC` (0x0402) | % | Planned |
| Cell V Min | `DP_CELL_V_MIN` (0x0403) | V | Planned |
| Cell V Max | `DP_CELL_V_MAX` (0x0404) | V | Planned |
| Cell V Delta | `DP_CELL_V_DELTA` (0x0405) | mV | Planned |
| BMS Temp Max | `DP_BMS_TEMP_MAX` (0x0406) | °C | Planned |
| Pack Power | `DP_PACK_POWER` (0x0407) | W | Planned |
| Energy Used | `DP_ENERGY_USED` (0x0408) | Wh | Planned |
| Individual Cells | `DP_CELL_V_BASE+n` (0x0410+) | V | Planned (up to 16 cells) |

---

## System Sensors

| Sensor | Data Point ID | Unit | Source |
|---|---|---|---|
| CPU Temperature | `DP_CPU_TEMP` (0x0500) | °C | ESP32-S3 internal |
| Free Heap | `DP_FREE_HEAP` (0x0501) | KB | ESP32-S3 |
| WiFi RSSI | `DP_WIFI_RSSI` (0x0502) | dBm | ESP-NOW |
| Uptime | `DP_UPTIME` (0x0503) | seconds | ESP32-S3 |
| SD Free Space | `DP_SD_FREE` (0x0504) | MB | SD card |
| Log Session | `DP_LOG_SESSION` (0x0505) | # | SD logger |

---

## Data Flow Architecture

```
  ┌──────────────────────┐
  │   MultiDisplay v2    │   ATmega2560, 8×EGT, 8×VDO, RPM, boost, etc.
  │   (Sensor Package)   │
  └─────────┬────────────┘
            │ HC-06 Bluetooth slave ("mdv2")
            │ 115200 baud, binary 95-byte frames @ ~100 Hz
            ▼
  ┌──────────────────────┐
  │  HC-05 Bluetooth     │   Pre-paired master, wired to J9 header
  │  (Master Module)     │
  └─────────┬────────────┘
            │ UART1 RX on GPIO20 (USB D+ reclaimed)
            ▼
  ┌──────────────────────┐        ESP-NOW          ┌──────────────────────┐
  │   LEFT Gauge Pod     │ ──────────────────────▶  │   CENTER Display     │
  │   (2.8" Round)       │  DATA_RESPONSE msgs     │   (4.3" Main Dash)   │
  │   Parses MD frames   │                         │   Displays + logs    │
  └──────────────────────┘                         └─────────┬────────────┘
                                                             │ ESP-NOW
                                                             ▼
                                                   ┌──────────────────────┐
                                                   │   RIGHT Gauge Pod    │
                                                   │   (2.8" Round)       │
                                                   └──────────────────────┘
```

---

## Adding New Sensors

To add a new sensor data source to OpenDash:

1. **Define a data point ID** in `common/include/opendash_data_model.h`
   (pick the next available ID in the appropriate range)
2. **Parse/extract the value** in the appropriate driver
   (e.g., `opendash_uart.c` for MD data, `gps_handler.c` for GPS)
3. **Forward to Center** via `send_data_point_to_center()` if the source
   node is not Center itself
4. **Display on UI** via `ui_manager_update_value(dp_id, value)` in the
   main loop of each display that should show it
5. **Log to SD** via `sd_logger_log_datapoint(dp_id, value)` on Center

No protocol changes needed — the ESP-NOW DATA_RESPONSE format carries
any `uint16_t dp_id + float value` pair automatically.

---

*OpenDash — Built for racers, by racers.*
