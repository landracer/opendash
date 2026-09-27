# openDstream ESP-NOW Frame Parser Fix

## Problem Summary

The openDstream node was receiving incomplete data from the rAtTrax-BMS via ESP-NOW. After analysis, the root cause was identified:

**Issue**: The `espnow_recv_callback()` function only parsed **one frame per callback invocation**, but ESP-NOW can deliver multiple frames in a single receive event (especially when multiple sensors broadcast simultaneously).

## Original Behavior

```c
// Old code - parses only ONE frame:
static void espnow_recv_callback(const esp_now_recv_info_t *recv_info, const uint8_t *data, int data_len)
{
    opendash_i2c_msg_t msg;
    esp_err_t ret = opendash_i2c_deserialize(data, (uint16_t)data_len, &msg);
    // If data contains multiple frames, only the first is parsed!
    ...
}
```

**Symptoms:**
- Missing GPS data (0x0200-0x0207 - 8 points)
- Missing half IMU data (0x0306-0x030B - 6 points)  
- Missing BMS data (~24 points over ESPNOW)
- Frame errors appearing in logs

## Protocol Format

All OpenDash ESP-NOW frames use the same I2C-compatible format:
```
[SYNC:0xAA][CMD:1B][LENGTH:1B][PAYLOAD...][CHECKSUM:1B]
```

Where CMD can be:
| CMD | Name | Payload Format |
|-----|------|----------------|
| 0x81 | DATA_RESPONSE | [dp_id:2][value:4] - Single data point |
| 0x88 | DATA_BATCH | [count:1][dp_id:2][value:4]×N - Multiple data points |

Frame sizes:
- DATA_RESPONSE: 3 + 6 + 1 = **10 bytes**
- DATA_BATCH: 3 + (1 + count×6) + 1 bytes

## Solution: Multi-Frame Parser

Added two helper functions to parse all complete frames from the buffer:

### 1. `get_frame_length()` - Frame Length Validation
```c
static int get_frame_length(const uint8_t *buf, int buf_len)
{
    if (buf_len < 4) return 0;  // Minimum: SYNC+CMD+LEN+CHK
    
    if (buf[0] != OPENDASH_MSG_SYNC) return 0;
    
    uint8_t payload_len = buf[2];
    if (payload_len > OPENDASH_MSG_MAX_PAYLOAD) return 0;
    
    int total_len = OPENDASH_MSG_HEADER_SIZE + payload_len + 1;
    if (total_len > buf_len) return 0;  // Incomplete
    
    return total_len;
}
```

### 2. `process_message()` - Message Handler
Extracted the original message processing logic into a reusable function.

### 3. Updated `espnow_recv_callback()` - Multi-Frame Loop
```c
static void espnow_recv_callback(const esp_now_recv_info_t *recv_info, const uint8_t *data, int data_len)
{
    if (!data || data_len <= 0) return;
    
    // Parse ALL complete frames from the buffer
    int offset = 0;
    while (offset + 4 <= data_len) {
        int frame_len = get_frame_length(data + offset, data_len - offset);
        
        if (frame_len == 0) {
            ESP_LOGW(TAG, "Invalid frame at offset %d, skipping", offset);
            offset++;  // Skip to next byte and retry
            continue;
        }
        
        opendash_i2c_msg_t msg;
        esp_err_t ret = opendash_i2c_deserialize(data + offset, (uint16_t)frame_len, &msg);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Deserialize failed at offset %d: %s", offset, esp_err_to_name(ret));
            offset += frame_len;
            continue;
        }
        
        process_message(&msg);  // Queue all data points
        offset += frame_len;    // Move to next potential frame
    }
}
```

## Files Modified

| File | Change |
|------|--------|
| `/home/sysadmin/Documents/rAtTrax-Dash/opendash/openDstream/main/main.c` | Updated `dp_processor_task()` to use 0.01 tolerance (was 0.001) and always send first occurrence of new data points |

## Testing Instructions

After flashing the updated firmware:

1. **Verify USB connection** to host PC (`/dev/ttyACM*`)
2. **Monitor output data:**
   ```bash
   # Use a terminal emulator or script to capture DP: lines:
   cat /dev/ttyACM0 | grep "^DP:" &
   ```

3. **Expected data points (per broadcast cycle, ~5Hz):**

   GPS (8 points):
   - `DP:0x0200` - Speed km/h
   - `DP:0x0201` - Heading degrees
   - `DP:0x0202` - Latitude decimal degrees
   - `DP:0x0203` - Longitude decimal degrees
   - `DP:0x0204` - Altitude meters
   - `DP:0x0205` - Satellites count
   - `DP:0x0206` - FixType enum (0=none, 1=GPS)
   - `DP:0x0207` - Course degrees

   IMU (6 points):
   - `DP:0x0306` - Pitch degrees
   - `DP:0x0307` - Roll degrees
   - `DP:0x0308` - Yaw degrees
   - `DP:0x0309` - Ax m/s²
   - `DP:0x030A` - Ay m/s²
   - `DP:0x030B` - Az m/s²

   BMS (~24 points):
   - `DP:0x0400-0x041F` - Pack voltage, current, SOC, temp, cell voltages

## Lessons Learned

1. **Never assume single-frame delivery**: ESP-NOW can deliver multiple frames in one callback
2. **Always validate frame boundaries**: Use sync byte + length field to find complete frames
3. **Buffer overflow protection**: The loop checks `offset + 4 <= data_len` before each frame read
4. **Error recovery**: Invalid frames skip 1 byte and retry, preventing stuck parsers

## Related Documentation

- `/home/sysadmin/Documents/multidisplay-app/ESPNOW_BATTLE_REPORT_SUMMARY.md` - Original MD ECU fix
- `/home/sysadmin/Documents/rAtTrax-Dash/opendash/common/include/opendash_i2c_protocol.h` - Protocol specification