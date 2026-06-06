<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# UART Connection: Multidisplay → OpenDash

Receives serial telemetry from the **multidisplay** Arduino ECU logger via UART.
Data arrives through an **HC-05** Bluetooth SPP bridge (HC-05 in master mode,
pre-configured to auto-connect to the multidisplay's HC-06 slave named `mdv2`).

> **Full protocol specification:** See `SERIAL_PROTOCOL.md` in the multidisplay
> firmware repository for the canonical, exhaustive byte-level documentation
> covering all serial modes, the receive protocol, and build variants.

---

## Hardware

### Current Pin Assignment (Waveshare ESP32-S3-LCD-2.8C)

| Signal   | GPIO        | Connector | Notes                                       |
|----------|-------------|-----------|---------------------------------------------|
| UART RX  | **GPIO 20** | J9 (DP)   | ESP32-S3 USB D+ pad, reclaimed for UART     |
| UART TX  | *N/C*       | —         | Not needed (HC-05 pre-configured externally) |
| HC-05 KEY| *N/C*       | —         | Not needed (HC-05 pre-configured externally) |

Pin configuration in [common/include/opendash_uart.h](common/include/opendash_uart.h):

```c
#define OPENDASH_UART_RX_PIN     20   /* J9 DP — USB D+ reclaimed for UART1 RX */
#define OPENDASH_UART_TX_PIN     -1   /* not connected (RX-only)                */
#define OPENDASH_HC05_KEY_PIN    -1   /* not connected (passive listen)          */
```

### GPIO Pin History and Constraints

This board has very few free GPIOs — the 16-bit RGB parallel LCD consumes most pins.

| GPIO | Status     | Why                                                    |
|------|------------|--------------------------------------------------------|
| 0    | **In use** | Boot button (screen cycle)                             |
| 1–3  | **In use** | LCD SPI MOSI, SCLK, RGB data                          |
| 4    | Free*      | Board labels "BAT ADC" — has voltage divider circuit   |
| 5–18 | **In use** | LCD RGB data, backlight, I2C, touch                    |
| 19   | **Reclaimed** | J9 DN (USB D−) — available if USB PHY disabled      |
| 20   | **Reclaimed** | J9 DP (USB D+) — **current UART RX pin**            |
| 21   | **In use** | LCD RGB data B4                                        |
| 33–37| **Reserved**| SPI flash / PSRAM (internal occupancy)                |
| 38–41| **In use** | LCD HSYNC, VSYNC, DE, PCLK                            |
| 42   | Free*      | Board labels "SD MISO / UART TXD" — not used          |
| 43   | **In use** | Console UART0 TX (log output)                          |
| 44   | **Blocked**| Console UART0 RX + CH343P UART-to-USB bridge           |
| 45–48| **In use** | LCD RGB data                                           |

> **J10 (RXD/TXD/3V3/GND) is NOT usable for UART input.** The CH343P
> UART-to-USB bridge chip permanently drives the RXD line (GPIO44), causing
> bus contention with any external signal. J10 is an alternate programming
> header only.

### Wiring (Current Setup)

HC-05 master module connected to J9 header on the 2.8C board:
- **HC-05 TX → J9 DP (GPIO 20)** — data into ESP32
- **HC-05 RX → J9 DN (GPIO 19)** — not used by firmware but wired
- **HC-05 VCC → J9 3V3**
- **HC-05 GND → J9 GND**

The USB PHY is disabled in software to release GPIO19/20 for UART use.
This means `/dev/ttyACM` (USB-Serial/JTAG) console output stops after boot.
Console logs route through UART0 (GPIO43) → CH343 → UART USB-C port instead.

**Flashing still works** via the USB port — the ROM bootloader re-enables
USB-Serial/JTAG independently of the application.

---

## Compile-Time Toggle

```c
/* In opendash_uart.h — default is 1 (enabled) */
#define OPENDASH_MULTIDISPLAY_CONNECTION  1   /* set 0 to disable entirely */
```

When disabled, `opendash_uart_init()` returns immediately and no UART or task is created.

---

## Binary Protocol (SERIALOUT_BINARY)

The multidisplay default mode is `SERIALOUT_BINARY` (mode 1). Frames are 95 bytes,
sent at 100 Hz (every 10 ms). All multi-byte values are **little-endian** (AVR ATmega).

### Frame Envelope

```
┌─────┬─────┬──────────────────────────────────────────┬─────┐
│ STX │ TAG │             93-byte payload              │ ETX │
│0x02 │0x5F │   (57 bytes MD2 + 35 bytes variant + 1)  │0x03 │
└─────┴─────┴──────────────────────────────────────────┴─────┘
  byte 0  byte 1                                        byte 94
```

- **STX** = `0x02`, **TAG** = `0x5F` (95 decimal = total frame size), **ETX** = `0x03`
- **No byte-stuffing** — use fixed frame size for synchronization
- Tag byte doubles as frame-type identifier: other tag values indicate
  ACK (4), N75 maps (22/38), gear ratios (17), etc.

### Complete Byte Map

| Offset | Size | Field            | C Type        | Scaling   | Unit / Range              |
|--------|------|------------------|---------------|-----------|---------------------------|
| 0      | 1    | STX              | —             | `0x02`    | Frame start marker        |
| 1      | 1    | TAG              | `uint8_t`     | `0x5F`    | = 95 = frame size         |
| 2      | 4    | time             | `uint32_t` LE | raw       | `millis()` since boot (ms)|
| 6      | 2    | calRPM           | `int16_t` LE  | raw       | Engine RPM (0–8500)       |
| 8      | 2    | calAbsoluteBoost | `uint16_t` LE | ÷100.0    | Boost in Bar (absolute, ~1.0 = atm) |
| 10     | 1    | calThrottle      | `uint8_t`     | raw       | Throttle 0–100 %         |
| 11     | 2    | calLambdaF       | `uint16_t` LE | ÷100.0    | Lambda ratio (1.0 = stoich) |
| 13     | 2    | calLMM           | `uint16_t` LE | ÷100.0    | Air-flow sensor (0–5 V)  |
| 15     | 2    | calCaseTemp      | `uint16_t` LE | ÷100.0    | Case temp in °C          |
| 17     | 2    | calEgt[0]        | `uint16_t` LE | raw       | EGT ch 0 in °C           |
| 19     | 2    | calEgt[1]        | `uint16_t` LE | raw       | EGT ch 1 in °C           |
| 21     | 2    | calEgt[2]        | `uint16_t` LE | raw       | EGT ch 2 in °C           |
| 23     | 2    | calEgt[3]        | `uint16_t` LE | raw       | EGT ch 3 in °C           |
| 25     | 2    | calEgt[4]        | `uint16_t` LE | raw       | EGT ch 4 in °C           |
| 27     | 2    | calEgt[5]        | `uint16_t` LE | raw       | EGT ch 5 in °C           |
| 29     | 2    | calEgt[6]        | `uint16_t` LE | raw       | EGT ch 6 (may be 0/stale)|
| 31     | 2    | calEgt[7]        | `uint16_t` LE | raw       | EGT ch 7 (may be 0/stale)|
| 33     | 2    | batVolt          | `uint16_t` LE | ÷100.0    | Battery voltage in V     |
| 35     | 2    | VDOPres1         | `int16_t` LE  | ÷10.0     | VDO pressure 1 in Bar    |
| 37     | 2    | VDOPres2         | `int16_t` LE  | ÷10.0     | VDO pressure 2 in Bar    |
| 39     | 2    | VDOPres3         | `int16_t` LE  | ÷10.0     | VDO pressure 3 in Bar    |
| 41     | 2    | VDOTemp1         | `int16_t` LE  | raw       | VDO temperature 1 in °C  |
| 43     | 2    | VDOTemp2         | `int16_t` LE  | raw       | VDO temperature 2 in °C  |
| 45     | 2    | VDOTemp3         | `int16_t` LE  | raw       | VDO temperature 3 in °C  |
| 47     | 2    | speedF           | `uint16_t` LE | ÷100.0    | Speed in km/h            |
| 49     | 1    | gear             | `uint8_t`     | raw       | Gear 1–6 (0=neutral)     |
| 50     | 1    | n75_duty         | `uint8_t`     | raw       | N75 duty cycle 0–255     |
| 51     | 2    | req_Boost        | `uint16_t` LE | ÷100.0    | Target boost setpoint Bar|
| 53     | 1    | req_Boost_PWM    | `uint8_t`     | raw       | N75 map raw PWM          |
| 54     | 1    | flags            | `uint8_t`     | bitfield  | bit0: PID on, bit1: aggressive |
| 55     | 2    | efr_speed_reading| `uint16_t` LE | special   | EFR turbo timer ticks    |
| 57     | 2    | knock            | `uint16_t` LE | raw       | Knock sensor value       |
| 59     | 35   | *variant data*   | `uint8_t[35]` | varies    | VR6: all zeros. DF: K-line data. |
| 94     | 1    | ETX              | —             | `0x03`    | Frame end marker         |

### EGT Details (8 Channels)

The binary frame **always sends 8 EGT slots** (`MAX_ATTACHED_TYPE_K = 8`),
regardless of how many are physically connected:

| Build    | Connected | Valid Channels      | Unused (0/stale)    |
|----------|-----------|---------------------|---------------------|
| VR6      | 6         | calEgt[0] – [5]     | calEgt[6], [7]      |
| Digifant | 5         | calEgt[0] – [4]     | calEgt[5] – [7]     |

Values ≥ 1200 °C indicate an open (disconnected) thermocouple.

### EFR Turbo Speed Conversion

```c
if (efr_speed_reading == 0 || efr_speed_reading == 0xFFFF)
    efr_rpm = 0;
else
    efr_rpm = 40000000 / efr_speed_reading;  /* BW_EFR_TIME_2_RPM */
```

### Scaling Summary

| Scaling | Fields                                                    |
|---------|-----------------------------------------------------------|
| ÷100.0  | boost, lambda, LMM, caseTemp, batVolt, speed, req_Boost  |
| ÷10.0   | VDOPres1, VDOPres2, VDOPres3                              |
| raw     | RPM, throttle, EGT[0–7], VDOTemp1–3, gear, n75, knock    |

---

## OpenDash Parser Implementation

### Data Struct (opendash_uart.h)

```c
typedef struct {
    uint32_t timestamp_ms;
    int16_t  rpm;
    float    boost;           /* Bar, absolute (÷100) */
    uint8_t  throttle;        /* 0–100 % */
    float    lambda;          /* ratio (÷100) */
    float    lmm;             /* Volts (÷100) */
    float    casetemp;        /* °C (÷100) */
    int16_t  egt[8];          /* °C, 8 channels */
    float    battery;         /* Volts (÷100) */
    int16_t  vdo_pres1;       /* raw ×10 */
    int16_t  vdo_pres2;
    int16_t  vdo_pres3;
    int16_t  vdo_temp1;       /* °C */
    int16_t  vdo_temp2;
    int16_t  vdo_temp3;
    float    speed;           /* km/h (÷100) */
    uint8_t  gear;
    uint8_t  n75_duty;
    float    req_boost;       /* Bar (÷100) */
    uint16_t efr_speed;       /* raw timer ticks */
    uint16_t knock;
} opendash_md_data_t;
```

### Frame Receiver (uart_rx_task)

```
1. Scan byte-by-byte until STX (0x02) found
2. Read next byte — must be TAG (0x5F). If not, go to step 1.
3. Bulk-read remaining 93 bytes (95 - STX - TAG already consumed)
4. Verify ETX at frame_buf[93-1] == 0x03
5. Call parse_binary_frame() on the 93-byte payload
```

### Offset Defines (opendash_uart.c)

```c
#define MD_FRAME_SIZE       95
#define MD_PAYLOAD_SIZE     93   /* frame minus STX and TAG */
#define MD_BINARY_TAG       0x5F /* 95 decimal */

/* Offsets within payload (0-based from TAG byte) */
#define MD_OFF_TAG          0    /* TAG byte itself */
#define MD_OFF_TIME         1    /* uint32 LE */
#define MD_OFF_RPM          5    /* int16 LE */
#define MD_OFF_BOOST        7    /* uint16 LE ÷100 */
#define MD_OFF_THROTTLE     9    /* uint8 */
#define MD_OFF_LAMBDA       10   /* uint16 LE ÷100 */
#define MD_OFF_LMM          12   /* uint16 LE ÷100 */
#define MD_OFF_CASETEMP     14   /* uint16 LE ÷100 */
#define MD_OFF_EGT0         16   /* 8×uint16 LE, 16 bytes total */
#define MD_OFF_BATVOLT      32   /* uint16 LE ÷100 */
#define MD_OFF_VDOP1        34   /* int16 LE ÷10 */
#define MD_OFF_VDOP2        36
#define MD_OFF_VDOP3        38
#define MD_OFF_VDOT1        40   /* int16 LE raw */
#define MD_OFF_VDOT2        42
#define MD_OFF_VDOT3        44
#define MD_OFF_SPEED        46   /* uint16 LE ÷100 */
#define MD_OFF_GEAR         48   /* uint8 */
#define MD_OFF_N75          49   /* uint8 */
#define MD_OFF_REQBOOST     50   /* uint16 LE ÷100 */
#define MD_OFF_REQPWM       52   /* uint8 */
#define MD_OFF_FLAGS        53   /* uint8, bit0=PID, bit1=aggressive */
#define MD_OFF_EFR          54   /* uint16 LE */
#define MD_OFF_KNOCK        56   /* uint16 LE */
```

---

## HC-05 Bluetooth Configuration

The HC-05 master module is **pre-configured externally** with AT commands
(not configured by opendash at runtime). Required configuration:

```
AT+ROLE=1                          # Master mode
AT+BIND=2017,01,117753             # Bound to mdv2's HC-06
AT+UART=115200,0,0                 # 115200 baud, 1 stop bit, no parity
AT+CMODE=0                         # Fixed address mode (connect to BIND address)
```

The HC-05 KEY pin must be LOW during operation (data mode).
On power-up, the HC-05 auto-connects to `mdv2` and transparently bridges
serial data to its UART TX output.

---

## LCD Status Display

| Status                     | Display Text                    |
|----------------------------|---------------------------------|
| `OPENDASH_UART_DISABLED`   | *(not shown)*                   |
| `OPENDASH_UART_WAITING`    | `"MD: Waiting [mdv2]..."`       |
| `OPENDASH_UART_RECEIVING`  | `"MD: mdv2 Connected"`          |
| `OPENDASH_UART_TIMEOUT`    | `"MD: mdv2 No Data"`            |

On first valid frame received, a **triple beep** is played via `display_buzzer_pattern(3, 80, 80)`.

---

## Data Flow Architecture

```
┌──────────────┐  HC-06   ┌──────┐  UART    ┌──────────────────┐
│ Multidisplay │ ──BT──→  │HC-05 │ ──RX──→  │ opendash_uart.c  │
│  (Arduino)   │  slave   │master│  GPIO20  │ parse_binary_frame│
│  mdv2        │ 115200   │      │  (J9 DP) │ (common/ component│
└──────────────┘          └──────┘          └────────┬─────────┘
                                                      │ opendash_md_data_t
                                                      │ (portMUX spinlock)
                                             ┌────────▼─────────┐
                                             │    main.c         │
                                             │ (left/main/)      │──── ESP-NOW ──→ Center
                                             │ polls get_data()  │     (forwarded)
                                             │ every 200ms       │
                                             └────────┬─────────┘
                                                      │ LVGL lock
                                             ┌────────▼─────────┐
                                             │  ui_manager.c     │
                                             │ update_value()    │
                                             │ set_status_text() │
                                             └──────────────────┘
```

- **common/** has no display dependencies — only UART + parser + data struct
- **main.c** bridges UART data to the display, and forwards key fields to
  the center display via ESP-NOW
- Thread safety: `portMUX_TYPE` spinlock protects `opendash_md_data_t` reads/writes

---

## Files

| File | Purpose |
|------|---------|
| [common/include/opendash_uart.h](common/include/opendash_uart.h) | Public API, pin config, data struct, status enum |
| [common/src/opendash_uart.c](common/src/opendash_uart.c) | UART driver, binary frame parser, HC-05 AT commands |
| [common/CMakeLists.txt](common/CMakeLists.txt) | Component registration (`esp_driver_uart`, `esp_driver_gpio`, `esp_timer`) |
| [left/main/main.c](left/main/main.c) | UART init, data polling, UI updates, ESP-NOW forwarding |
| [left/main/ui_manager.c](left/main/ui_manager.c) | Status label widget, `set_status_text()` |