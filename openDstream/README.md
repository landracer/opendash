# Licensed under Sovereign Individual License v1.0 — see LICENSE file
# ─────────────────────────────────────────────────────────────────────────────
# OpenDash openDstream — ESP-NOW → UART Data-Point Bridge
# ─────────────────────────────────────────────────────────────────────────────

## openDstream — ESP-NOW to UART Bridge Node

### Purpose
Bridge the OpenDash ESP-NOW network to a host PC. The node receives OpenDash
frames from the network (today: the rAtTrax BMS Logger's broadcasts, plus any
other node traffic on the channel), extracts each data point, and prints one
line per DP over UART to the host (multidisplay-app Qt application).

### Hardware
- **MCU**: ESP32-WROOM-32 (classic) — **not** an S3; no native USB.
- **Host link**: onboard USB-to-UART bridge (CP2102/CH340 class) on UART0.
- **UART**: UART0, TX = GPIO1, RX = GPIO3, **921600 baud** 8N1.

### ESP-NOW Protocol
- **Role**: ESP-NOW listener (no peer table required — frames are broadcast)
- **Channel**: locked to **channel 1** — matches the BMS Logger (`main.cpp`
  in `rAtTrax_BMS_Logger` pins the same channel).
- **Frame format** (identical to the fleet protocol, see
  `common/include/opendash_protocol.h`):

  `[SYNC 0xAA][CMD 1B][LEN 1B][payload…][XOR checksum 1B]`

- **Payload** (as sent by the BMS Logger, one DP per frame, cmd `0x81
  DATA_RESPONSE`): `[dp_id_hi][dp_id_lo][float32 LE]` → 10-byte frame.
- Checksum = XOR of SYNC, CMD, LEN and all payload bytes (verified against
  `buildOpenDashFrame()` in the BMS Logger source).

### UART Output Format
One newline-delimited ASCII line per data point:
```
DP:0x0401:42.10\n
```
`DP:` + `0x` + 4-hex DP id + `:` + value with 2 decimals.

### Build & Flash
```bash
cd openDstream
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

### Known Limitations (current baseline)
- The payload walk assumes one DP tuple per frame at offset 3 (matches the
  BMS Logger's single-DP `0x81` frames). `DATA_BATCH (0x88)` frames with a
  leading count byte are **not** decoded yet — see TODO §5 (single spine parser).
- One frame per receive callback is parsed; that is correct for one-frame
  broadcasts (how the BMS Logger sends) but does not accumulate pipelined
  frames that arrive while the radio task is busy.
- No TX path (receive-only bridge), no re-broadcast.

### Dependencies
- ESP-IDF v6.1+ (matches the fleet toolchain)
- `common` component listed in the project `REQUIRES` for future use — the
  current `main.c` parses frames standalone (see Known Limitations)

### Integration
Wire the module's UART0 header (GPIO1 TX / GPIO3 RX) to the host USB-UART
bridge; the host-side app (multidisplay-app) parses `DP:0x…:v.vv` lines and
maps hex IDs to telemetry channels.
