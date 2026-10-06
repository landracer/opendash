<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OpenDash — Features & Sensor Capabilities

> Complete matrix of sensor data available in OpenDash and the source systems
> that provide it. Updated 2026-10-04 — matrix sections reconciled with the wire
> truth in `left/main/main.c` (`forward_md_data_to_center()` /
> `forward_obd2_to_center()`) and the GPS/IMU drivers; [`DATAFLOW.md`](DATAFLOW.md)
> remains the authoritative data-path reference.

---

## Sensor Data Sources

OpenDash aggregates data from multiple sources into a unified display.
Each source connects to a different ESP32 node.

| Source | Connection | Node | Status |
|---|---|---|---|
| **MultiDisplay (MD)** | HC-05 Bluetooth → UART1 @ 115200, batched onto ESP-NOW (`MD_*` ids, 0x0800 block) | Left gauge pod | **LIVE** |
| **GPS (LC76G)** | I2C CASIC @ 100 kHz | GPS/Telemetry unit | **LIVE** — closed as read-only 1 Hz data pipe (wiki/GPS-LC76G-POSTMORTEM.md) |
| **IMU (QMI8658)** | I2C @ 0x6B | GPS unit + pod1/pod2 | **LIVE** — g-force display + parachute rollover votes |
| **OBD2 / ECU domain** | Decoded PIDs relayed by MD serial (0x0100-range ids) | Center → fan-out to pods | **LIVE via MD** — direct CAN planned (wiki/vesc-integration.md) |
| **VESC ESC** | CAN @ 500 kbps (planned via BMS Logger) | rAtTrax BMS Logger | Planned |
| **rAtTrax BMS** | ESP-NOW from BMS Logger / openDstream DP bridge | Center display | **LIVE (external repo)** |
| **Relay / MOS controllers** | ESP-NOW (`SET_RELAY 0x08` / `RELAY_STATUS 0x85`) | relay-4ch-hd, relay-8ch-a/b, mos-4ch-a/b | **LIVE** (boost loop staged — GPL-lineage clean-room gate, TODO §6.0) |
| **Demo generator** | Internal software | Center display | **Active (auto-halts on real data)** |

---

## MultiDisplay Sensor Matrix

MultiDisplay is the primary external sensor package. It runs on a Seeeduino
Mega (ATmega1280/2560) and streams all data over Bluetooth at ~100 Hz in a
95-byte binary frame.

### Forwarded to Center — LIVE, batch 1 (MD domain, 15 DPs)

`forward_md_data_to_center()` in `left/main/main.c` packs these 15 channels into
ONE `DATA_BATCH (0x88)` frame per UART frame (~every 200 ms) and relays it to
Center. EGT 1–8 ship under the *shared* engine ids 0x0112–0x011B; every other
MD-native channel ships under its own `MD_*` id (0x0800 block) so an MD-native
channel can NEVER populate an OBD-bound widget (and vice versa).

| Sensor | Data Point ID (forwarded under) | Unit | Scaling | MD Byte Offset | Notes |
|---|---|---|---|---|---|
| EGT 1–8 | `DP_EGT1`–`DP_EGT8` (shared ids 0x0112–0x011B) | °C | Raw int16 | 15-30 | Type K thermocouple, one pair per channel |
| Lambda / O2 | `DP_MD_LAMBDA` (0x0800) | ratio | ÷100 | 7-8 | Narrow or wideband |
| Mass Air Flow (LMM) | `DP_MD_MAF` (0x0801) | g/s | ÷100 | 9-10 | |
| RPM (MD tach input) | `DP_MD_RPM` (0x0117) | RPM | Raw uint16 | 1-2 | MD-domain id (predates the 0x0800 block) |
| Boost / MAP | `DP_MD_BOOST` (0x0802) | kPa | ÷100 | 3-4 | int16 ×100 in frame |
| Battery Voltage | `DP_MD_BAT` (0x0803) | V | ÷100 | 13-14 | int16 ×100 |
| Oil Pressure (VDO P1) | `DP_MD_OIL_PRESS` (0x0805) | kPa | ÷10 | 31-32 | |
| Oil Temp (VDO T1) | `DP_MD_OIL_TEMP` (0x0804) | °C | ÷10 | 37-38 | |

### Forwarded to Center — LIVE, batch 2 (OBD / ECU domain, 15 DPs)

The MD hardware's OBD reader (frame bytes 58-92) is relayed by
`forward_obd2_to_center()` as a SECOND `DATA_BATCH (0x88)` under the plain shared
engine ids: `DP_RPM` (0x0100), `DP_VEHICLE_SPEED` (0x0101), `DP_COOLANT_TEMP`,
`DP_INTAKE_TEMP`, `DP_ENGINE_LOAD`, `DP_THROTTLE_POS`, `DP_BOOST_PRESSURE`,
`DP_OIL_TEMP`, `DP_FUEL_PRESSURE`, `DP_MAF_RATE`, `DP_TIMING_ADVANCE`,
`DP_STFT_B1`, `DP_LTFT_B1`, `DP_BARO_PRESSURE`, `DP_DTC_COUNT`.
This batch is deliberately NOT gated on `obd2_present` — this bench's MD
prototype is the OBD transport (the old flag-gate silently killed the whole ECU
channel set when flags==0; regression found 2026-09-28).

### Defined in the data model but NOT currently forwarded (0x0806–0x080F)

Parsed from the frame and/or id-defined, but not in either relay batch:

