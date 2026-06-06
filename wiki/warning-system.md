<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OpenDash Warning & Alert System

Complete reference for all monitorable data points with recommended warning thresholds
and audio callout assignments for pod1/pod2 speakers.

## Architecture

- **Center**: Evaluates alarm rules against incoming data points, triggers visual warnings
  (flashing colored overlay) and sends audio alert commands to pods via ESP-NOW
- **Pod1/Pod2**: Receive `CMD_AUDIO_ALERT` with sound ID + priority, play WAV from SPIFFS
- **Alarm Config**: Up to 16 simultaneous alarms (`opendash_alarm_config_t`), stored in NVS
- **Severity Levels**: INFO (blue), WARNING (yellow), CRITICAL (red + flash + audio)

---

## Engine / ECU Data Points

| Data Point | ID | Warning Threshold | Critical Threshold | Audio Callout |
|---|---|---|---|---|
| **Coolant Temp** | 0x0102 | > 100°C (212°F) | > 110°C (230°F) | "Coolant temperature high" |
| **Oil Temp** | 0x0107 | > 120°C (248°F) | > 135°C (275°F) | "Oil temperature critical" |
| **Oil Pressure** | 0x0108 | < 150 kPa (21 PSI) | < 100 kPa (14 PSI) | "Low oil pressure" |
| **Intake Air Temp** | 0x0103 | > 55°C (131°F) | > 70°C (158°F) | "Intake temp high" |
| **Boost / MAP** | 0x0106 | > 200 kPa (29 PSI) | > 250 kPa (36 PSI) | "Boost limit exceeded" |
| **Engine RPM** | 0x0100 | > 6500 RPM | > 7200 RPM | "Overrev warning" |
| **Battery Voltage** | 0x010D | < 12.2V | < 11.5V | "Battery voltage low" |
| **AFR (Lean)** | 0x010A | > 15.0:1 | > 16.0:1 | "Running lean" |
| **AFR (Rich)** | 0x010A | < 11.0:1 | < 10.0:1 | "Running rich" |
| **Lambda** | 0x010B | > 1.05 | > 1.10 | "Lambda lean" |
| **EGT (max)** | 0x010C | > 800°C (1472°F) | > 900°C (1652°F) | "Exhaust temp critical" |
| **EGT1-EGT8** | 0x0112-0x011B | > 800°C | > 900°C | "EGT cylinder N high" |
| **Fuel Pressure** | 0x0109 | < 250 kPa (36 PSI) | < 200 kPa (29 PSI) | "Fuel pressure low" |
| **Fuel Level** | 0x0110 | < 15% | < 5% | "Fuel level low" |
| **Transmission Temp** | 0x0111 | > 100°C (212°F) | > 120°C (248°F) | "Trans temp high" |
| **Engine Load** | 0x0104 | > 90% | > 98% | -- (info only) |
| **Throttle Position** | 0x0105 | -- | -- | -- (display only) |
| **MAF Rate** | 0x010F | -- | -- | -- (display only) |
| **Timing Advance** | 0x010E | < -5° (detonation) | < -10° | "Timing retard detected" |
| **MIL Lamp** | 0x011D | == 1 (ON) | -- | "Check engine light on" |
| **DTC Count** | 0x011E | > 0 | > 3 | "Diagnostic codes stored" |

## GPS / Navigation

| Data Point | ID | Warning Threshold | Critical Threshold | Audio Callout |
|---|---|---|---|---|
| **GPS Speed** | 0x0200 | > pit lane limit | > track limit | "Speed limit exceeded" |
| **Satellite Count** | 0x0205 | < 6 | < 4 | "GPS signal weak" |
| **GPS Fix** | 0x020D | == 0 | -- | "No GPS fix" |
| **HDOP** | 0x0206 | > 3.0 | > 5.0 | "GPS accuracy degraded" |

## IMU / Motion

