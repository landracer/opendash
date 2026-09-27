<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OpenDash Pod 1 and Pod 2 — Display and Safety Deployment Units

> **STATUS: ACTIVE**. Pod 1 and Pod 2 are fully functional display units that also serve as safety deployment detectors.

Pod 1 and Pod 2 are specialized display units that extend the OpenDash system beyond the standard left/right gauge pods. These units are designed to provide additional display capabilities while also serving as safety deployment detectors in the rollover parachute system.

## Hardware Specifications

- **Board:** Waveshare ESP32-S3-Touch-AMOLED-1.75
- **Display:** 466×466 Round AMOLED (CO5300 QSPI)
- **Resolution:** 466×466 pixels
- **Touch:** CST9217 capacitive touch controller (I2C)
- **IMU:** QMI8658 6-axis accelerometer + gyroscope (I2C)
- **MCU:** ESP32-S3 dual-core @ 240MHz
- **Flash:** 16MB
- **PSRAM:** 8MB Octal SPI

## Role in System

### Display Function
Pod 1 and Pod 2 function as display units that:
- Receive engine data from the Center display via ESP-NOW
- Display various engine parameters including:
  - Oil Temperature
  - Coolant Temperature
  - Air-Fuel Ratio (AFR)
  - Boost Pressure
  - System diagnostics
- Cycle through different display modes using the boot button or touch swipe

### Safety Deployment System
Pod 1 and Pod 2 also serve as safety deployment detectors in the rollover parachute system:
- Each unit contains a QMI8658 IMU for detecting vehicle rollover
- They participate in the distributed safety system by voting on rollover detection
- They broadcast votes to the Center display for fusion and interlock evaluation
- They are part of the quorum required for autonomous parachute deployment
- They support both automatic rollover detection and manual override capabilities

## Node Identity

- **Pod 1:** Node type `OPENDASH_NODE_POD1` (ESP-NOW)
- **Pod 2:** Node type `OPENDASH_NODE_POD2` (ESP-NOW)

## Display Screens

Pod 1 and Pod 2 display the following screens that cycle with touch swipe or boot button press:

1. **OIL_TEMP** — Oil temperature (large) + info panel
2. **WATER** — Coolant temperature (large) + info panel
3. **AFR** — Air-fuel ratio (large) + info panel
4. **BOOST** — Boost pressure (large) + info panel
5. **GFORCE** — G-force display with lateral/longitudinal/total
6. **DEBUG** — System diagnostics + ESP-NOW stats

## Safety Deployment Integration

Pod 1 and Pod 2 are integral components of the safety deployment system as described in [Safety Deployment System](safety-deployment-system.md). They:

- Act as rollover detectors using their QMI8658 IMU
- Broadcast votes to the Center display for quorum evaluation
- Participate in the distributed safety architecture where autonomous deployment requires unanimous agreement from all detectors
- Support both automatic rollover detection and manual override capabilities
- Are configured as part of the fixed detector set: `{ RIGHT, POD1, POD2 }`

## Building & Flashing

### Prerequisites

1. **ESP-IDF v5.3** installed
2. **Visual Studio Code** with ESP-IDF extension (recommended)
3. **USB-C cable** for programming and power

### Command Line Build

```bash
cd pod1/  # or pod2/
source ~/esp/esp-idf/export.sh   # Set up ESP-IDF environment
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

### Visual Studio Code Build

1. Open `opendash.code-workspace` in VS Code
2. Press **F1** and select **ESP-IDF: Set Espressif device target**
3. Select **ESP32-S3**
4. Press **F1** and select **ESP-IDF: Build your project**
5. Press **F1** and select **ESP-IDF: Flash your project**
6. Press **F1** and select **ESP-IDF: Monitor device**

## Project Structure

```
pod1/ or pod2/
├── CMakeLists.txt                # Main project CMake file
├── sdkconfig.defaults            # ESP-IDF configuration defaults
├── main/
│   ├── CMakeLists.txt            # Main component CMake file
│   ├── main.c                    # Application entry point
│   ├── display_init.c/h          # Display hardware initialization
│   ├── ui_manager.c/h            # LVGL UI management
│   ├── imu_handler.c/h           # IMU (QMI8658) driver
│   └── idf_component.yml         # Component dependencies
├── partitions.csv                # Flash partition table
└── README.md                     # This file
```

## Integration with Other Components

Pod 1 and Pod 2 integrate with the broader OpenDash system:

- **Communication:** Use ESP-NOW wireless bus for communication with Center display
- **Data Flow:** Receive data points from Center via SET_DATA_POINT commands
- **Safety System:** Participate in the rollover detection and deployment system
- **Shared Code:** Use the common library (`common/`) for ESP-NOW protocol, data models, and display configuration

## Troubleshooting

### Display not turning on

- Check USB-C power connection
- Verify ESP32-S3 is properly powered
- Check serial output for initialization errors

### Touch not responding

- Ensure CST9217 touch controller is properly initialized
- Check I2C connections to touch controller
- Verify touch interrupt GPIO is configured

### Build errors

```bash
# Clean build artifacts
idf.py fullclean

# Reconfigure and rebuild
idf.py set-target esp32s3
idf.py build
```

## Future Enhancements

- Support for additional pod types (POD3-POD8) as part of the expansion system
- Enhanced diagnostic capabilities for safety systems
- Integration with additional vehicle systems for expanded monitoring