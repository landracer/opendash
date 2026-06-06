<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# VESC Integration with OpenDash

This document describes the VESC CAN protocol integration for OpenDash, based on the vesc_express analysis.

## Overview

OpenDash supports VESC telemetry data through CAN bus communication. The integration includes parsing of standard VESC CAN messages and mapping them to OpenDash data point IDs.

## VESC CAN Protocol

### Standard VESC CAN Message Format

VESC uses 29-bit extended CAN IDs in the format:
```
ID = controller_id | (CAN_PACKET_TYPE << 8)
```

Where:
- `controller_id` is 8-bit (0-255)
- `CAN_PACKET_TYPE` is 8-bit (0-255)
- The full 29-bit ID is used for message identification

### CAN Message Types (STATUS 1-6)

#### STATUS 1 (Type 9)
- **ID**: `controller_id | 0x0900` (e.g., 0x0109 for controller 1)
- **Fields**:
  - eRPM (int32) - Motor revolutions per minute
  - current (int16) - Motor current × 100
  - duty (int16) - Duty cycle × 1000

#### STATUS 2 (Type 14)
- **ID**: `controller_id | 0x0E00` (e.g., 0x010E for controller 1)
- **Fields**:
  - Ah consumed (int32) - Amp-hours consumed × 10000
  - Ah charged (int32) - Amp-hours charged × 10000

#### STATUS 3 (Type 15)
- **ID**: `controller_id | 0x0F00` (e.g., 0x010F for controller 1)
- **Fields**:
  - Wh consumed (int32) - Watt-hours consumed × 10000
  - Wh charged (int32) - Watt-hours charged × 10000

#### STATUS 4 (Type 16)
- **ID**: `controller_id | 0x1000` (e.g., 0x0110 for controller 1)
- **Fields**:
  - FET temp (int16) - FET temperature × 10
  - motor temp (int16) - Motor temperature × 10
  - current_in (int16) - Input current × 100
  - PID pos (int16) - PID position × 50

#### STATUS 5 (Type 27)
- **ID**: `controller_id | 0x1B00` (e.g., 0x011B for controller 1)
- **Fields**:
  - tacho (int32) - Tacho value
  - V_in (int16) - Input voltage × 10

#### STATUS 6 (Type 58)
- **ID**: `controller_id | 0x3A00` (e.g., 0x013A for controller 1)
- **Fields**:
  - ADC1 (int16) - ADC channel 1 × 1000
  - ADC2 (int16) - ADC channel 2 × 1000
  - ADC3 (int16) - ADC channel 3 × 1000
  - PPM (int16) - PPM value × 1000

## Data Point Mapping

### VESC Data Points (0x0600-0x06FF)

| OpenDash ID | VESC CAN Type | Description |
|-------------|---------------|-------------|
| 0x0600 | STATUS 1 eRPM | Motor revolutions per minute |
| 0x0601 | STATUS 1 current | Motor current |
| 0x0602 | STATUS 1 duty | Motor duty cycle |
| 0x0603 | STATUS 2 Ah | Ah consumed |
| 0x0604 | STATUS 2 Ah charged | Ah charged |
| 0x0605 | STATUS 3 Wh | Wh consumed |
| 0x0606 | STATUS 3 Wh charged | Wh charged |
| 0x0607 | STATUS 4 FET temp | FET temperature |
| 0x0608 | STATUS 4 motor temp | Motor temperature |
| 0x0609 | STATUS 4 current_in | Input current |
| 0x060A | STATUS 4 PID pos | PID position |
| 0x060B | STATUS 5 tacho | Tacho value |
| 0x060C | STATUS 5 V_in | Input voltage |
| 0x060D | STATUS 6 ADC1 | ADC channel 1 |
| 0x060E | STATUS 6 ADC2 | ADC channel 2 |
| 0x060F | STATUS 6 ADC3 | ADC channel 3 |
| 0x0610 | STATUS 6 PPM | PPM value |
| 0x0611 | STATUS 1 RPM | RPM (derived from eRPM) |
| 0x0612 | STATUS 1 power_in | Input power (V_in × I_in) |
| 0x0613 | STATUS 1 power_motor | Motor power |
| 0x0614 | STATUS 1 fault | Fault code |

## Integration Implementation

### CAN Bus Configuration

1. **Baud Rate**: 500 kbps (standard VESC rate)
2. **Controller ID**: Default is 1, can be configured via VESC Tool
3. **Message Filtering**: All VESC messages should be accepted
4. **Error Handling**: Implement CAN error counters and logging

### Data Parsing

The VESC CAN parser should:
1. Receive CAN frames with 29-bit IDs
2. Identify message type from the ID format
3. Extract fields according to the message type
4. Convert units to OpenDash standard (e.g., current × 100 → A)
5. Map to appropriate OpenDash data point IDs

### ESP-NOW Integration

When integrating with OpenDash via ESP-NOW:
1. Parse VESC CAN messages in the background
2. Convert to OpenDash data point format
3. Send via ESP-NOW using `OPENDASH_CMD_SET_DATA_POINT` command
4. Broadcast at 5-10 Hz rate to match display refresh

## Example Implementation

```c
// Example VESC CAN message parsing
void parse_vesc_status_1(uint32_t controller_id, uint8_t *data) {
    int32_t erpm = (int32_t)(data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24));
    int16_t current = (int16_t)(data[4] | (data[5] << 8));
    int16_t duty = (int16_t)(data[6] | (data[7] << 8));
    
    // Convert to OpenDash units
    float current_a = current / 100.0f;  // Convert from centi-A to A
    float duty_percent = duty / 1000.0f; // Convert from milli-percent to percent
    
    // Send to OpenDash via ESP-NOW
    send_data_point(OPENDASH_DP_VESC_ERPM, erpm);
    send_data_point(OPENDASH_DP_VESC_CURRENT, current_a);
    send_data_point(OPENDASH_DP_VESC_DUTY, duty_percent);
}
```

## Testing Considerations

1. **CAN Bus Monitoring**: Use CAN analyzer to verify message format
2. **Unit Conversion**: Verify all unit conversions are correct
3. **Data Range Validation**: Ensure values stay within expected ranges
4. **Error Handling**: Test with malformed CAN frames
5. **Integration Testing**: Verify data appears correctly on OpenDash displays