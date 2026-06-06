<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OpenDash — GPS / Telemetry Unit (1.75" AMOLED)

This is the GPS and Telemetry Unit project for OpenDash, running on the
Waveshare ESP32-S3-Touch-AMOLED-1.75 hardware.

> **Production Code:** `gps_handler.c` v15L2 — verified working March 2025.
> **Authoritative Reference:** `wiki/LC76G-I2C-GPS-Driver-Guide.md` (v2.0.0)

## Hardware Specifications

| Component | Detail |
|-----------|--------|
| **Board** | Waveshare ESP32-S3-Touch-AMOLED-1.75 |
| **MCU** | ESP32-S3R8 dual-core LX7 @ 240 MHz |
| **Display** | 466×466 Round AMOLED (CO5300 controller, QSPI) |
| **Touch** | CST9217 capacitive touch (I2C 0x5A) |
| **GPS** | LC76G GNSS module — **I2C ONLY** (CASIC protocol) |
| **IMU** | QMI8658 6-axis accelerometer + gyroscope (I2C 0x6B) |
| **PMIC** | AXP2101 power management (I2C 0x34) |
| **GPIO Expander** | TCA9554 (I2C 0x20) — GPS FORCE_ON (P4), NRESET (P5) |
| **Audio Codec** | ES8311 (I2C 0x18), ES7210 ADC (I2C 0x40) |
| **RTC** | PCF85063 (I2C 0x51) |
| **Flash** | 16 MB |
| **PSRAM** | 8 MB Octal SPI |
| **I2C Bus** | GPIO15 (SDA) / GPIO14 (SCL) — **SHARED** by all I2C devices |

## GPS Communication — I2C (NOT UART)

The LC76G communicates via the **CASIC I2C protocol** on a shared bus.
**UART is NOT functional** on this board — GPIO17/18 are I2C routed only.

| Address | Direction | Handle | Purpose |
|---------|-----------|--------|---------|
| **0x50** | Write (TX) | `lc76g_handle` | CASIC command/query endpoint |
| **0x54** | Read (RX) | `lc76g_read_handle` | NMEA data + avail response |
| **0x58** | Write | `lc76g_dwr_handle` | Data write / WAKE bus activity |

**Clock:** 100 kHz (required — 400 kHz causes NACK)

### Key Mechanisms (v15L2)

1. **PMIC Power Cycle** — 5s OFF, 500ms ON, 5s boot wait
2. **I2C WAKE** — CW config write(0x50) + dummy write(0x58) + 200ms delay
3. **Activation** — TxRx on 0x50 re-registers the 0x54 slave address
4. **Primer** — TxRx + data_req(0x2000) + drain read at boot AND recovery
5. **Per-Read WAKE** — CW+0x58 after every successful data read
6. **Re-Prime + Drain** — data_req + 256-byte drain on every 5th RX fail

See `wiki/LC76G-I2C-GPS-Driver-Guide.md` §5 for full details.

## Building & Flashing

### Prerequisites

1. **ESP-IDF v6.1-dev** (v6.1-dev-2441 or later)
2. **Visual Studio Code** with ESP-IDF extension (recommended)
3. **USB-C cable** — device appears as `/dev/ttyACM0` (USB-CDC)

### Command Line Build

```bash
cd gps/
source ~/esp/v5.4/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

> **Note:** Use `esptool.py` reset if flash fails. The board uses USB-CDC,
> not a UART bridge.

### Visual Studio Code Build

1. Open this folder in VS Code
2. Press **F1** → **ESP-IDF: Set Espressif device target** → **ESP32-S3**
3. Press **F1** → **ESP-IDF: Build your project**
4. Press **F1** → **ESP-IDF: Flash your project**
5. Press **F1** → **ESP-IDF: Monitor device**

## Project Structure

```
gps/
├── CMakeLists.txt                # Main project CMake file
├── sdkconfig.defaults            # ESP-IDF configuration defaults
├── INTENSIVE_TODO.md             # Build TODO (partially archived)
├── README.md                     # This file
└── main/
    ├── CMakeLists.txt            # Main component CMake file
    ├── idf_component.yml         # Component dependencies
    ├── main.c                    # Application entry point
    ├── display_init.c/h          # Display hardware + I2C bus init
    ├── ui_manager.c/h            # LVGL UI management
    ├── gps_handler.c/h           # LC76G GPS I2C driver (v15L2 PRODUCTION)
    └── imu_handler.c/h           # QMI8658 IMU interface
```

## GPS Module (LC76G) Capabilities

| Parameter | Value |
|-----------|-------|
| Constellations | GPS, GLONASS, BeiDou, Galileo, QZSS |
| Accuracy | <2.5m CEP (50%) |
| Cold start | <30s |
| Hot start | <1s |
| Default update rate | 1 Hz |
| Max update rate | **10 Hz** (via `$PAIR050,100` command) |
| I2C buffer | ~4096 bytes |
| Protocol | CASIC I2C (0x50/0x54/0x58) |
| Firmware | LC76GABNR12A03S (2024/04/14) |
| NMEA output | GGA, RMC, GSV, GSA, GLL, VTG, TXT, PQTM |

### Production Performance (v15L2)

| Metric | Value |
|--------|-------|
| Continuous data flow | 100–125 seconds per burst |
| Data per 5-min run | 62,000–95,000 bytes |
| NMEA sentences per run | 1,100+ |
| Satellites tracked | 9–16 |
| HDOP | 0.7–1.1 |
| Fix type | 3D |
| Recovery mechanism | Automatic PMIC power cycle + primer |

## IMU Sensor (QMI8658)

| Parameter | Value |
|-----------|-------|
| Accelerometer | ±2g / ±4g / ±8g / ±16g |
| Gyroscope | ±16 to ±2048 °/s |
| Max sample rate | 1 kHz |
| I2C address | 0x6B |
| WHO_AM_I | 0x05 |

## Troubleshooting

### GPS not getting data

1. **NOT UART** — This board uses I2C CASIC protocol, not UART.
2. Check PMIC power rails: ALDO3 + ALDO4 + BLDO2 (bitmask 0x2C on register 0x90)
3. Check TCA9554: P4 (FORCE_ON) HIGH, P5 (NRESET) HIGH after boot
4. Ensure WAKE sequence includes both CW(0x50) AND dummy(0x58) writes
5. Ensure primer runs at boot: TxRx + data_req + drain read
6. Wait 5 seconds after power cycle before probing

### Bus degradation after 100–125s

This is a **known hardware limitation** of the shared I2C bus. Touch, IMU,
display, and GPS all share GPIO15/14. The recovery mechanism (PMIC power
cycle + primer) automatically restores data flow.

### [2C2C2C2C] avail response

data_req was sent without drain read. The NMEA content pollutes the avail
query response register. Fix: always drain-read 256 bytes after data_req.

### [4D4D4D4D] avail response

CW write was placed in the RX fail path, poisoning the module's response
register with command bytes. Fix: NEVER put CW writes in the RX fail path.

## License

See main repository README for license information.

---

**OpenDash GPS / Telemetry Unit** — Built for racers, by racers.
