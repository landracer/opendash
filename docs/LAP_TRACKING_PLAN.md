<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# Lap Tracking Plan — BonoGPS-Inspired On-Device Implementation

> Detailed design for integrating GPS-based lap timing, sector tracking, and
> predictive delta into OpenDash. Inspired by the BonoGPS project's u-blox
> configuration patterns, but with **all computation done on-device** (ESP32-S3)
> rather than relying on a mobile app.
>
> **Reference:** https://github.com/renatobo/bonogps
> **Status:** Design phase — March 2026

---

## Table of Contents

1. [Design Philosophy](#design-philosophy)
2. [What We Take from BonoGPS](#what-we-take)
3. [What We Build Ourselves](#what-we-build)
4. [GPS Hardware & Configuration](#gps-hardware)
5. [Lap Detection Algorithm](#lap-detection)
6. [Sector Timing](#sector-timing)
7. [Predictive Lap Delta](#predictive-delta)
8. [Track Database](#track-database)
9. [Data Logging Integration](#data-logging)
10. [Implementation Plan](#implementation-plan)
11. [rAtTrax-BMS Integration](#bms-integration)
12. [File Structure](#file-structure)

---

## 1. Design Philosophy <a name="design-philosophy"></a>

**BonoGPS is a GPS relay** — it reads NMEA from a u-blox module and forwards
raw sentences to mobile apps (Harry's Lap Timer, RaceChrono, TrackAddict)
via BLE, BT-SPP, or TCP. The **apps** do all lap detection and timing.

**OpenDash computes everything on-device.** We don't rely on a phone.
The ESP32-S3 handles:
- GPS configuration (u-blox UBX binary commands)
- NMEA sentence parsing
- Start/finish line crossing detection
- Sector boundary crossings
- Lap time computation
- Predictive delta vs. reference lap
- On-screen display of all timing data
- SD card logging of position + timing + sensor data

We take BonoGPS's **proven u-blox configuration sequences** and **GPS
initialization patterns**, then write our own timing engine.

---

## 2. What We Take from BonoGPS <a name="what-we-take"></a>

### UBX Binary Configuration Commands

BonoGPS contains ready-to-use byte sequences for u-blox configuration.
These are standard UBX protocol commands — not proprietary to BonoGPS.

| Command | Purpose | BonoGPS Reference |
|---|---|---|
| `UBLOX_INIT_10HZ` | Set 10 Hz update rate | UBX-CFG-RATE, measRate=100ms |
| `UBLOX_INIT_25HZ` | Set 25 Hz (M10 only) | UBX-CFG-RATE, measRate=40ms |
| `UBLOX_INIT_*` | Baud rate changes | UBX-CFG-PRT |
| `UBLOX_GxGGA_ON/OFF` | Enable/disable GGA | UBX-CFG-MSG |
| `UBLOX_GxRMC_ON/OFF` | Enable/disable RMC | UBX-CFG-MSG |
| `UBLOX_GxGSA_ON/OFF` | Enable/disable GSA | UBX-CFG-MSG |
| Dynamic model | Automotive (CFG-NAV5) | Pre-saved config files |

### GPS Initialization Pattern

```
1. Auto-detect baud rate (try 115200, 38400, 9600)
2. Send configuration commands (rate, messages, dynamic model)
3. Enable only needed NMEA sentences (GGA + RMC for position + speed)
4. Poll GSA/GSV at intervals (not every fix) to save bandwidth
```

### BLE Service UUIDs (Reference)

If we later add BLE data streaming to a phone app:
- Location & Navigation service: `0x1819`
- Location & Speed characteristic: `0x2A67`
- MTU: 185 bytes for NimBLE

---

## 3. What We Build Ourselves <a name="what-we-build"></a>

BonoGPS does **none** of the following. We implement from scratch:

| Feature | Algorithm | Complexity |
|---|---|---|
| **Line crossing detection** | Perpendicular distance + ray casting | Medium |
| **Sector boundary detection** | Same algorithm, applied to N boundaries | Medium |
| **Lap time calculation** | Interpolated crossing timestamp | Low |
| **Best lap tracking** | Simple min(lap_time) with NVS persistence | Low |
| **Predictive delta** | Distance-based interpolation vs. reference lap | High |
| **Track database** | JSON on SD card, auto-detect by GPS proximity | Medium |
| **Reference lap storage** | Position-time array logged to SD | Medium |

---

## 4. GPS Hardware & Configuration <a name="gps-hardware"></a>

### Supported GPS Modules

OpenDash supports two GPS integration paths:

| Path | Module | Chipset | Interface | Max Hz | Node |
|---|---|---|---|---|---|
| **Primary** | Waveshare LC76G | Quectel LC76G | I2C CASIC | 10 Hz | GPS unit |
| **Alternative** | BN-220/BN-880 | u-blox M8N | UART | 10 Hz | Any node |
| **Future** | BK-280/BK-880 | u-blox M10 | UART | 25 Hz | Any node |

### LC76G Configuration (Existing Driver)

The GPS unit already has a working LC76G I2C driver (`gps_handler.c` v15L2).
For lap timing we need to:

1. **Enable 10 Hz mode** after first fix: `$PAIR050,100*CS\r\n`
2. **Enable only GGA + RMC** — minimize bus traffic
3. **Parse with millisecond timestamp resolution** — GGA `hhmmss.sss`

### u-blox Configuration (New — from BonoGPS patterns)

For u-blox modules (BN-220, BN-880, BK-280):

```c
// UBX-CFG-RATE: 10 Hz (100ms measurement, 1:1 nav rate, GPS time)
static const uint8_t UBX_10HZ[] = {
    0xB5, 0x62, 0x06, 0x08, 0x06, 0x00,
    0x64, 0x00,   // measRate = 100 ms
    0x01, 0x00,   // navRate = 1
    0x01, 0x00,   // timeRef = GPS
    0x7A, 0x12    // checksum
};

// UBX-CFG-NAV5: Automotive dynamic model
static const uint8_t UBX_AUTOMOTIVE[] = {
    0xB5, 0x62, 0x06, 0x24, 0x24, 0x00,
    0xFF, 0xFF,   // mask
    0x04,         // dynModel = 4 (automotive)
    // ... remaining fields zeroed (use current) ...
};
```

### Baud Rate Handling

- OpenDash starts UART at 9600, sends UBX baud change to 115200
- BonoGPS pattern: auto-baud detect via `HardwareSerial.begin(0)` on ESP32
  (we can adopt this for u-blox modules)

---

## 5. Lap Detection Algorithm <a name="lap-detection"></a>

### Start/Finish Line Definition

A start/finish line is defined by **two GPS coordinates** forming a line
segment perpendicular to the track direction:

```c
typedef struct {
    double lat1, lon1;   // Left edge of line
    double lat2, lon2;   // Right edge of line
    float  heading;      // Expected crossing direction (degrees)
    float  tolerance;    // Heading tolerance (±degrees)
} lap_line_t;
```

Stored in NVS per track. Can be set via:
- Manual coordinate entry (from map)
- "Set current position as start/finish" button on display
- SD card track database

### Crossing Detection — Perpendicular Distance Method

For each GPS fix, compute the perpendicular distance from the current
position to the start/finish line segment:

```
d = perpendicular_distance(current_pos, line_p1, line_p2)
```

**Algorithm:**
1. Convert lat/lon to local XY (meters) using equirectangular approximation
2. Compute signed distance from position to line
3. Track sign changes: `d_prev > 0` and `d_now <= 0` = **crossing**
4. Validate: heading within ±tolerance of expected direction
5. **Interpolate exact crossing time** between the two fixes:
   `t_cross = t_prev + (t_now - t_prev) * |d_prev| / (|d_prev| + |d_now|)`

```c
// Equirectangular approximation (fast, accurate for <100km)
static void latlon_to_xy(double lat, double lon,
                         double ref_lat, double ref_lon,
                         float *x, float *y)
{
    const double R = 6371000.0;  // Earth radius meters
    double cos_lat = cos(ref_lat * M_PI / 180.0);
    *x = (float)((lon - ref_lon) * M_PI / 180.0 * R * cos_lat);
    *y = (float)((lat - ref_lat) * M_PI / 180.0 * R);
}

// Signed perpendicular distance from point P to line AB
static float signed_distance(float px, float py,
                             float ax, float ay,
                             float bx, float by)
{
    float dx = bx - ax, dy = by - ay;
    float len = sqrtf(dx*dx + dy*dy);
    if (len < 0.001f) return 0.0f;
    return (dy * (px - ax) - dx * (py - ay)) / len;
}
```

### Anti-Bounce

- Minimum lap time: 10 seconds (ignore immediate re-crossings)
- Must be >50m from line before another crossing counts
- Heading validation prevents counting wrong-direction crossings

---

## 6. Sector Timing <a name="sector-timing"></a>

Sectors use the same crossing detection as the start/finish line.

```c
#define MAX_SECTORS  8

typedef struct {
    lap_line_t  start_finish;
    lap_line_t  sectors[MAX_SECTORS];
    uint8_t     sector_count;        // 0 = no sectors, just lap timing
    char        name[32];            // e.g., "Bonneville", "Local Track"
} track_def_t;
```

Each sector boundary triggers:
- Sector split time = `t_sector_cross - t_sector_start`
- Compare vs. best sector
- Display sector delta on screen

---

## 7. Predictive Lap Delta <a name="predictive-delta"></a>

### Reference Lap

The best lap is stored as a **position-time array**:

```c
typedef struct {
    float  distance_m;    // Cumulative distance from start line
    float  elapsed_ms;    // Time since lap start
    float  lat, lon;      // For position matching
} ref_point_t;

#define MAX_REF_POINTS  2000  // 10 Hz × 200 seconds = 2000 points max
```

### Delta Calculation

For each GPS fix during the current lap:

1. Compute cumulative distance from start line
2. Find the two reference points bracketing this distance
3. Interpolate expected time: `t_ref = lerp(ref[i], ref[i+1], distance)`
4. Delta = `t_current - t_ref` (positive = behind, negative = ahead)
5. Display as `+0.5s` or `-1.2s` with color coding (green = faster)

### Distance Tracking

Cumulative distance computed using Haversine between consecutive fixes:

```c
static float haversine_m(double lat1, double lon1,
                         double lat2, double lon2)
{
    const double R = 6371000.0;
    double dlat = (lat2 - lat1) * M_PI / 180.0;
    double dlon = (lon2 - lon1) * M_PI / 180.0;
    double a = sin(dlat/2)*sin(dlat/2) +
               cos(lat1*M_PI/180)*cos(lat2*M_PI/180)*sin(dlon/2)*sin(dlon/2);
    return (float)(2.0 * R * atan2(sqrt(a), sqrt(1.0-a)));
}
```

---

## 8. Track Database <a name="track-database"></a>

### Storage Format (SD Card)

```
/sd/tracks/
  bonneville.json
  local_dragstrip.json
  road_course_1.json
```

JSON example:
```json
{
  "name": "Bonneville",
  "start_finish": {
    "lat1": 40.7580, "lon1": -113.8430,
    "lat2": 40.7582, "lon2": -113.8428,
    "heading": 270.0, "tolerance": 45.0
  },
  "sectors": [],
  "best_lap_ms": 42150,
  "reference_lap_file": "bonneville_ref.bin"
}
```

### Auto-Detection

On GPS fix, compute distance to each track's start/finish line:
- If within 500m, auto-select that track
- Display track name on screen
- Load reference lap data

---

## 9. Data Logging Integration <a name="data-logging"></a>

### Combined Log Format

Each logged row includes GPS position + all sensor data:

```csv
timestamp_ms,lap,sector,lat,lon,alt,speed_kmh,heading,hdop,rpm,boost,egt_max,throttle,lambda,...
```

At 10 Hz with ~20 fields, that's ~200 bytes/line × 10/s = 2 KB/s.
With an 8 GB SD card, that's >1000 hours of logging.

### Log File Organization

```
/sd/logs/
  2026-03-20_session_01.csv    ← GPS-timestamped filename
  2026-03-20_session_01.lap    ← Lap summary (times, deltas)
```

---

## 10. Implementation Plan <a name="implementation-plan"></a>

### Phase 1: GPS Configuration Library

Create `common/src/opendash_gps_config.c`:

| Function | Purpose |
|---|---|
| `gps_config_detect_baud()` | Auto-detect GPS module baud rate |
| `gps_config_set_rate()` | Set update rate (1/5/10/25 Hz) via UBX or PAIR |
| `gps_config_set_messages()` | Enable/disable NMEA sentences |
| `gps_config_set_dynamic()` | Set automotive dynamic model |
| `gps_config_init()` | Run full configuration sequence |

Support both **LC76G (PAIR commands)** and **u-blox (UBX binary)**.

### Phase 2: Lap Timer Core

Create `common/src/opendash_lap_timer.c`:

| Function | Purpose |
|---|---|
| `lap_timer_init()` | Load track from NVS/SD, reset state |
| `lap_timer_update()` | Called every GPS fix — crossing detection + timing |
| `lap_timer_get_current_lap()` | Returns current lap time |
| `lap_timer_get_best_lap()` | Returns best lap time (session) |
| `lap_timer_get_delta()` | Returns predictive delta |
| `lap_timer_get_sector()` | Returns current sector + split time |
| `lap_timer_set_start_finish()` | Set line from current position |

### Phase 3: Track Database

Create `common/src/opendash_track_db.c`:

| Function | Purpose |
|---|---|
| `track_db_init()` | Scan SD card for track files |
| `track_db_auto_detect()` | Match current position to nearest track |
| `track_db_save_track()` | Save new track definition |
| `track_db_load_reference()` | Load reference lap data |
| `track_db_save_reference()` | Save new best lap as reference |

### Phase 4: Display Integration

Add lap timing widgets to Center and GPS display units:

| Widget | Content |
|---|---|
| Lap counter | `LAP 3` |
| Current lap time | `1:42.350` |
| Best lap time | `BEST 1:41.200` |
| Delta | `+1.150` (red) or `-0.500` (green) |
| Sector splits | `S1: 28.4  S2: 35.1  S3: ...` |

### Phase 5: Data Logging

Extend `sd_logger` to include GPS + timing data in each row.

---

## 11. rAtTrax-BMS Integration <a name="bms-integration"></a>

The rAtTrax BMS Logger also has GPS (NMEA via UART1) and runs on ESP32.
For shared lap tracking:

1. **BMS Logger** runs identical lap detection code (same `opendash_lap_timer.c` 
   algorithms ported to Arduino/PlatformIO)
2. **BMS Logger** logs to its own SD card with identical CSV format
3. **ESP-NOW bridge** (future) sends lap events from BMS to OpenDash Center
4. Both systems can independently compute lap times from their own GPS
5. Data merge happens post-session on a PC (correlate by GPS timestamp)

See `rAtTrax_BMS_Logger/TODO.md` for the BMS-side implementation plan.

---

## 12. File Structure <a name="file-structure"></a>

### New Files to Create

```
common/
├── include/
│   ├── opendash_gps_config.h    ← GPS module configuration API
│   ├── opendash_lap_timer.h     ← Lap timing engine API
│   └── opendash_track_db.h      ← Track database API
└── src/
    ├── opendash_gps_config.c    ← UBX + PAIR command sequences
    ├── opendash_lap_timer.c     ← Crossing detection, timing, delta
    └── opendash_track_db.c      ← SD card track storage + auto-detect
```

### Key Data Point IDs (Already Defined)

```c
OPENDASH_DP_LAP_NUMBER    0x0207
OPENDASH_DP_LAP_TIME      0x0208
OPENDASH_DP_BEST_LAP_TIME 0x0209
OPENDASH_DP_LAP_DELTA     0x020A
OPENDASH_DP_SECTOR_TIME   0x020B
OPENDASH_DP_PREDICTIVE_LAP 0x020C
```

### Dependencies

- No external libraries required
- Pure C with ESP-IDF — `math.h` for trig, `cJSON` for track files
- SD card via `esp_vfs_fat` (already used for logging)
- NVS for best-lap persistence
- GPS data from existing `gps_handler.c` or new u-blox driver

---

## Appendix: UBX Binary Command Reference

### Frame Format

```
┌──────┬──────┬───────┬──────┬─────────────┬──────┬──────┐
│ SYNC1│ SYNC2│ CLASS │  ID  │   Length     │ DATA │ CK   │
│ 0xB5 │ 0x62 │  1B   │  1B  │  2B (LE)    │ N B  │ 2B   │
└──────┴──────┴───────┴──────┴─────────────┴──────┴──────┘
```

### Checksum (Fletcher-8)

```c
void ubx_checksum(const uint8_t *data, size_t len,
                  uint8_t *ck_a, uint8_t *ck_b)
{
    *ck_a = 0; *ck_b = 0;
    for (size_t i = 0; i < len; i++) {
        *ck_a += data[i];
        *ck_b += *ck_a;
    }
}
```

### Key Command Sequences

```c
// 10 Hz update rate
{0xB5,0x62, 0x06,0x08, 0x06,0x00, 0x64,0x00, 0x01,0x00, 0x01,0x00, 0x7A,0x12}

// Enable GGA on UART1
{0xB5,0x62, 0x06,0x01, 0x08,0x00, 0xF0,0x00, 0x00,0x01,0x00,0x00,0x00,0x00, 0x00,0x28}

// Enable RMC on UART1
{0xB5,0x62, 0x06,0x01, 0x08,0x00, 0xF0,0x04, 0x00,0x01,0x00,0x00,0x00,0x00, 0x04,0x2C}

// Automotive dynamic model
{0xB5,0x62, 0x06,0x24, 0x24,0x00, 0xFF,0xFF, 0x04, ...}
```

---

*OpenDash — Built for racers, by racers.*
