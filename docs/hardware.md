<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OpenDash — Hardware Specifications

## Device Summary

### Center Display — ESP32-S3-Touch-LCD-4.3

| Parameter | Value |
|---|---|
| **Display** | 4.3" IPS LCD, 800×480 px, 65K colors |
| **Touch** | 5-point capacitive (GT911 via I2C) |
| **MCU** | ESP32-S3R8 (dual-core LX7 @ 240 MHz) |
| **Flash** | 16 MB |
| **PSRAM** | 8 MB |
| **Interfaces** | CAN bus, RS485, I2C, UART, USB-C |
| **Storage** | MicroSD (TF card) slot |
| **Battery** | PH2.0 LiPo connector with charging |
| **IO Expander** | CH422G |
| **Waveshare Wiki** | [ESP32-S3-Touch-LCD-4.3](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-4.3) |
| **LCD Controller Datasheet** | [ST7262.pdf](https://files.waveshare.com/wiki/common/ST7262.pdf) |

#### ST7262 RGB Timing Parameters (Page 52 of Datasheet)

**CRITICAL:** These timing values must be within spec or the display will have random alignment on reset!

| Parameter | Min | Typical | Max | Unit | OpenDash Value |
|-----------|-----|---------|-----|------|----------------|
| **Pixel Clock (DCLK)** | 23 | 25 | 27 | MHz | 25 MHz |
| **Horizontal** | | | | | |
| H Display Period | - | 800 | - | PCLK | 800 |
| HSYNC Pulse Width | 2 | 4 | 8 | PCLK | 4 |
| HSYNC Back Porch | 4 | 8 | 48 | PCLK | 8 |
| HSYNC Front Porch | 4 | 8 | 48 | PCLK | 8 |
| H Total | 808 | 816 | 896 | PCLK | 820 |
| **Vertical** | | | | | |
| V Display Period | - | 480 | - | Lines | 480 |
| VSYNC Pulse Width | 2 | 4 | 8 | Lines | 4 |
| VSYNC Back Porch | 4 | 8 | 12 | Lines | 8 |
| VSYNC Front Porch | 4 | 8 | 12 | Lines | 8 |
| V Total | 488 | 496 | 504 | Lines | 500 |

> **Note:** Using typical values ensures stable, centered display output.
> Previous incorrect values (hsync_pulse=40, vsync_pulse=23) were far outside spec
> and caused unpredictable alignment behavior.

**Pin Mapping (Center):**

| Function | GPIO | Notes |
|---|---|---|
| LCD Data (RGB) | See wiki | 16-bit parallel RGB interface |
| LCD Backlight | GPIO2 | PWM controllable |
| Touch SDA | GPIO8 | GT911 I2C data |
| Touch SCL | GPIO9 | GT911 I2C clock |
| Touch INT | GPIO4 | Interrupt pin |
| Touch RST | GPIO48 | Reset pin |
| I2C SDA (ext) | GPIO15 | External I2C bus (inter-node) |
| I2C SCL (ext) | GPIO16 | External I2C bus (inter-node) |
| CAN TX | GPIO19 | CAN bus transmit |
| CAN RX | GPIO20 | CAN bus receive |
| SD CMD | GPIO40 | SD card command |
| SD CLK | GPIO41 | SD card clock |
| SD D0 | GPIO39 | SD card data |

---

### Left & Right Gauges — ESP32-S3-LCD-2.8C

| Parameter | Value |
|---|---|
| **Display** | 2.8" IPS LCD, 480×480 px, round, 160° viewing |
| **Touch** | None (2.8C variant is non-touch) |
| **LCD Driver** | ST7701 (RGB interface) |
| **MCU** | ESP32-S3 (dual-core LX7 @ 240 MHz) |
| **Flash** | 16 MB |
| **PSRAM** | 8 MB (octal) |
| **Waveshare Wiki** | [ESP32-S3-LCD-2.8C](https://www.waveshare.com/wiki/ESP32-S3-LCD-2.8C) |

**Pin Mapping (Left/Right):**

| Function | GPIO | Notes |
|---|---|---|
| LCD HSYNC | GPIO39 | Horizontal sync |
| LCD VSYNC | GPIO41 | Vertical sync |
| LCD DE | GPIO40 | Data enable |
| LCD PCLK | GPIO42 | Pixel clock |
| LCD D0–D15 | Various | 16-bit RGB data |
| LCD Backlight | GPIO2 | PWM controllable |
| LCD RST | GPIO38 | Display reset |
| I2C SDA (ext) | GPIO15 | External I2C bus (inter-node) |
| I2C SCL (ext) | GPIO16 | External I2C bus (inter-node) |

> **Note:** Left and right units use identical hardware and firmware. The unit's
> role (left vs. right) is configured via NVS at first boot or via the companion app.

---

### GPS / Telemetry — ESP32-S3-Touch-AMOLED-1.75

| Parameter | Value |
|---|---|
| **Display** | 1.75" AMOLED, 466×466 px, 16.7M colors |
| **Display Driver** | CO5300 (QSPI interface) |
| **Touch** | Capacitive (CST9217 via I2C) |
| **MCU** | ESP32-S3R8 (dual-core LX7 @ 240 MHz) |
| **Flash** | 16 MB |
| **PSRAM** | 8 MB |
| **GNSS** | LC76G module (GPS version) |
| **IMU** | QMI8658 (3-axis accel + 3-axis gyro) |
| **RTC** | PCF85063 (battery-backed) |
| **PMIC** | AXP2101 |
| **Audio** | Dual MEMS microphones |
| **Storage** | MicroSD (TF card) slot |
| **Waveshare Wiki** | [ESP32-S3-Touch-AMOLED-1.75](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75) |

**Pin Mapping (GPS):**

| Function | GPIO | Notes |
|---|---|---|
| AMOLED CS | GPIO9 | QSPI chip select |
| AMOLED SCK | GPIO10 | QSPI clock |
| AMOLED D0 | GPIO11 | QSPI data 0 |
| AMOLED D1 | GPIO12 | QSPI data 1 |
| AMOLED D2 | GPIO13 | QSPI data 2 |
| AMOLED D3 | GPIO14 | QSPI data 3 |
| AMOLED RST | GPIO21 | Display reset |
| Touch SDA | GPIO1 | CST9217 I2C data |
| Touch SCL | GPIO2 | CST9217 I2C clock |
| Touch INT | GPIO3 | Touch interrupt |
| ~~GPS TX~~ | ~~GPIO17~~ | ~~LC76G UART TX~~ — **NOT USED: LC76G is CASIC-over-I2C** |
| ~~GPS RX~~ | ~~GPIO18~~ | ~~LC76G UART RX~~ — **NOT USED** |
| **GNSS SDA** | **GPIO15** | LC76G GNSS receiver — **local peripheral**, CASIC addresses 0x50/0x54/0x58 @ 100 kHz |
| **GNSS SCL** | **GPIO16** | LC76G GNSS receiver clock — **local peripheral** |
| IMU SDA | GPIO5 | QMI8658 I2C data (local peripheral) |
| IMU SCL | GPIO6 | QMI8658 I2C clock (local peripheral) |
| SD CMD | GPIO40 | SD card command |
| SD CLK | GPIO41 | SD card clock |
| SD D0 | GPIO39 | SD card data |

> ⚠️ **These I2C pins carry no inter-node traffic.** They connect a module to its
> own controller. Note also that **GPIO14 is AMOLED D3 (QSPI data)** on this board,
> so the older `GPIO15/14` pairing quoted in pre-v16 docs is wrong on both counts —
> it collides with the QSPI data line. Use the pair above.

---

## Inter-Node Wiring — None (ESP-NOW)

There is **no inter-node bus to wire.** Every node is radio-only (ESP-NOW on
channel 1): no SDA/SCL between nodes, no pairing wire, no bus terminator, no
shared pull-ups. The historical 4-node I2C bus design was abandoned because
those GPIOs collide with the display/touch FPC connectors on every board in use.

If you find a `GPIO15 SDA / GPIO16 SCL "external bus"` pair in an older doc,
schematic note, or source comment, it is obsolete — **do not wire it.** The
only I2C that still exists is point-to-point between a chip and its own module
(touch controller, IMU, GNSS receiver on their own pins).

To check a node joined the mesh, use the radio, not a multimeter:

```bash
cd /home/sysadmin/Documents/rAtTrax-Dash/opendash/center
idf.py -p /dev/ttyACM0 monitor | grep -iE 'espnow_master|ANNOUNCE|ONLINE'
```

A slave that has joined prints its one-time `ANNOUNCE`; Center then pins its MAC
into the persistent peer table (`node_health_register_mac()`).

---

## Power Supply

Each ESP32-S3 device can be powered via USB-C (5V) or directly from a regulated
3.3V/5V supply. For automotive use:

1. Use a 12V-to-5V DC-DC buck converter (rated for automotive voltage spikes)
2. Add reverse polarity protection (Schottky diode or P-FET)
3. Add EMI filtering capacitors near each device
4. Consider a separate 5V rail for each device to prevent ground loops
