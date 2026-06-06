<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OBD-II Integration via MultiDisplay

> **How OpenDash receives, parses, and displays OBD-II data from MultiDisplay.**
> The MultiDisplay (MD) firmware bridges the vehicle's OBD-II port via an ELM327
> IC and embeds the decoded PID values into its standard binary serial frame.
> OpenDash receives this data over Bluetooth UART and maps it to display data points.
>
> **UI Access — Dual OBD Screens (v0.7.3+):**
> - **Normal mode:** Enable `OBD PAGE: ON` in Config → OBD2 SETUP. OBD Performance
>   Dashboard appears in the normal swipe cycle (ENGINE → GPS → MD → **OBD** → CONFIG).
> - **Debug mode:** Enter via `CONFIG → DEBUG MODE`, swipe to OBD. Shows the raw 5×3
>   data grid with all 15 OBD-II PID values for verification.
>
> **MIL/CEL Detection (v0.7.3+):** MD firmware polls PID 0x01 every 10 seconds for
> MIL lamp status and DTC count. `obd_flags` byte: bit0=ELM ready, bit1=DTCs present,
> bit2=MIL on. OpenDash checks bits 1+2 to trigger the CEL indicator overlay.
>
> **Standards Compliance:** SAE J1979-2, ISO 15031-5

---

## Table of Contents

