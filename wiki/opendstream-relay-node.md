# openDstream — ESP-NOW to USB Serial/JTAG Relay Node

## Overview

**openDstream** is a headless ESP32-S3 relay node that bridges wireless ESP-NOW telemetry data to a PC running [multidisplay-app](/home/sysadmin/Documents/multidisplay-app/multidisplay-app/) via native USB-OTG.

### Purpose

- Receives all ESP-NOW frames from the OpenDash wireless network
- Pipes raw frames out via USB Serial/JTAG to multidisplay-app on PC
- Enables real-time telemetry visualization without display hardware
- Acts as a data bridge between vehicle sensors and desktop monitoring software

## Hardware Requirements

- **ESP32-S3 development board** with native USB-OTG (e.g., ESP32-S3-DevKitC-1)
- **USB-C cable** for both power and data connection to PC
- No external display, no LVGL, no peripherals — pure relay

## Architecture

```
┌─────────────┐     ESP-NOW      ┌──────────────┐     USB Serial      ┌─────────────────┐
│  Center     │ ◄──────────────► │ openDstream  │ ◄─────────────────► │ multidisplay-   │
│  Display    │   Wireless Bus   │  (Relay Node)│    JTAG/USB-OTG     │    app (PC)     │
│  (Master)   │                  │              │                       │                 │
└─────────────┘                  └──────────────┘                       └─────────────────┘
```

## Data Flow

1. **ESP-NOW Reception**: openDstream listens on WiFi channel 6 as an ESP-NOW slave
2. **Frame Forwarding**: Every received frame is piped directly to USB Serial/JTAG
3. **PC Interface**: multidisplay-app reads the USB serial stream and displays telemetry

## Project Structure

```
openDstream/
├── CMakeLists.txt              # Project configuration (standalone, no common component)
├── sdkconfig.defaults          # ESP-IDF defaults for ESP32-S3
├── partitions.csv              # Partition table
├── main/
│   ├── CMakeLists.txt          # Component config
│   └── main.c                  # Relay implementation
└── build/                      # Build output
    └── openDstream.bin         # Flash binary
```

## Key Implementation Details

### USB Serial/JTAG (ESP-IDF v6.x)

Uses the high-level driver API:
- `usb_serial_jtag_driver_install()` — Initialize USB Serial/JTAG
- `usb_serial_jtag_write_bytes()` — Write data to USB (blocking with timeout)
- No display, no UART pins needed — uses native USB-OTG

### ESP-NOW Configuration

- **Mode**: ESP-NOW slave (receives from center master)
- **WiFi Channel**: 6
- **Callback**: `espnow_recv_cb()` pipes all frames to USB
- **No peer registration needed** — receives from any sender

### Status LED

- GPIO8 (active-low) on most devkits
- Double-blink pattern indicates alive status
- No other peripherals required

## Building

```bash
cd openDstream
source /home/sysadmin/Documents/esp-ide/esp-idf/export.sh
idf.py build
```

Output: `build/openDstream.bin` (~267KB)

## Flashing

```bash
# Connect ESP32-S3 via USB-C to PC
idf.py -p /dev/ttyACM0 flash monitor
```

Replace `/dev/ttyACM0` with the actual USB Serial/JTAG port.

## Usage with multidisplay-app

1. Flash openDstream to ESP32-S3
2. Connect ESP32-S3 to PC via USB-C
3. Start multidisplay-app on PC
4. App should detect USB serial stream and display telemetry

The relay passes through all ESP-NOW frame types:
- `MSG_TYPE_SERIALOUT_BATCH` — Batched telemetry
- `MSG_TYPE_SERIALOUT_BINARY` — Single binary frames
- `MSG_TYPE_STATUS_REPORT` — Node status reports
- All other message types (passthrough)

## Configuration

### WiFi Channel

Edit `main/main.c`:
```c
#define ESPNOW_CHANNEL      6  // Change to match your network
```

### Status LED Pin

Edit `main/main.c`:
```c
#define STATUS_LED_GPIO     GPIO_NUM_8  // Adjust for your board
```

## Troubleshooting

### USB Not Detected

- Ensure ESP32-S3 has native USB (not USB-to-UART adapter)
- Check USB cable supports data (not charge-only)
- Verify `CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED=y` in sdkconfig

### No Data on USB

- Check ESP-NOW network is active (center master running)
- Verify WiFi channel matches between nodes
- Monitor ESP32 serial output for debug messages

### Build Errors

- Ensure ESP-IDF v6.1 environment is activated
- Run `idf.py fullclean` before rebuilding
- Check `esp_driver_usb_serial_jtag` and `esp_driver_gpio` are in REQUIRES

## Future Enhancements

- [ ] Bidirectional relay (PC → ESP-NOW)
- [ ] TinyUSB CDC-ACM for higher throughput
- [ ] Configurable filtering by node ID or message type
- [ ] Frame buffering for burst handling
- [ ] Web interface for configuration

## License

Sovereign Individual License v1.0 — see [LICENSE](../../LICENSE) file