| Data Point | ID | Warning Threshold | Critical Threshold | Audio Callout |
|---|---|---|---|---|
| **Lateral G-Force** | 0x0300 | > 1.2G | > 1.5G | -- (info only) |
| **Longitudinal G** | 0x0301 | > 1.5G (braking) | > 2.0G | -- (display only) |
| **Roll Angle** | 0x0307 | > 15° | > 25° | "Vehicle roll detected" |
| **Pitch Angle** | 0x0306 | > 20° | > 30° | "Steep grade warning" |
| **Yaw Rate** | 0x0303 | > 45°/s | > 90°/s | "Oversteer detected" |

## BMS / Battery (rAtTrax)

| Data Point | ID | Warning Threshold | Critical Threshold | Audio Callout |
|---|---|---|---|---|
| **Pack Voltage** | 0x0400 | < 44V (12S low) | < 40V | "Battery voltage low" |
| **Pack Current** | 0x0401 | > 200A | > 300A | "Battery overcurrent" |
| **SOC** | 0x0402 | < 20% | < 10% | "Battery low" |
| **Cell V Min** | 0x0403 | < 3.2V | < 3.0V | "Cell voltage low" |
| **Cell V Max** | 0x0404 | > 4.15V | > 4.25V | "Cell voltage high" |
| **Cell V Delta** | 0x0405 | > 100 mV | > 200 mV | "Cell imbalance detected" |
| **BMS Temp Max** | 0x0406 | > 50°C | > 60°C | "Battery temperature high" |
| **BMS IC Temp** | 0x040A | > 70°C | > 85°C | "BMS overheating" |
| **SOH** | 0x0409 | < 80% | < 60% | "Battery degraded" |
| **Pack Power** | 0x0407 | > 15kW | > 20kW | -- (info only) |

## System

| Data Point | ID | Warning Threshold | Critical Threshold | Audio Callout |
|---|---|---|---|---|
| **CPU Temp** | 0x0500 | > 70°C | > 85°C | "System overheating" |
| **Free Heap** | 0x0501 | < 50 KB | < 20 KB | "Low memory warning" |
| **WiFi RSSI** | 0x0502 | < -75 dBm | < -85 dBm | "Signal weak" |
| **SD Free Space** | 0x0504 | < 100 MB | < 20 MB | "Storage almost full" |

## VESC Motor Controller

| Data Point | ID | Warning Threshold | Critical Threshold | Audio Callout |
|---|---|---|---|---|
| **VESC FET Temp** | 0x0607 | > 80°C | > 100°C | "Controller overheating" |
| **VESC Motor Temp** | 0x0608 | > 100°C | > 130°C | "Motor overheating" |
| **VESC Input Current** | 0x0609 | > 250A | > 350A | "Motor overcurrent" |
| **VESC Input Voltage** | 0x060C | < 44V | < 40V | "Motor battery low" |
| **VESC Fault Code** | 0x0614 | != 0 | -- | "Motor fault detected" |
| **VESC Duty Cycle** | 0x0602 | > 0.95 | == 1.0 | "Duty cycle limit" |

## Node Health (meta-warnings)

These are not data points but system-level conditions evaluated by the center:

| Condition | Detection Method | Severity | Audio Callout |
|---|---|---|---|
| **Node Offline** | 15s timeout on ESP-NOW | WARNING | "Device [name] offline" |
| **Node Reconnected** | STATUS_REPORT after offline | INFO | "Device [name] reconnected" |
| **ESP-NOW TX Fail** | 3 consecutive send failures | WARNING | "Communication error" |
| **SD Logging Stopped** | Write error or card removed | CRITICAL | "Logging stopped — check SD card" |
| **OBD2 No Response** | PID timeout > 5s | WARNING | "OBD2 not responding" |
| **Relay Mismatch** | Audit mask != commanded mask | CRITICAL | "Relay state mismatch" |
| **BMS Communication Lost** | No BMS data for 10s | CRITICAL | "BMS communication lost" |
| **Pre-flight Failed** | Checklist item not passed | WARNING | "Pre-flight check incomplete" |

