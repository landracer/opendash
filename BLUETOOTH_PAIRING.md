<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# Bluetooth Pairing Guide — HC-05 Master ↔ HC-06 Slave

> End-user guide for setting up the Bluetooth link between the MultiDisplay
> sensor package (HC-06 slave) and the OpenDash left gauge pod (HC-05 master).
>
> **Audience:** Anyone wiring up a new OpenDash system or replacing a
> Bluetooth module. You need a USB-to-serial adapter (FTDI, CH340, CP2102)
> and a terminal program (Arduino Serial Monitor, PuTTY, minicom, etc.).

---

## Overview

OpenDash receives sensor data from MultiDisplay over a Bluetooth serial
link. Two modules are used:

| Role | Module | Location | What it does |
|---|---|---|---|
| **Slave** | HC-06 | On the MultiDisplay board | Receives serial data from MD's Serial2, broadcasts over Bluetooth |
| **Master** | HC-05 | On the OpenDash left gauge pod | Connects to HC-06 automatically, forwards data to ESP32-S3 UART |

The HC-05 (master) automatically connects to the HC-06 (slave) every time
both are powered on. No phone or computer is needed once pairing is
configured — it's a permanent headless link.

---

## Part 1: Configure the HC-06 Slave (on MultiDisplay)

The HC-06 only accepts AT commands when **not connected** to anything.
Power it up without the HC-05 nearby (or HC-05 powered off).

### Wiring for Configuration

| HC-06 Pin | USB-Serial Adapter |
|---|---|
| VCC | 5V (or 3.3V if module supports it) |
| GND | GND |
| TXD | RXD |
| RXD | TXD |

### AT Commands

Open your serial terminal at **9600 baud** (HC-06 default) with **no line
ending** (HC-06 does NOT expect `\r\n`).

```
AT                  → responds "OK"
AT+NAMEmdv2         → responds "OKsetname" — sets Bluetooth name to "mdv2"
AT+PIN1234          → responds "OKsetPIN" — sets pairing PIN to "1234"
AT+BAUD8            → responds "OK115200" — sets baud to 115200
```

> **Baud codes:** 1=1200, 2=2400, 3=4800, 4=9600, 5=19200, 6=38400,
> 7=57600, **8=115200**

After setting baud to 115200, close and reopen your terminal at 115200 baud.
Verify with `AT` → should respond `OK`.

### HC-06 Notes

- HC-06 can only be a **slave** — it cannot initiate connections
- AT commands are sent **without** CR/LF line endings
- AT command response has **no** CR/LF either
- The LED blinks rapidly when waiting for connection, solid when connected
- Default pairing PIN is usually `1234` (set it explicitly to be sure)

---

## Part 2: Configure the HC-05 Master (on OpenDash)

The HC-05 has two modes:
- **Data mode** — normal serial forwarding (LED blinks fast ~5Hz)
- **AT command mode** — configuration (LED blinks slow ~1Hz, 2 seconds on/off)

### Entering AT Command Mode

**Method 1 — KEY/EN pin (recommended):**
1. Wire the HC-05 KEY (or EN) pin to 3.3V
2. Power on the HC-05
3. LED blinks slowly (~2s on / 2s off) = you're in AT mode

**Method 2 — Hold button at power-on:**
1. Hold the small button on the HC-05 module
2. Apply power while holding the button
3. Release after LED starts slow blink

### Wiring for Configuration

| HC-05 Pin | USB-Serial Adapter |
|---|---|
| VCC | 5V |
| GND | GND |
| TXD | RXD |
| RXD | TXD |
| KEY/EN | 3.3V (for AT mode) |

### AT Commands

Open your serial terminal at **38400 baud** (HC-05 AT mode always uses
38400) with **CR+LF** line ending (`\r\n`).

```
AT                          → responds "OK"
AT+ROLE=1                   → responds "OK" — sets to MASTER mode
AT+CMODE=0                  → responds "OK" — connect to fixed address only
AT+BIND=2017,01,117753      → responds "OK" — bind to HC-06's address
AT+UART=115200,0,0          → responds "OK" — 115200 baud, 1 stop, no parity
AT+PSWD="1234"              → responds "OK" — must match HC-06 PIN
AT+RESET                    → responds "OK" — reboot into data mode
```

### Finding Your HC-06's Bluetooth Address

If you don't know the HC-06 address, you can discover it:

```
AT+ROLE=1                   → set master first
AT+RESET                    → reboot
AT+CMODE=1                  → allow any address (temporary)
AT+INQM=0,5,9              → inquiry: standard mode, max 5 devices, 9s timeout
AT+INIT                     → initialize SPP profile
AT+INQ                      → scan... will list found devices:
```

Response looks like:
```
+INQ:2017:1:117753,1F00,7FFF
+INQ:AABB:CC:DDEEFF,1F00,7FFF
```

Find the one matching your HC-06 name:
```
AT+RNAME?2017,01,117753     → responds "+RNAME:mdv2" — that's our HC-06!
```

Then bind to it:
```
AT+CMODE=0                  → back to fixed address mode
AT+BIND=2017,01,117753      → bind to discovered address
AT+RESET                    → save and reboot
```

### HC-05 Notes

