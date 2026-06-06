<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OpenDash - GPS Unit (1.75" AMOLED)

> **See also:** The definitive LC76G I2C driver guide with complete byte sequences,  
> timing, power management, recovery mechanisms, and code-from-scratch instructions:  
> **[LC76G-I2C-GPS-Driver-Guide.md](LC76G-I2C-GPS-Driver-Guide.md)**  
>  
> For hardware conversion details (QSPI AMOLED, PMIC, GPIO expander, touch):  
> **[waveshare-1.75-opendash-conversion.md](waveshare-1.75-opendash-conversion.md)**

## Overview

The GPS unit is a WaveShare ESP32-S3-Touch-AMOLED-1.75 display node that provides GPS positioning data to the OpenDash system. This unit interfaces with the LC76G GNSS module via I2C to extract position, speed, heading, and satellite information.

## Hardware Specifications

### Display
- **Model**: WaveShare ESP32-S3-Touch-AMOLED-1.75
- **Display Type**: 1.75" Round AMOLED
- **Resolution**: 466×466 pixels
- **Controller**: CO5300
- **Interface**: QSPI (SPI2_HOST, quad-data)

### GNSS Module
- **Model**: LC76G GNSS module
- **Protocol**: Multi-constellation (GPS, GLONASS, BeiDou, Galileo)
- **Accuracy**: Sub-meter CEP (typical), ~2.5m with HDOP <2.0
- **Update Rate**: 10 Hz
- **I2C Interface**: CASIC protocol

## I2C Configuration

### Bus Setup
- **Bus Speed**: 100 kHz (standard I2C)
- **Master**: Center display (I2C master)
- **Slave Address**: 0x12 (GPS unit)
- **GPIO Pins**:
  - SDA: GPIO15
  - SCL: GPIO14

### I2C Addresses
- **Write Address**: 0x50 (7-bit) - for sending CASIC commands
- **Read Address**: 0x54 (7-bit) - for receiving NMEA data
- **Data Write Address**: 0x58 (7-bit) - for sending NMEA commands

## CASIC I2C Protocol

The LC76G GNSS module uses the CASIC I2C protocol as described in the Quectel I2C Application Note v1.0:

### Read Sequence
1. **Query Data Length**:
   - Transmit to 0x50: `{0x08, 0x00, 0x51, 0xAA, 0x04, 0x00, 0x00, 0x00}`
   - Receive from 0x54: 4 bytes → little-endian uint32 available byte count

2. **Request NMEA Data**:
   - Transmit to 0x50: `{0x00, 0x20, 0x51, 0xAA, <4 bytes length>}`
   - Receive from 0x54: N bytes → raw NMEA ASCII

### Write Sequence
1. **Query RX Buffer Free Space**:
   - Transmit to 0x50: `{0x04, 0x00, 0x51, 0xAA, 0x04, 0x00, 0x00, 0x00}`
   - Receive from 0x54: 4 bytes → free space

2. **Config Write**:
   - Transmit to 0x50: `{0x00, 0x10, 0x53, 0xAA, <4 bytes length>}`

3. **Write Data**:
   - Transmit to 0x58: actual NMEA command bytes

## Supported NMEA Sentences

### $GPRMC (Recommended Minimum)
- **Fields**: Time, status, latitude, N/S, longitude, E/W, speed_knots, heading, date
- **Purpose**: Position, time, status, speed, heading

### $GPGGA (Global Positioning System Fix Data)
- **Fields**: Time, latitude, N/S, longitude, E/W, fix quality, satellites, HDOP, altitude, MSL, geoid, age, DGPS station ID
- **Purpose**: Fix quality, altitude, satellites, HDOP

### $GPGSV (Satellites in View)
- **Fields**: Number of messages, message number, total satellites, satellite PRN, elevation, azimuth, SNR
- **Purpose**: Visible satellite count and information

## Data Point Integration

The GPS unit provides data points that are integrated into the OpenDash system:

| Data Point ID | Name | Unit | Range | Description |
|---------------|------|------|-------|-------------|
| `0x0200` | GPS Speed | km/h | 0–500 | Speed from GNSS |
| `0x0201` | GPS Heading | ° | 0–360 | Heading (true north) |
| `0x0202` | Latitude | ° | ±90 | GPS latitude |
| `0x0203` | Longitude | ° | ±180 | GPS longitude |
| `0x0204` | Altitude | m | -500–9000 | GPS altitude |
| `0x0205` | Satellite Count | count | 0–40 | Number of satellites locked |
| `0x0206` | HDOP | ratio | 0–50 | Horizontal dilution of precision |

## OpenDash Integration

### GPS Task Implementation
The GPS unit runs a dedicated task that polls the LC76G module at 10 Hz:

```c
// GPS task loop
while (1) {
    // Query data length from LC76G (0x50)
    // Request NMEA data from LC76G (0x54)
    // Parse NMEA sentences
    // Update shared GPS data structure
    // Log status every 5 seconds
    vTaskDelay(pdMS_TO_TICKS(100));  // 100ms delay for 10 Hz
}
```

### Data Structure
```c
typedef struct {
    double latitude;     // Decimal degrees
    double longitude;    // Decimal degrees
    float altitude;      // Meters above MSL
    float speed;         // km/h
    float heading;       // Degrees (0–360)
    int satellites;      // Number of satellites used
    int fix_quality;     // 0=invalid, 1=GPS, 2=DGPS, 6=estimated
    float hdop;          // Horizontal dilution of precision
    bool fix_valid;      // GPS fix status
    int visible_sats;    // Total visible satellites
    uint32_t timestamp;  // Unix timestamp
} gps_data_t;
```

## Power Management

### Power Rails
The GPS unit is powered through the Waveshare BSP which manages power rails via the AXP2101 PMIC:

1. **ALDO3**: GPS module power rail
2. **ALDO4**: GPS module power rail  
3. **BLDO2**: GPS module power rail

### Power Cycling
The system performs a full power cycle on GPS initialization:
1. Turn OFF all GPS power rails (ALDO3, ALDO4, BLDO2)
2. Assert NRESET (P5 LOW) via TCA9554
3. Wait 2 seconds for full discharge
4. Turn ON all GPS power rails
5. Release NRESET (P5 HIGH) via TCA9554
6. Wait 3 seconds for module to boot

## Troubleshooting

### Common Issues
1. **No GPS Fix**: 
   - Check antenna connection
   - Verify satellite visibility
   - Confirm module power supply

2. **I2C Communication Failure**:
   - Verify I2C bus speed (100 kHz)
   - Check I2C address conflicts
   - Ensure proper pull-up resistors

3. **Data Parsing Errors**:
   - Check NMEA sentence format
   - Verify data length queries
   - Monitor for corrupted data

### Diagnostic Commands
```c
// Send cold start command to GPS module
esp_err_t gps_handler_send_cold_start(void) {
    return lc76g_send_command("PQTMCOLD");
}

// Send warm start command to GPS module  
esp_err_t gps_handler_send_warm_start(void) {
    return lc76g_send_command("PQTMWARM");
}
```

## Maintenance

### Regular Checks
1. **I2C Bus Health**: Verify all devices respond
2. **GPS Signal Quality**: Monitor satellite count and HDOP
3. **Power Supply**: Check voltage levels on GPS rails
4. **Data Integrity**: Validate GPS data consistency

### Calibration
The GPS unit requires no manual calibration. The LC76G module automatically calibrates its internal state when powered on.

## References

- [WaveShare ESP32-S3-Touch-AMOLED-1.75 Wiki](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75)
- [Quectel LC26GABLC76G Series I2C Application Note v1.0](./quectel_lc26gablc76g_series_i2c_application_note_v1-0.pdf)
- [OpenDash I2C Communication Protocol](./protocol.md)
- [OpenDash Data Points Legend](./data-points.md)