| Sensor | Data Point ID (defined) | Unit | MD Byte Offset | Notes |
|---|---|---|---|---|
| VDO Pressure 2 | `DP_MD_VDO_P2` (0x0806) | kPa | 33-34 | Fuel pressure |
| VDO Pressure 3 | `DP_MD_VDO_P3` (0x0807) | kPa | 35-36 | Brake pressure |
| VDO Temperature 2 | `DP_MD_VDO_T2` (0x0808) | °C | 39-40 | Coolant / trans temp |
| VDO Temperature 3 | `DP_MD_VDO_T3` (0x0809) | °C | 41-42 | Aux temp |
| Vehicle Speed (VSS) | `DP_MD_SPEED` (0x080A) | km/h | 43-44 | Wheel-speed channel |
| Case Temperature | `DP_MD_CASE_TEMP` (0x080B) | °C | 11-12 | MD board internal temp |
| EFR Turbo Speed | `DP_MD_EFR_SPEED` (0x080C) | RPM | 49-52 | BorgWarner EFR sensor |
| Knock Sensor | `DP_MD_KNOCK` (0x080D) | raw | 53-54 | |
| Throttle Position | `DP_MD_THROTTLE` (0x080E) | % | 5-6 | 0-100% |
| Gear Position | `DP_MD_GEAR` (0x080F) | gear# | 45 | Computed from RPM+speed |

N75 Duty Cycle (byte 46) and Requested Boost (47-48) have no OpenDash data-point
id at all. *(VR6 padding: bytes 55-89 — 35 zero bytes on non-VR6 builds.)*

> **Byte offsets are relative to the payload** (after TAG byte).
> See `UART_CONNECTION.md` and `SERIAL_PROTOCOL.md` for authoritative byte map.

### Derived Values (Computed by OpenDash)

| Value | Data Point ID | Source | Status |
|---|---|---|---|
| Max EGT | `DP_EGT` (0x010C) | EGT 1-8 | **LIVE** — display/loggers compute `max(egt[0..7])` for widgets + SD log |
| O2/Lambda (engine-domain widget id) | `DP_O2_LAMBDA` (0x0116) | — | Center widget binding only; no forwarding source today (MD lambda ships under `DP_MD_LAMBDA` 0x0800) |

Note: `DP_MD_RPM` (0x0117) is **not** a derived copy — it is the native MD tach channel
(batch 1). The ECU RPM arrives independently under `DP_RPM` (0x0100) in batch 2.

---

## GPS Sensor Matrix

Provided by the LC76G GNSS module on the GPS/Telemetry display unit.

| Sensor | Data Point ID | Unit | Source | Status |
|---|---|---|---|---|
| GPS Speed | `DP_GPS_SPEED` (0x0200) | km/h | RMC sentence | **LIVE** |
| Heading | `DP_GPS_HEADING` (0x0201) | degrees | RMC sentence | **LIVE** |
| Latitude | `DP_LATITUDE` (0x0202) | degrees | GGA sentence | **LIVE** |
| Longitude | `DP_LONGITUDE` (0x0203) | degrees | GGA sentence | **LIVE** |
| Altitude | `DP_ALTITUDE` (0x0204) | meters | GGA sentence | **LIVE** |
| Satellite Count | `DP_SAT_COUNT` (0x0205) | count | GGA sentence | **LIVE** |
| HDOP | `DP_HDOP` (0x0206) | ratio | GGA sentence | **LIVE** |
| GPS Fix | `DP_GPS_FIX` (0x020D) | 0/1 | GGA sentence | **LIVE** |

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

## IMU Sensor Matrix (LIVE)

QMI8658 6-axis IMU on the GPS/Telemetry display unit — and on the pod1/pod2
detectors, where the same sensor feeds the parachute rollover detector via the
node's read callback (roll angle + roll rate are consumed **internally** by the
detector module, not fanned out as DPs — see
[`wiki/safety-deployment-system.md`](wiki/safety-deployment-system.md)).
The three g-force channels are live end-to-end: read, displayed, SD-logged, and
fanned out to Center under the ids below.

| Sensor | Data Point ID | Unit | Status |
|---|---|---|---|
| Lateral G-Force | `DP_GFORCE_LAT` (0x0300) | G | **LIVE** |
| Longitudinal G-Force | `DP_GFORCE_LONG` (0x0301) | G | **LIVE** |
| Vertical G-Force | `DP_GFORCE_VERT` (0x0302) | G | **LIVE** |
| Yaw Rate | `DP_YAW_RATE` (0x0303) | °/s | Planned (id defined, unused) |
| Pitch Rate | `DP_PITCH_RATE` (0x0304) | °/s | Planned (id defined, unused) |
| Roll Rate | `DP_ROLL_RATE` (0x0305) | °/s | Internal to rollover detector (DP fan-out planned) |
| Pitch Angle | `DP_PITCH_ANGLE` (0x0306) | ° | Planned (id defined, unused) |
| Roll Angle | `DP_ROLL_ANGLE` (0x0307) | ° | Internal to rollover detector (DP fan-out planned) |

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
  │   (2.8" Round)       │  DATA_BATCH 0x88 ×2    │   (4.3" Main Dash)   │
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

No protocol changes needed — the ESP-NOW `DATA_BATCH (0x88)` frame automatically
carries any set of `uint16_t dp_id + float value` pairs from slave to Center
(slave→master batches MUST use `DATA_BATCH` 0x88; `SET_DATA_BATCH` 0x0C is
master→slave only).

---

*OpenDash — Built for racers, by racers.*