1. [System Overview](#1-system-overview)
2. [Standards Reference](#2-standards-reference)
3. [Data Flow Architecture](#3-data-flow-architecture)
4. [UART Frame Format — OBD2 Bytes](#4-uart-frame-format--obd2-bytes)
5. [Standalone OBD2 Frame (TAG 0x4F)](#5-standalone-obd2-frame-tag-0x4f)
6. [OpenDash Parser Implementation](#6-opendash-parser-implementation)
7. [Data Point Mapping](#7-data-point-mapping)
8. [OpenDash Struct Fields](#8-opendash-struct-fields)
9. [OBD2 Detection Logic](#9-obd2-detection-logic)
10. [ESP-NOW Forwarding](#10-esp-now-forwarding)
11. [Display Integration](#11-display-integration)
12. [Configuration](#12-configuration)
13. [Troubleshooting](#13-troubleshooting)
14. [Relevant Source Files](#14-relevant-source-files)
15. [Future Improvements](#15-future-improvements)

---

## 1. System Overview

```
 Vehicle ECU                MultiDisplay (ATmega2560)               OpenDash (ESP32-S3)
┌──────────┐   ISO 9141-2  ┌────────────────────────┐  BT SPP    ┌─────────────────────┐
│ OBD-II   │◄─── K-line ──►│ ELM327DSL IC           │            │  LEFT Gauge Pod      │
│ DLC Port │               │ ↕ Serial1 38400 baud   │            │  opendash_uart.c     │
└──────────┘               │                        │            │  parse_binary_frame()│
                           │ OBD2Manager polls PIDs  │            │                     │
                           │ Embeds 35-byte summary  │  115200    │  ↓ obd2_present?    │
                           │ into binary frame       │ ─────────►│  Maps to data points │
                           │ bytes 58-92             │  HC-06→    │                     │
                           │                        │  HC-05     │  ↓ ESP-NOW           │
                           │ Also sends standalone   │            │  forward to CENTER   │
                           │ frame TAG=0x4F          │            │  → LEFT, RIGHT pods  │
                           └────────────────────────┘            └─────────────────────┘
```

MultiDisplay handles all OBD-II communication — OpenDash is a **passive receiver**.
It never sends OBD-II commands itself. The ELM327 chip, PID polling, and decode
formulas all run on the ATmega2560. OpenDash simply reads the already-decoded
values from the serial frame.

---

## 2. Standards Reference

The PID values embedded in the MultiDisplay serial frame are decoded using
standard SAE / ISO formulas. OpenDash does NOT decode raw OBD-II responses —
it receives pre-decoded, scaled values. However, understanding the standards
is essential for interpreting values correctly and adding new PIDs.

### SAE J1979-2 (OBD-II Diagnostic Test Modes)
- Defines Mode 01 live data PIDs, Mode 03 DTCs, Mode 04 clear DTCs
- All PID numbers and decode formulas come from J1979 Section 6
- The 16 PIDs in the MD → OpenDash frame are standard Mode 01 PIDs

### ISO 15031-5 (Emissions-Related Diagnostic Services)
- International equivalent of SAE J1979 — identical PID definitions
- DTC encoding format (2-byte P/C/B/U codes)

### Value Scaling Convention
All values arrive as **int16_t × 100 fixed-point**, little-endian.
To recover the float value:
```c
float value = (float)rd_s16(&payload[offset]) / 100.0f;
```
This matches the SAE J1979 decoded result after the firmware applies the formula.

---

## 3. Data Flow Architecture

```
1. MD firmware polls ELM327 (one PID per main loop cycle)
   └─ ELM327::requestPID("010C\r") → "410C1AF8" → RPM = 1726.0

2. OBD2Data::decodePID() applies SAE J1979 formula
   └─ PID 0x0C: ((0x1A × 256) + 0xF8) / 4 = 1726.0 RPM

3. OBD2Data::updateCache() stores float value with timestamp

4. MultidisplayController::serialSend() (SERIALOUT_BINARY)
   └─ Writes bytes 0-57 (standard MD data: RPM, boost, EGT, etc.)
   └─ Writes bytes 58-92 (OBD2 summary: flags + 16 PIDs × int16 LE)
   └─ Writes byte 94 (ETX)
   └─ MD_SERIAL_WRITE sends to BOTH Serial (USB) and Serial2 (BT)

5. HC-06 (MD) → HC-05 (OpenDash) Bluetooth SPP link at 115200 baud

6. opendash_uart.c → uart_rx_task() receives bytes
   └─ State machine: STX → TAG check → buffer 93 payload bytes → ETX
   └─ TAG=0x5F (95): parse_binary_frame() extracts all fields
   └─ TAG=0x4F (79): standalone OBD2 frame — drained/skipped

7. parse_binary_frame() extracts OBD2 fields from offsets 58-92
   └─ Stores in s_latest_data (opendash_md_data_t struct)
   └─ Sets obd2_present = true if flags byte ≠ 0

8. left/main/main.c reads s_latest_data every 200ms
   └─ If obd2_present: maps 13 PID values → OpenDash data point IDs
   └─ ui_manager_update_value(OPENDASH_DP_xxx, value)

9. ESP-NOW DATA_RESPONSE forwards data points to CENTER and RIGHT pods
```

---

## 4. UART Frame Format — OBD2 Bytes

The standard MD binary frame is 95 bytes. The OBD2 data occupies the
**variant data region** at bytes 58–92 (35 bytes).

### Complete OBD2 Byte Map

| Byte Offset | Size | Field Name      | Type       | Scaling | SAE PID | Unit    |
|-------------|------|-----------------|------------|---------|---------|---------|
| 58          | 1    | obd2_flags      | `uint8_t`  | bitfield| —       | —       |
| 59–60       | 2    | obd2_rpm        | `int16_t` LE | ×100 | 0x0C    | RPM     |
| 61–62       | 2    | obd2_speed      | `int16_t` LE | ×100 | 0x0D    | km/h    |
| 63–64       | 2    | obd2_coolant_temp| `int16_t` LE | ×100| 0x05    | °C      |
| 65–66       | 2    | obd2_engine_load| `int16_t` LE | ×100 | 0x04    | %       |
| 67–68       | 2    | obd2_intake_map | `int16_t` LE | ×100 | 0x0B    | kPa     |
| 69–70       | 2    | obd2_throttle   | `int16_t` LE | ×100 | 0x11    | %       |
| 71–72       | 2    | obd2_intake_temp| `int16_t` LE | ×100 | 0x0F    | °C      |
| 73–74       | 2    | obd2_maf_rate   | `int16_t` LE | ×100 | 0x10    | g/s     |
| 75–76       | 2    | obd2_timing_adv | `int16_t` LE | ×100 | 0x0E    | ° BTDC  |
| 77–78       | 2    | obd2_stft_b1    | `int16_t` LE | ×100 | 0x06    | %       |
| 79–80       | 2    | obd2_ltft_b1    | `int16_t` LE | ×100 | 0x07    | %       |
| 81–82       | 2    | obd2_fuel_press | `int16_t` LE | ×100 | 0x0A    | kPa     |
| 83–84       | 2    | obd2_baro_press | `int16_t` LE | ×100 | 0x33    | kPa     |
| 85–86       | 2    | obd2_oil_temp   | `int16_t` LE | ×100 | 0x5C    | °C      |
| 87–88       | 2    | obd2_ctrl_voltage| `int16_t` LE | ×100| 0x42    | V       |
| 89–90       | 2    | obd2_fuel_level | `int16_t` LE | ×100 | 0x2F    | %       |
| 91–92       | 2    | reserved        | —          | —       | —       | —       |

### Flags Byte Detail (Offset 58)

| Bit | Mask | Meaning                                                |
|-----|------|--------------------------------------------------------|
| 0   | 0x01 | ELM327 initialized and communicating with vehicle ECU  |
| 1   | 0x02 | At least one DTC stored in ECU (from PID 0x01 or readDTCs) |
| 2   | 0x04 | MIL (Check Engine) lamp is ON (from PID 0x01 byte A bit7) |
| 3–7 | —    | Reserved (always 0)                                    |

### Parser Offset Defines (in opendash_uart.c)

```c
#define MD_OFF_OBD2       58
#define MD_OFF_OBD2_FLAGS 58
#define MD_OFF_OBD2_RPM   59
#define MD_OFF_OBD2_SPD   61
#define MD_OFF_OBD2_CLT   63
#define MD_OFF_OBD2_LOAD  65
#define MD_OFF_OBD2_MAP   67
#define MD_OFF_OBD2_TPS   69
#define MD_OFF_OBD2_IAT   71
#define MD_OFF_OBD2_MAF   73
#define MD_OFF_OBD2_ADV   75
#define MD_OFF_OBD2_STFT  77
#define MD_OFF_OBD2_LTFT  79
#define MD_OFF_OBD2_FP    81
#define MD_OFF_OBD2_BARO  83
#define MD_OFF_OBD2_OIL   85
#define MD_OFF_OBD2_VOLT  87
#define MD_OFF_OBD2_FUEL  89
```

---

## 5. Standalone OBD2 Frame (TAG 0x4F)

MultiDisplay also sends a standalone variable-length frame with per-PID detail:

```
STX(0x02)  TAG(0x4F)  time[4]  count[1]  {pid[1] val_hi[1] val_lo[1]}×N  ETX(0x03)
```

**OpenDash handling:** The UART RX task detects TAG=0x4F and **drains** the
frame (reads bytes until ETX without parsing). This is deliberate — the
35-byte summary in the main frame already contains all needed values.
The standalone frame exists for PC logging software that wants per-PID detail.

```c
// In uart_rx_task():
if (tag == 0x4F) {
    // Standalone OBD2 frame — drain until ETX
    while (bytes_read < max_drain) {
        if (byte == 0x03) break;   // ETX found
    }
    continue;  // Skip to next frame
}
```

---

## 6. OpenDash Parser Implementation

### Location

`common/src/opendash_uart.c` → `parse_binary_frame()`

### OBD2 Extraction Code

```c
/* ── OBD-II summary (35 bytes at offset 58) ─────────────────────── */
uint8_t obd2_flags = payload[MD_OFF_OBD2_FLAGS];
s_latest_data.obd2_flags       = obd2_flags;
s_latest_data.obd2_rpm         = (float)rd_s16(&payload[MD_OFF_OBD2_RPM])   / 100.0f;
s_latest_data.obd2_speed       = (float)rd_s16(&payload[MD_OFF_OBD2_SPD])   / 100.0f;
s_latest_data.obd2_coolant_temp= (float)rd_s16(&payload[MD_OFF_OBD2_CLT])   / 100.0f;
s_latest_data.obd2_engine_load = (float)rd_s16(&payload[MD_OFF_OBD2_LOAD])  / 100.0f;
s_latest_data.obd2_intake_map  = (float)rd_s16(&payload[MD_OFF_OBD2_MAP])   / 100.0f;
s_latest_data.obd2_throttle    = (float)rd_s16(&payload[MD_OFF_OBD2_TPS])   / 100.0f;
s_latest_data.obd2_intake_temp = (float)rd_s16(&payload[MD_OFF_OBD2_IAT])   / 100.0f;
s_latest_data.obd2_maf_rate    = (float)rd_s16(&payload[MD_OFF_OBD2_MAF])   / 100.0f;
s_latest_data.obd2_timing_adv  = (float)rd_s16(&payload[MD_OFF_OBD2_ADV])   / 100.0f;
s_latest_data.obd2_stft_b1     = (float)rd_s16(&payload[MD_OFF_OBD2_STFT])  / 100.0f;
s_latest_data.obd2_ltft_b1     = (float)rd_s16(&payload[MD_OFF_OBD2_LTFT])  / 100.0f;
s_latest_data.obd2_fuel_press  = (float)rd_s16(&payload[MD_OFF_OBD2_FP])    / 100.0f;
s_latest_data.obd2_baro_press  = (float)rd_s16(&payload[MD_OFF_OBD2_BARO])  / 100.0f;
s_latest_data.obd2_oil_temp    = (float)rd_s16(&payload[MD_OFF_OBD2_OIL])   / 100.0f;
s_latest_data.obd2_ctrl_voltage= (float)rd_s16(&payload[MD_OFF_OBD2_VOLT])  / 100.0f;
s_latest_data.obd2_fuel_level  = (float)rd_s16(&payload[MD_OFF_OBD2_FUEL])  / 100.0f;
s_latest_data.obd2_present     = (obd2_flags != 0);
```

### Little-Endian Helper

```c
static inline int16_t rd_s16(const uint8_t *p) {
    return (int16_t)(p[0] | ((uint16_t)p[1] << 8));
}
```

---

## 7. Data Point Mapping

When `obd2_present` is `true`, the left pod maps OBD2 values to OpenDash
data point IDs.  These are the same IDs used by the native MD analog
sensors where applicable — OBD2 values **augment or replace** the analog
sensor readings.

| OBD2 Field          | OpenDash Data Point        | ID     | Unit    |
|---------------------|----------------------------|--------|---------|
| `obd2_rpm`          | `OPENDASH_DP_RPM`          | 0x0100 | RPM     |
| `obd2_speed`        | `OPENDASH_DP_VEHICLE_SPEED`| 0x0101 | km/h    |
| `obd2_coolant_temp` | `OPENDASH_DP_COOLANT_TEMP` | 0x0102 | °C      |
| `obd2_engine_load`  | `OPENDASH_DP_ENGINE_LOAD`  | 0x0103 | %       |
| `obd2_intake_map`   | `OPENDASH_DP_BOOST_PRESSURE`| 0x0104| kPa     |
| `obd2_throttle`     | `OPENDASH_DP_THROTTLE_POS` | 0x0105 | %       |
| `obd2_intake_temp`  | `OPENDASH_DP_INTAKE_TEMP`  | 0x0106 | °C      |
| `obd2_maf_rate`     | `OPENDASH_DP_MAF_RATE`     | 0x0107 | g/s     |
| `obd2_timing_adv`   | `OPENDASH_DP_TIMING_ADVANCE`| 0x0108| ° BTDC  |
| `obd2_fuel_press`   | `OPENDASH_DP_FUEL_PRESSURE`| 0x0109 | kPa     |
| `obd2_oil_temp`     | `OPENDASH_DP_OIL_TEMP`     | 0x010A | °C      |
| `obd2_fuel_level`   | `OPENDASH_DP_FUEL_LEVEL`   | 0x010B | %       |
| `obd2_ctrl_voltage` | `OPENDASH_DP_BATTERY_VOLTAGE`| 0x010C| V       |

### Unmapped OBD2 Fields (available in struct but not yet displayed)

| OBD2 Field          | Reason Not Mapped                              |
|---------------------|------------------------------------------------|
| `obd2_stft_b1`      | No STFT data point defined yet                 |
| `obd2_ltft_b1`      | No LTFT data point defined yet                 |
| `obd2_baro_press`   | No barometric pressure data point defined yet  |

> **To add:** Define new data point IDs in `opendash_data_model.h` in the
> 0x0100 range and add `ui_manager_update_value()` calls in `left/main/main.c`.

---

## 8. OpenDash Struct Fields

All OBD2 values are stored in `opendash_md_data_t` (defined in `opendash_uart.h`):

```c
typedef struct {
    // ... (existing MD fields: rpm, boost, egt, vdo, etc.)

    /* ── OBD-II summary (from bytes 58-92 of MD frame) ── */
    uint8_t  obd2_flags;        /* bit0=ELM ready, bit1=has DTCs, bit2=MIL on */
    float    obd2_rpm;
    float    obd2_speed;
    float    obd2_coolant_temp;
    float    obd2_engine_load;
    float    obd2_intake_map;
    float    obd2_throttle;
    float    obd2_intake_temp;
    float    obd2_maf_rate;
    float    obd2_timing_adv;
    float    obd2_stft_b1;
    float    obd2_ltft_b1;
    float    obd2_fuel_press;
    float    obd2_baro_press;
    float    obd2_oil_temp;
    float    obd2_ctrl_voltage;
    float    obd2_fuel_level;
    bool     obd2_present;      /* true if OBD2 flags byte ≠ 0 */
} opendash_md_data_t;
```

---

## 9. OBD2 Detection Logic

OBD2 presence is determined **automatically** — no manual configuration needed:

```c
s_latest_data.obd2_present = (obd2_flags != 0);
```

- **Non-OBD2 MD builds:** Bytes 58–92 are either all zeros (VR6) or Digifant
  K-line data. The flags byte at offset 58 will be 0x00, so `obd2_present`
  stays `false` and no OBD2 data points are pushed.
- **OBD2 MD builds:** Flags byte has bit 0 set (ELM ready), so
  `obd2_present = true` and all 13 PID values are mapped.

This means OpenDash is **backward compatible** — it works with both OBD2 and
non-OBD2 MultiDisplay firmware without any configuration changes.

---

## 10. ESP-NOW Forwarding

OBD2 data follows the same path as all other MD sensor data:

1. **Left pod** receives binary frame via UART and extracts OBD2 fields
2. Left pod calls `ui_manager_update_value()` for each mapped data point
3. `forward_md_data_to_center()` packages data points into ESP-NOW
   `DATA_RESPONSE` messages and broadcasts to Center
4. Center's `espnow_master.c` dispatches incoming data points to
   `ui_manager_update_value()` — which automatically updates any gauge
   bound to that data point ID
5. Center forwards selected data points to Right pod via ESP-NOW

> **No OBD2-specific forwarding code is needed.** The data follows the
> standard data point pipeline. Any new OBD2 data points added will
> automatically forward as long as they're mapped in the left pod.

---

## 11. Display Integration

### Gauge Page Configuration

OBD2 data points use the same IDs as their analog sensor equivalents.
Any gauge page configured to show `OPENDASH_DP_RPM`, `OPENDASH_DP_COOLANT_TEMP`,
etc. will automatically display OBD2 values when `obd2_present` is true.

### Recommended OBD2-Focused Gauge Pages

| Page | Primary Arc            | Secondary Box          |
|------|------------------------|------------------------|
| 1    | RPM (0–8000)           | Vehicle Speed          |
| 2    | Coolant Temp (0–130°C) | Oil Temp               |
| 3    | Engine Load (0–100%)   | Throttle Position      |
| 4    | Fuel Level (0–100%)    | Battery Voltage        |
| 5    | MAP (0–255 kPa)        | MAF Rate               |
| 6    | Timing Advance         | STFT Bank 1            |

---

## 12. Configuration

### MultiDisplay Side

All OBD2 configuration is in `MultidisplayDefinesOBD2.h` on the MD firmware:

| Parameter               | Default       | Description                    |
|-------------------------|---------------|--------------------------------|
| `OBD2_ELM327_BAUD`     | 38400         | ELM327 UART baud rate          |
| `OBD2_SERIAL`          | `Serial1`     | ATmega UART port               |
| `OBD2_ELM_TIMEOUT_MS`  | 2000          | Response timeout (ms)          |
| `OBD2_ELM_RETRIES`     | 2             | Retry count per command        |
| `OBD2_ELM_PROTOCOL`    | 3             | ISO 9141-2 (change for CAN)   |
| `OBD2_MAX_TRACKED_PIDS`| 32            | Max simultaneous PIDs          |

### OpenDash Side

**No configuration needed.** OBD2 parsing is always active in the UART parser.
When the flags byte is zero (non-OBD2 firmware), the values are parsed but
`obd2_present` stays false and no data points are pushed.

---

## 13. Troubleshooting

### No OBD2 Data Visible on OpenDash

| Check | What To Do |
|-------|------------|
| Flags byte | Enable UART debug logging (`OPENDASH_UART_DEBUG=1`), check if byte 58 shows non-zero flags |
| Bluetooth link | Verify full 95-byte frames arriving (not just STX+ETX). Check MD firmware has `BLUETOOTH_ON_SERIAL2` enabled |
| Data point mapping | Verify `left/main/main.c` has the `if (md.obd2_present)` block with `ui_manager_update_value()` calls |
| Gauge page | Confirm the gauge page is configured to show one of the mapped data point IDs |

### OBD2 Values Stuck at Zero

| Cause | Solution |
|-------|----------|
| ELM327 not initialized | Check MD serial monitor for ELM327 init messages |
| ECU not supporting PID | Some PIDs return NO DATA — value stays at zero. Normal. |
| Stale cache | Values update at 1–3 Hz per PID. Wait a few seconds after engine start. |

### Bad Frame Rate with OBD2

OBD2 polling adds ~50–200 ms latency per PID to the MD main loop.
If the standard sensor update rate feels slow:
- Reduce the number of enabled Tier 1 PIDs
- Move less-critical PIDs from Tier 1 → Tier 2 or 3
- Consider using `OBD2_ELM_PROTOCOL 0` (auto) — CAN is much faster

---

## 14. Relevant Source Files

### MultiDisplay Firmware (`/multidisplay/`)

| File | Purpose |
|------|---------|
| `MultidisplayDefinesOBD2.h` | ELM327 config, PID enables, tier assignments |
| `MultidisplayDefines.h` | ECU type, BT enable, MD_SERIAL_WRITE macro |
| `obd2_implementation/ELM327.h/.cpp` | ELM327 AT command driver |
| `obd2_implementation/OBD2Data.h/.cpp` | SAE J1979 decode formulas + cache |
| `obd2_implementation/OBD2Manager.h/.cpp` | Poll scheduler, serial frame builder |
| `MultidisplayController.cpp` | Integration: begin/poll/serialSend |
| `OBD2_IMPLEMENTATION.md` | Comprehensive MD-side documentation |
| `SERIAL_PROTOCOL.md` | Complete binary frame specification |

### OpenDash (`/opendash/`)

| File | Purpose |
|------|---------|
| `common/include/opendash_uart.h` | `opendash_md_data_t` struct with OBD2 fields |
| `common/src/opendash_uart.c` | UART parser: OBD2 offsets, parse_binary_frame(), TAG=0x4F drain |
| `common/include/opendash_data_model.h` | Data point ID definitions |
| `left/main/main.c` | OBD2 → data point mapping (13 values) |

---

## 15. Future Improvements

- [ ] **Map remaining OBD2 fields:** STFT, LTFT, baro pressure → new data point IDs
- [ ] **DTC display widget:** Show DTC count and code list on Center display
  when `obd2_flags & 0x02` is set
- [ ] **OBD2 status indicator:** Small icon on gauge display showing ELM327
  connection state (flags bit 0)
- [ ] **Parse standalone 0x4F frames:** Extract per-PID detail for
  high-resolution logging (currently drained/skipped)
- [ ] **Configurable PID-to-gauge mapping:** Allow user to choose which OBD2
  values appear on which gauge pages (NVS setting)
- [ ] **Direct CAN OBD2 on Center:** Bypass MultiDisplay entirely with a
  CAN transceiver on the ESP32-S3 Center node
- [ ] **OBD2 data logging to SD:** Include OBD2 values in GPS SD card log files
- [ ] **Protocol version field:** Add a version byte to the MD frame so
  OpenDash knows which variant data format to expect

---

*Last updated: July 2026*
*Standards: SAE J1979-2, ISO 15031-5*
*See also: `OBD2_IMPLEMENTATION.md` in the multidisplay firmware repository*