- HC-05 AT mode always uses **38400 baud** regardless of data mode baud rate
- AT commands require **CR+LF** (`\r\n`) line endings
- In master mode, HC-05 automatically connects to the bound address on power-up
- LED pattern: rapid blinking = searching/connecting, double-blink = connected
- The `AT+BIND` address format uses **commas** not colons: `2017,01,117753`

---

## Part 3: Wiring to OpenDash (Final Installation)

Once both modules are configured, wire the HC-05 to the Waveshare
ESP32-S3-LCD-2.8C (left gauge pod):

| HC-05 Pin | Connects To | Notes |
|---|---|---|
| VCC | 5V rail | From vehicle power or USB supply |
| GND | Board GND | Common ground with ESP32-S3 |
| TXD | **GPIO20** (J9 DP) | HC-05 data output → ESP32 UART1 RX |
| RXD | **GPIO19** (J9 DN) | Optional — not used (TX disabled in firmware) |
| KEY/EN | **Not connected** | Leave floating for data mode |

### J9 Header Pinout (Waveshare ESP32-S3-LCD-2.8C)

```
  J9 Header (3 pins):
  ┌─────┬─────┬─────┐
  │ GP0 │  DN │  DP │
  │     │GP19 │GP20 │
  └─────┴─────┴─────┘
         RXD   TXD
       (to HC-05) (from HC-05)
```

> **Important:** GPIO19 and GPIO20 are normally USB D-/D+ pins. The OpenDash
> firmware releases them from the USB-Serial/JTAG peripheral at boot.
> Flashing via USB still works — the ROM bootloader re-enables USB
> automatically.

---

## Part 4: Verification

### Check Connection

1. Power on MultiDisplay (HC-06 LED should blink rapidly)
2. Power on OpenDash left pod (HC-05 LED should blink rapidly, then go
   steady or double-blink when connected)
3. Monitor the ESP32-S3 serial console:

```bash
idf.py -p /dev/ttyACM1 monitor
```

You should see:
```
UART diag: 9500 bytes, 100 STX, status=RECEIVING
```

If you see `status=WAITING` and `0 bytes`, the Bluetooth link isn't active.

### Quick Test Without OpenDash

To verify the Bluetooth link independently, use a USB-serial adapter
connected to the HC-05 TXD pin:

1. Open terminal at **115200 baud**
2. Power on MultiDisplay
3. You should see raw binary data streaming (not human-readable)
4. Use a hex viewer — look for `0x02 0x5F` frame starts

Alternatively, use a Python script:
```python
import serial
ser = serial.Serial('/dev/ttyUSB0', 115200)
while True:
    b = ser.read(1)
    if b == b'\x02':  # STX
        tag = ser.read(1)
        if tag == b'\x5f':  # TAG = 95
            frame = ser.read(93)
            if frame[-1] == 0x03:  # ETX
                rpm = int.from_bytes(frame[0:2], 'little')
                print(f"RPM: {rpm}")
```

---

## Troubleshooting

### HC-05 won't enter AT mode
- Ensure KEY/EN pin is HIGH (3.3V) **before** powering on
- Try the button-hold method as an alternative
- Verify you're using 38400 baud with CR+LF

### HC-05 won't connect to HC-06
- Verify addresses match: `AT+BIND?` should show the HC-06 address
- Verify PINs match: `AT+PSWD?` on HC-05, should be `"1234"` (or whatever
  you set on HC-06)
- Verify HC-05 is master: `AT+ROLE?` → `+ROLE:1`
- Power cycle both modules (HC-06 first, then HC-05)

### Connected but no data in OpenDash
- Verify baud rates match: both must be 115200
- Check wiring: HC-05 TXD → GPIO20, not GPIO44
- Check firmware: `OPENDASH_UART_RX_PIN` must be 20 in `opendash_uart.h`
- Run with `OPENDASH_UART_DEBUG=1` to see per-frame logging

### Garbled data / wrong baud
- HC-06 baud change is immediate — close terminal, reopen at new baud
- HC-05 data mode baud: `AT+UART?` should show `+UART:115200,0,0`
- If you changed HC-06 baud and forgot what it is, try common rates:
  9600, 38400, 57600, 115200

---

## Summary of AT Commands

### HC-06 (Slave) — 9600 baud, no line ending

| Command | Response | Purpose |
|---|---|---|
| `AT` | `OK` | Test |
| `AT+NAMEmdv2` | `OKsetname` | Set name |
| `AT+PIN1234` | `OKsetPIN` | Set pairing PIN |
| `AT+BAUD8` | `OK115200` | Set 115200 baud |

### HC-05 (Master) — 38400 baud, CR+LF line ending

| Command | Response | Purpose |
|---|---|---|
| `AT` | `OK` | Test |
| `AT+ROLE=1` | `OK` | Set master mode |
| `AT+CMODE=0` | `OK` | Fixed address only |
| `AT+BIND=addr` | `OK` | Bind to HC-06 address |
| `AT+UART=115200,0,0` | `OK` | Set data baud rate |
| `AT+PSWD="1234"` | `OK` | Set PIN (must match HC-06) |
| `AT+RESET` | `OK` | Reboot |
| `AT+INQ` | `+INQ:...` | Scan for devices |
| `AT+RNAME?addr` | `+RNAME:...` | Get device name |

---

*OpenDash — Built for racers, by racers.*
