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
| GPS TX | GPIO17 | LC76G UART TX |
| GPS RX | GPIO18 | LC76G UART RX |
| IMU SDA | GPIO5 | QMI8658 I2C data |
| IMU SCL | GPIO6 | QMI8658 I2C clock |
| I2C SDA (ext) | GPIO15 | External I2C bus (inter-node) |
| I2C SCL (ext) | GPIO16 | External I2C bus (inter-node) |
| SD CMD | GPIO40 | SD card command |
| SD CLK | GPIO41 | SD card clock |
| SD D0 | GPIO39 | SD card data |

---

## Wiring — Inter-Node I2C Bus

All four nodes (Center, Left, Right, GPS) connect via a shared I2C bus:

```
        3.3V ──┬──── 4.7kΩ ────┬──── 4.7kΩ ────┐
               │                │                │
              SDA              SCL              GND
               │                │                │
    ┌──────────┼────────────────┼────────────────┼──────────┐
    │  CENTER  │  GPIO15(SDA)   │  GPIO16(SCL)   │  GND     │
    ├──────────┼────────────────┼────────────────┼──────────┤
    │  LEFT    │  GPIO15(SDA)   │  GPIO16(SCL)   │  GND     │
    ├──────────┼────────────────┼────────────────┼──────────┤
    │  RIGHT   │  GPIO15(SDA)   │  GPIO16(SCL)   │  GND     │
    ├──────────┼────────────────┼────────────────┼──────────┤
    │  GPS     │  GPIO15(SDA)   │  GPIO16(SCL)   │  GND     │
    └──────────┴────────────────┴────────────────┴──────────┘
```

> **Important:** Use 4.7kΩ pull-up resistors on SDA and SCL lines.
> Only one set of pull-ups is needed per bus (not per device).

## Power Supply

Each ESP32-S3 device can be powered via USB-C (5V) or directly from a regulated
3.3V/5V supply. For automotive use:

1. Use a 12V-to-5V DC-DC buck converter (rated for automotive voltage spikes)
2. Add reverse polarity protection (Schottky diode or P-FET)
3. Add EMI filtering capacitors near each device
4. Consider a separate 5V rail for each device to prevent ground loops
