# OpenDash — GPS / Telemetry Unit (1.75" AMOLED)

This is the GPS and Telemetry Unit project for OpenDash, running on the Waveshare ESP32-S3-Touch-AMOLED-1.75 hardware.

## Hardware Specifications

- **Board:** Waveshare ESP32-S3-Touch-AMOLED-1.75
- **Display:** 466×466 Round AMOLED (CO5300 controller)
- **Touch:** Capacitive touch (CST816S)
- **GPS:** LC76G GNSS module (GPS, GLONASS, BeiDou, Galileo)
- **IMU:** QMI8658 6-axis (accelerometer + gyroscope)
- **MCU:** ESP32-S3 dual-core @ 240MHz
- **Flash:** 16MB
- **PSRAM:** 8MB Octal SPI
- **Role:** I2C Slave, GPS/IMU data provider

## Features

- **GPS Positioning:** Multi-constellation GNSS for accurate position and speed
- **Lap Timing:** Real-time lap timing with predictive delta
- **G-Force Monitoring:** 3-axis accelerometer for lateral, longitudinal, and vertical g-forces
- **Motion Tracking:** Gyroscope for yaw, pitch, and roll rates
- **Professional AMOLED Display:** High-contrast round display with GPS speed, lap timing, and g-force visualization
- **I2C Slave:** Provides GPS and IMU data to the Center unit

## Building & Flashing

### Prerequisites

1. **ESP-IDF v5.3** installed
2. **Visual Studio Code** with ESP-IDF extension (recommended)
3. **USB-C cable** for programming and power

### Command Line Build

```bash
cd gps/
source ~/esp/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

### Visual Studio Code Build

1. Open this folder in VS Code
2. Press **F1** → **ESP-IDF: Set Espressif device target** → **ESP32-S3**
3. Press **F1** → **ESP-IDF: Build your project**
4. Press **F1** → **ESP-IDF: Flash your project**
5. Press **F1** → **ESP-IDF: Monitor device**

## Project Structure

```
gps/
├── CMakeLists.txt                # Main project CMake file
├── sdkconfig.defaults            # ESP-IDF configuration defaults
├── README.md                     # This file
└── main/
    ├── CMakeLists.txt            # Main component CMake file
    ├── idf_component.yml         # Component dependencies
    ├── main.c                    # Application entry point
    ├── display_init.c/h          # Display hardware initialization
    ├── ui_manager.c/h            # LVGL UI management
    ├── gps_handler.c/h           # LC76G GPS module interface
    └── imu_handler.c/h           # QMI8658 IMU interface
```

## Default UI Layout

```
        ┌──────────────────┐
       ╱    GPS Speed       ╲
      │    (Large numeric)   │
      │       125 km/h       │
      │                      │
      │ ┌──────────────────┐ │
      │ │   Lap Time       │ │
      │ │   1:32.456       │ │
      │ │   Lap Delta      │ │
      │ │   +0.234s        │ │
      │ │                  │ │
      │ │   G-Force Circle │ │
      │ │     [Viz]        │ │
      │ └──────────────────┘ │
       ╲ 12 Sats   275°     ╱
        └──────────────────┘
```

## GPS Module (LC76G)

The LC76G GNSS module provides:
- **Multi-constellation:** GPS, GLONASS, BeiDou, Galileo
- **High accuracy:** <2.5m CEP (50% circular error probability)
- **Fast fix:** Cold start <30s, hot start <1s
- **NMEA output:** Standard NMEA 0183 sentences via UART

## IMU Sensor (QMI8658)

The QMI8658 6-axis IMU provides:
- **Accelerometer:** ±2g/±4g/±8g/±16g selectable range
- **Gyroscope:** ±16/±32/±64/±128/±256/±512/±1024/±2048 °/s selectable range
- **High sample rate:** Up to 1 kHz for real-time motion tracking
- **Low noise:** Suitable for precision g-force measurements

## Configuration

Display configuration is stored in NVS and can be modified via:
- Center unit (over I2C)
- Companion app (over WiFi/BLE)

## Data Provided to System

The GPS unit provides the following data points to the OpenDash system:
- GPS position (latitude, longitude, altitude)
- GPS speed and heading
- Satellite count and fix quality
- 3-axis acceleration (g-forces)
- 3-axis gyroscope (rotation rates)
- Calculated lap times and deltas

## Next Steps

- **Add UART GPS Interface:** Parse NMEA sentences from LC76G
- **Add I2C IMU Interface:** Read accelerometer and gyroscope data from QMI8658
- **Add Lap Timing Logic:** Implement track detection and lap timing
- **Add G-Force Visualization:** Real-time g-force circle with position indicator
- **Add I2C Slave Handler:** Respond to data requests from Center unit
- **Add SD Card Logging:** Log GPS track data and telemetry

## Troubleshooting

### GPS not getting a fix

- Ensure clear view of the sky
- Check antenna connection
- Wait 30-60 seconds for cold start
- Verify UART communication (9600 baud)

### IMU reading zeros

- Check I2C connections to QMI8658
- Verify I2C address (typically 0x6A or 0x6B)
- Ensure IMU is properly initialized

## License

See main repository README for license information.

---

**OpenDash GPS / Telemetry Unit** — Built for racers, by racers. 🏎️💨