---

## Audio Alert Protocol

### ESP-NOW Command: `CMD_AUDIO_ALERT` (proposed 0x0B)
```
Payload: [sound_id:1][priority:1][duration_ms:2]
```
- **sound_id**: Index into pod's SPIFFS sound table (0-63)
- **priority**: 0=low, 1=normal, 2=high (interrupts current playback)
- **duration_ms**: 0 = play full clip, else truncate at N ms

### Pod Audio Hardware
- **Pod1/Pod2**: Waveshare ESP32-S3-Touch-AMOLED-1.75
  - I2S output via GPIO (external DAC or direct PDM to speaker)
  - SPIFFS partition for WAV storage (~256KB budget)
  - 8kHz 8-bit mono WAV recommended for callouts (~8KB per second)

### Sound File Convention
```
/spiffs/
  snd_00_coolant_high.wav
  snd_01_oil_pressure.wav
  snd_02_overrev.wav
  snd_03_battery_low.wav
  snd_04_check_engine.wav
  snd_05_gps_weak.wav
  snd_06_battery_soc.wav
  snd_07_bms_hot.wav
  snd_08_node_offline.wav
  snd_09_comm_error.wav
  snd_10_generic_beep.wav
  snd_11_generic_alarm.wav
  ...
```

---

## Default Alarm Configuration

These are sensible defaults for a VR6/turbo race car. User can customize via
the DEVICE MGMT submenu on the center config page or via companion app.

```c
opendash_alarm_config_t default_alarms[OPENDASH_MAX_ALARMS] = {
    { OPENDASH_DP_COOLANT_TEMP,    0,    100.0f,  OPENDASH_ALARM_WARNING,  true  },
    { OPENDASH_DP_COOLANT_TEMP,    0,    110.0f,  OPENDASH_ALARM_CRITICAL, true  },
    { OPENDASH_DP_OIL_PRESSURE,    100.0f, 9999,  OPENDASH_ALARM_CRITICAL, true  },
    { OPENDASH_DP_OIL_TEMP,        0,    135.0f,  OPENDASH_ALARM_CRITICAL, true  },
    { OPENDASH_DP_BOOST_PRESSURE,  0,    250.0f,  OPENDASH_ALARM_CRITICAL, true  },
    { OPENDASH_DP_RPM,             0,    7200.0f, OPENDASH_ALARM_CRITICAL, true  },
    { OPENDASH_DP_BATTERY_VOLTAGE, 11.5f, 9999,   OPENDASH_ALARM_WARNING,  true  },
    { OPENDASH_DP_EGT,             0,    900.0f,  OPENDASH_ALARM_CRITICAL, true  },
    { OPENDASH_DP_MIL_ON,          0,    0.5f,    OPENDASH_ALARM_WARNING,  true  },
    { OPENDASH_DP_SOC,             10.0f, 9999,   OPENDASH_ALARM_CRITICAL, true  },
    { OPENDASH_DP_CELL_V_MIN,      3.0f, 9999,    OPENDASH_ALARM_CRITICAL, true  },
    { OPENDASH_DP_CELL_V_DELTA,    0,    200.0f,  OPENDASH_ALARM_CRITICAL, true  },
    { OPENDASH_DP_BMS_TEMP_MAX,    0,    60.0f,   OPENDASH_ALARM_CRITICAL, true  },
    { OPENDASH_DP_FUEL_LEVEL,      5.0f, 9999,    OPENDASH_ALARM_CRITICAL, true  },
    { OPENDASH_DP_TRANS_TEMP,      0,    120.0f,  OPENDASH_ALARM_CRITICAL, true  },
    { OPENDASH_DP_CPU_TEMP,        0,    85.0f,   OPENDASH_ALARM_WARNING,  true  },
};
```

---

## Version History

| Version | Date | Changes |
|---|---|---|
| v0.8.1 | 2025-01-12 | Initial warning system document. Comprehensive threshold catalog. |
