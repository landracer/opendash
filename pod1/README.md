<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OpenDash — Pod 1 (1.75" Round AMOLED)

**Active node.** Display + safety-deployment unit. Full subsystem reference:
[`wiki/pod1-pod2.md`](../wiki/pod1-pod2.md).

- **Board:** Waveshare ESP32-S3-Touch-AMOLED-1.75 (ESP32-S3, 16 MB flash / 8 MB PSRAM)
- **Display:** 466×466 round AMOLED, CO5300 QSPI; touch: CST9217 (I2C)
- **IMU:** QMI8658 6-axis (accel + gyro) on the shared I2C bus
- **Role:** ESP-NOW slave (`OPENDASH_NODE_POD1`). Receives `SET_DATA_POINT` /
  `SET_DATA_BATCH` from CENTER; broadcasts IMU rollover **votes** for parachute
  deployment voting; answers PINGs with `STATUS_REPORT`. No GNSS — display-only pod.
- **Screens** (cycle via touch swipe or boot button): OIL_TEMP, WATER, AFR,
  BOOST, GFORCE, DEBUG.

## Build & Flash

```bash
cd pod1
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

## BLE OTA — ⚠️ fragile

BLE OTA works on this node but is **not hardened**: the full sdkconfig recipe
(2M PHY, PPCP intervals, `BT_CTRL_PINNED_TO_CORE_1`, ACL buffers) and the
slave-side suspend sequence (`ui_manager_suspend()` + `display_pause_for_ota()`
before `opendash_bt_ota_enter()`) that LEFT/RIGHT use are **not yet applied**
here. See TODO §1.2 and [`wiki/ota-bluetooth.md`](../wiki/ota-bluetooth.md).

## Source of truth

[`main.c`](main/main.c) header comment carries the authoritative init-order and
pin details. Shared code lives in [`common/`](../common/) (see
[`DISPLAY_SYNCHRONIZATION.md`](../DISPLAY_SYNCHRONIZATION.md)).