<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OpenDash Agent Instructions

## Absolute Prohibitions
1. **NEVER delete, truncate, overwrite, or recreate any existing file.**
2. **NEVER remove working code** from the codebase without explicit user consent (a signed-off request naming the specific function or block).
3. **NEVER mark a TODO item `[x]`** unless the feature compiles, flashes, and runs.
4. **NEVER use Arduino framework APIs** in OpenDash code. This is ESP-IDF + CMake.
5. **NEVER modify `sdkconfig.defaults`** for WiFi, Bluetooth, or PSRAM settings without explaining impact.
6. **NEVER change the ESP-NOW protocol frame format** — it must match SYNC(0xAA)+CMD+LEN+PAYLOAD+CHECKSUM across ALL nodes.

## Code Style
- C99 for ESP-IDF components, no C++ in main firmware
- `opendash_` prefix for all public functions and types in common/
- Logging via `ESP_LOGI()`, `ESP_LOGW()`, `ESP_LOGE()` with file-specific TAG
- FreeRTOS tasks for concurrent operations with proper stack sizes
- No dynamic memory allocation in hot paths — use static buffers for ESP-NOW messages
- LVGL operations ONLY from the LVGL task (thread safety)

## Build & Flash Commands
```bash
# Per-node (center, left, right, gps):
cd /home/sysadmin/Documents/rAtTrax-Dash/opendash/{node}
idf.py set-target esp32s3     # First time only
idf.py build                  # Compile
idf.py -p /dev/ttyACM0 flash  # Flash only
idf.py -p /dev/ttyACM0 monitor  # Serial monitor (Ctrl+] to exit)
idf.py -p /dev/ttyACM0 flash monitor  # Flash + monitor combo
```

## ESP-NOW Protocol Rules
### Frame Format (MUST match opendash_i2c_protocol.h)
```
| SYNC 0xAA | CMD 1B | LENGTH 1B | PAYLOAD 0-248B | CHECKSUM 1B |
Checksum = XOR(SYNC, CMD, LENGTH, PAYLOAD[0], ..., PAYLOAD[n-1])
```

### Command Directions
| From | To | CMD | Purpose |
|------|----|-----|---------|
| Center | Nodes | SET_DATA_POINT (0x01) | Push display values |
| Center | Nodes | SYSTEM/PING (0x07+0x04) | Node discovery |
| Nodes | Center | DATA_RESPONSE (0x81) | Push sensor readings |
| Nodes | Center | STATUS_REPORT (0x82) | Respond to PING |

### API Stack (for node→center messages)
1. Build payload bytes: `[dp_id_hi][dp_id_lo][float_byte0...float_byte3]` = 6 bytes
2. Frame with: `opendash_i2c_build_msg(&msg, OPENDASH_CMD_DATA_RESPONSE, payload, 6)`
3. Serialize to wire: `opendash_i2c_serialize(&msg, buf, sizeof(buf), &len)`
4. Transmit: `opendash_espnow_send(peer, buf, len)` or `opendash_espnow_broadcast(buf, len)`

### Reference Implementation
GPS node `gps/main/main.c` function `send_data_point()` lines ~91-110 is the canonical example of a peripheral node sending data to center.

## Data Point IDs
See `common/include/opendash_data_model.h` for the canonical ID list.
See `docs/data-points.md` for human-readable legend.

### ID Ranges
- 0x0001–0x00FF: MultiDisplay/ECU (RPM, boost, EGT, temps, pressures)
- 0x0200–0x02FF: GPS (speed, heading, lat, lon, satellites)
- 0x0300–0x03FF: IMU (G-forces, pitch, roll, yaw)
- 0x0400–0x04FF: rAtTrax BMS (pack V/I, SOC, cells, temp, power)
- 0x0500–0x05FF: System (CPU temp, heap, RSSI, uptime)
- 0x0600–0x06FF: VESC (eRPM, current, duty, temps, fault)
- 0x0620–0x0624: Wheel RPM (4 channels + avg speed)

## Common Library
`common/include/` headers are included by ALL nodes. Key files:
- `opendash_i2c_protocol.h` — Frame builder, serializer, deserializer, checksums
- `opendash_espnow.h` — ESP-NOW init, send, broadcast, receive callbacks
- `opendash_data_model.h` — Data point ID enum and lookup
- `opendash_common.h` — Node types, error codes, shared macros
- `opendash_display_config.h` — Per-node display resolution/orientation config

## Multi-Node Awareness
When editing ANY file, consider:
1. Does this affect the common/ library? → Impacts ALL 4+ nodes
2. Does this change message format? → Must update sender AND receiver
3. Does this change data point IDs? → Must update data_model AND all consumers
4. Does this affect WiFi/ESP-NOW channel? → Must match ALL nodes AND rAtTrax-BMS

## rAtTrax-BMS Cross-Project Notes
- BMS Logger is a SEPARATE project in `/home/sysadmin/Documents/PlatformIO/Projects/rAtTrax_BMS_Logger`
- BMS uses **Arduino framework on PlatformIO**, NOT ESP-IDF
- BMS must reimplement the protocol framing in Arduino C++ (no access to common/ headers)
- BMS broadcasts on WiFi channel 1 — this CANNOT diverge
- Center auto-discovers BMS when it responds to PING with STATUS_REPORT
- Center transparently forwards ALL properly-framed DATA_RESPONSE data points — no BMS-specific code needed in center

## Adding a New Data Point
1. Define ID in `opendash_data_model.h` with appropriate range
2. Add to `docs/data-points.md` legend
3. Sender: build payload + frame + serialize + send (see GPS reference)
4. Receiver: `dispatch_message()` → `ui_manager_update_value(id, value)` already passes through all IDs
5. UI: Add gauge/display widget bound to the new ID in `ui_manager.c`

## Editing Checklist (Before Every Change)
- [ ] Read the file(s) I'm about to modify
- [ ] Understand the current implementation
- [ ] Check if change touches common/ (impacts all nodes)
- [ ] Check if change affects ESP-NOW messages (protocol compatibility)
- [ ] Make minimal, targeted edit
- [ ] Build with `idf.py build`
- [ ] Update TODO.md accurately (never mark untested work as done)
