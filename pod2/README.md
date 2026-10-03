<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OpenDash — Pod 2 (1.75" Round AMOLED)

**Active node.** Same hardware and firmware shape as
[`pod1/`](../pod1/README.md), as ESP-NOW slave `OPENDASH_NODE_POD2` — see
[`wiki/pod1-pod2.md`](../wiki/pod1-pod2.md) for the shared subsystem reference.

- **Board:** Waveshare ESP32-S3-Touch-AMOLED-1.75 (ESP32-S3, 16 MB flash / 8 MB PSRAM)
- **Display:** 466×466 round AMOLED, CO5300 QSPI; touch: CST9217 (I2C)
- **IMU:** QMI8658 6-axis — rollover votes broadcast for deployment voting
- **Screens:** cycle via touch swipe or boot button (see `main.c` header)

## Build & Flash

```bash
cd pod2
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

## BLE OTA — ⚠️ fragile

Mirror of POD1's gap (TODO §1.2): full sdkconfig recipe + pre-OTA suspend
sequence not yet applied. LEFT/RIGHT hold the working reference implementation.

## Source of truth

[`main.c`](main/main.c) header comment; shared code in [`common/`](../common/)
(see [`DISPLAY_SYNCHRONIZATION.md`](../DISPLAY_SYNCHRONIZATION.md)).