<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OpenDash — GPS / Telemetry Unit (1.75" AMOLED)

This is the GPS and Telemetry Unit project for OpenDash, running on the
Waveshare ESP32-S3-Touch-AMOLED-1.75 hardware.

> **✅ UNFROZEN / ACTIVE 2026-10-19 — OWNER SURGERY + ITERATION-8.** Owner
> disassembled the unit and ran direct wires: LC76G **TXD → GPIO18, RXD →
> GPIO17**, bypassing the one-way I2C bridge. Iteration-8 `uart_bridge.c`
> (fixed 17/18 direction, command at both bauds, reacting to the module's
> actual `$PAIR051,<ms>` echo format) then closed the loop on the bench, live
> in `opendash_gps` logs (ACM5): `UART» $PAIR051*3E` → `UART« $PAIR051,1000*13`
> → `UART» $PAIR050,100*22` → **measured GGA rate ~10.0 Hz**. FINAL TRUTH: the
> LC76G silicon always answered `$PAIR` on its native UART at 115200; only the
> module's I2C surface was read-only. The node is now a **configurable ≤10 Hz
> GNSS source**. Bench note: `Sats: 0 / NO FIX` is placement (indoors), not the
> command path. Full reversal: `wiki/GPS-LC76G-POSTMORTEM.md` §12.
>
> **Last updated:** 2026-10-19 (iteration-9 config diagnostics: `$PAIR059`=9 dB
> default, `$PAIR067` all-constellations-enabled — config fully exonerated for
> the bench unit's Sats-0; that unit additionally decoded real GNSS time, so
> its RF path is the suspect vs its own pre-mod history of 3D FIX/12 sats)
> **Production Code:** `gps_handler.c` v16k-port (v15L2 base + v16 fixes — see file header) + `uart_bridge.c` (iteration-8 command path)
> **Authoritative References:** `wiki/LC76G-I2C-GPS-Driver-Guide.md` (v2.0.0,
> read-path) + the vendored Quectel GNSS Protocol Specification PDF (command set)

## Hardware Specifications

| Component | Detail |
|-----------|--------|
| **Board** | Waveshare ESP32-S3-Touch-AMOLED-1.75 |
| **MCU** | ESP32-S3R8 dual-core LX7 @ 240 MHz |
| **Display** | 466×466 Round AMOLED (CO5300 controller, QSPI) |
| **Touch** | CST9217 capacitive touch (I2C 0x5A) |
| **GPS** | LC76G GNSS module — I2C data path (CASIC mirror) + native UART (RXD/TXD **hand-wired** by mandatory mod, see below) |
| **IMU** | QMI8658 6-axis accelerometer + gyroscope (I2C 0x6B) |
| **PMIC** | AXP2101 power management (I2C 0x34) |
| **GPIO Expander** | TCA9554 (I2C 0x20) — GPS FORCE_ON (P4), NRESET (P5) |
| **Audio Codec** | ES8311 (I2C 0x18), ES7210 ADC (I2C 0x40) |
| **RTC** | PCF85063 (I2C 0x51) |
| **Flash** | 16 MB |
| **PSRAM** | 8 MB Octal SPI |
| **I2C Bus** | GPIO15 (SDA) / GPIO14 (SCL) — **SHARED** by all I2C devices |

## GPS Communication — I2C data path + UART command path (MANDATORY MOD)

The LC76G module presents two interfaces, and this board's socket only wires
ONE of them:

1. **I2C data path (as sold, read-only).** The module's I2C surface (CASIC-style
   protocol behind a one-way bridge) only mirrors module output OUT to the
   ESP32. Commands written to 0x50/0x58 are transport-ACKed but NEVER reach the
   GNSS parser — bench-proven across iterations 5/6. This is why every pre-mod
   node "could not be configured".
2. **Native `$PAIR` UART command path — requires the mandatory mod below.**

### ⚠️ MANDATORY MODIFICATION (required for any unit running this firmware)

Waveshare did not wire the LC76G's native UART to the ESP32 (schematic marks
those module pins "NC/GPS RX|TX" = No Connect). To make the module
configurable as sold-by-spec, the owner must open the unit and run two wires:

| From | To | Net |
|---|---|---|
| LC76G module **TXD** pin | ESP32 GPIO**18** | module NMEA out → ESP RX |
| LC76G module **RXD** pin | ESP32 GPIO**17** | ESP TX → module command in |

With the wires in place, firmware `main/uart_bridge.c` (iteration-9) runs at
boot: live-detect → `$PAIR059`/`$PAIR067` config readout → (auto-fix search
mode if disabled) → `$PAIR051` query → `$PAIR051,<ms>` echo → `$PAIR050,100` →
**measured ≈10 Hz**. Checksums are computed on-device. The module answers
queries with direct echoes — it emits no `$PAIR001` ACK sentence.

Measured facts (bench, 2026-10-19): UART baud 115200 (spec default); min-SNR
mask = 9 dB default; search mode = GPS/GLO/GAL/BDS/QZSS all enabled; rate
setting **survives module power cycles** (later boots echo `$PAIR051,100*23`
before any new 050 write).

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
├── sdkconfig            # ESP-IDF configuration defaults
├── INTENSIVE_TODO.md             # Build TODO (partially archived)
├── README.md                     # This file
├── main/
    ├── CMakeLists.txt            # Main component CMake file
    ├── idf_component.yml         # Component dependencies
    ├── main.c                    # Application entry point
    ├── display_init.c/h          # Display hardware + I2C bus init
    ├── ui_manager.c/h            # LVGL UI management
    ├── gps_handler.c/h           # LC76G GPS I2C data path (v16k-port PRODUCTION)
    ├── uart_bridge.c/h           # LC76G native-UART command path (iteration-9)
    └── imu_handler.c/h           # QMI8658 IMU interface
```

## GPS Module (LC76G) Capabilities

| Parameter | Value |
|-----------|-------|
| Constellations | GPS, GLONASS, BeiDou, Galileo, QZSS |
| Accuracy | <2.5m CEP (50%) |
| Cold start | <30s |
| Hot start | <1s |
| Default update rate | 1 Hz module default (1000 ms) |
| Max update rate | **≈10 Hz ACHIEVED** via `$PAIR050,100` over the hand-wired UART command path (mandatory mod above). Rate setting survives module power cycles |
| NMEA command quirks | queries answered by direct echo (`$PAIR051,1000*13`); **no `$PAIR001` ACK sentence is emitted** |
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

1. **Check the mandatory UART mod first** — module TXD→GPIO18 / RXD→GPIO17
   wires intact? (`uart_bridge` logs `UART LIVE @ 115200` + config readouts.)
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
