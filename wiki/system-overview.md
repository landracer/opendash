<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OpenDash — End-User System Guide

> **Updated June 2026.** Detailed, hands-on walkthrough of what OpenDash is,
> how the pieces fit together, and how to operate it day-to-day. If you're
> new, read this in order; if you're returning, jump to the section you need.

This is **not** a "plop and play" system. It is a network of up to 18 small
embedded computers cooperating over an ESP-NOW wireless bus, fed by sensor
data from a MultiDisplay engine-management ECU. There are 12 active node
families, multiple control surfaces (the BOOT button on each display pod,
the CENTER touch screen, and the CENTER USB console), and three update paths
(USB-wired, BLE-OTA wireless, and — planned — WiFi/HTTP).

---

## 1. What's in the box

### Display nodes

| Node | Hardware | Resolution | Role |
|---|---|---|---|
| **CENTER** | Waveshare ESP32-S3-Touch-LCD-4.3 | 800×480 IPS touch | Main dash. ESP-NOW master. Hosts the touch UI, layout editor, boost config, and USB console. |
| **LEFT** | Waveshare ESP32-S3-LCD-2.8C (round) | 480×480 round RGB | Configurable gauges. **Sole MD UART ingest** (HC-05 BT-serial). Batches sensor data to ESP-NOW. |
| **RIGHT** | Waveshare ESP32-S3-LCD-2.8C (round) | 480×480 round RGB | Configurable gauges. ESP-NOW only — receives MD data relayed from CENTER. |
| **POD1** | Waveshare ESP32-S3-Touch-AMOLED-1.75 | 466×466 AMOLED | Auxiliary gauge display. Same hardware as GPS but no GNSS/SD. IMU broadcast for parachute vote system. |
| **POD2** | Waveshare ESP32-S3-Touch-AMOLED-1.75 | 466×466 AMOLED | Auxiliary gauge display. Same hardware as GPS but no GNSS/SD. IMU broadcast for parachute vote system. |
| **GPS** | Waveshare ESP32-S3-Touch-AMOLED-1.75 | 466×466 AMOLED | LC76G GNSS + QMI8658 IMU @ 100 Hz. SD-card data logger. |
| **BMS** *(external)* | ESP32-DOIT-DevKit-V1 + SSD1306 OLED | 128×64 | rAtTrax BMS logger. Broadcasts cell voltages, SOC, temperatures over ESP-NOW. |

### Headless control nodes

| Node | Hardware | Role |
|---|---|---|
| **MOS-4CH-A** | ESP32 4-Way MOSFET Module | **Boost controller target.** Runs the PID boost algorithm + PWM solenoid output. Receives SET_PARAMS / LIVE_DATA from CENTER. |
| **MOS-4CH-B** | ESP32 4-Way MOSFET Module | Secondary MOSFET output block. Same firmware as A with different node ID. |
| **Relay-4CH-HD** | LCTech 4-CH HD relay board | 4-channel relay output. Controlled by CENTER via `SET_RELAY` (0x08). |
| **Relay-8CH-A** | LCTech 8-CH relay board | 8-channel relay, GPIO-verified. Self-test mode on BOOT-hold. |
| **Relay-8CH-B** | LCTech 8-CH relay board | 8-channel relay. Parity target with -A; GPIO mapping TBD. |

### USB Bridge Nodes

| Node | Hardware | Role |
|---|---|---|
| **openDstream** | ESP32-WROOM-32 | **ESP-NOW → UART bridge.** Listens on channel 1 (same as the BMS Logger), decodes OpenDash frames, and prints each DP as a `DP:0x…:v.vv` line over UART0 → onboard USB-UART bridge → host PC (multidisplay-app Qt application). Headless, no display. See [`openDstream/README.md`](../openDstream/README.md) |

All nodes share `common/` for: ESP-NOW protocol, data model, BLE-OTA service,
node health state machine, NVS persistence, and layout system. Node-specific
code is limited to the display driver, input handler, and identity constants.

---

## 2. How the pieces talk

```
                              UART / HC-05 BT-serial
       ┌──────────┐ ──────────────────────────────┐
       │ MD ECU   │                               ↓
       └──────────┘                          ┌─────────┐
                                             │  LEFT   │ ◄──┐  ← MD ingest + batch
       ESP-NOW (all links, WiFi PHY P2P)     └─────────┘    │  + CENTER's
       ┌──────────┐                                          │  SET_DATA_BATCH
       │ CENTER   │ ◄────────────────────────────────────────┘  ┌─────────┐
       │ master   │ ─────────────────────────────────────────►  │  RIGHT  │
       └──────────┘                               │             └─────────┘
            ▲  │ CDC-ACM USB                      │             ┌─────────┐
            │  ↓                                  ├──────────►  │  GPS    │
       ┌──────────┐  ┌─────────┐                  │             └─────────┘
       │  laptop  │  │ BLE OTA │   (BLE only       │             ┌─────────┐
       └──────────┘  └─────────┘    during OTA)    ├──────────►  │ POD1/2  │
                                                   │             └─────────┘
                                                   │             ┌─────────┐
                                                   ├──────────►  │ MOS-A/B │  ← boost PWM out
                                                   │             └─────────┘
                                                   │             ┌─────────┐
                                                   └──────────►  │ RELAY*  │  ← relay ch0-7
                                                                 └─────────┘
```

- **Wireless bus**: ESP-NOW — WiFi-PHY peer-to-peer with **no access point**.
  Sub-millisecond latency, no pairing, no DHCP. Works even if WiFi is in use.
  All nodes share channel 6. BLE is **not** running during normal operation —
  it activates on-demand for OTA only.
- **4 priority channels** (CRITICAL/MEDIUM/LOW/CONTROL): independent retry
  caps, per-channel worker tasks, quarantine back-off on delivery failures.
- **CENTER is the ESP-NOW master**: knows every slave's MAC (learned via HELLO
  frames at boot). Node IDs: LEFT=0x10, RIGHT=0x11, GPS=0x12, BMS=0x20,
  POD1=0x30, POD2=0x31, MOS_4CH_A=0x40, MOS_4CH_B=0x41,
  RELAY_8CH_A=0x50, RELAY_8CH_B=0x51, RELAY_4CH_HD=0x52.
- **Data batching**: LEFT batches each MD UART frame into **two** `DATA_BATCH`
  (0x88) packets — one for MD-native channels (MD-domain ids: `MD_RPM`,
  `MD_BOOST`, `MD_LAMBDA`, `MD_OIL_PRESS`, EGT1–8…) and one for the ECU's
  OBD2 PID values (shared engine ids, 0x0100 range) — at ~5 pps instead of
  one packet per data point.
- **Domain separation**: a value's source domain determines its id, and widget
  bindings are id-based — so MD data can never populate an ECU-bound widget
  and vice versa. CENTER re-relays every batch as `SET_DATA_BATCH` (0x0C) to
  **both** pods.
- **MultiDisplay (MD) feed**: only LEFT has the UART link to the MD's HC-05
  Bluetooth-serial bridge. LEFT parses the 95-byte binary frames and
  re-broadcasts all fields over ESP-NOW so RIGHT, GPS, POD1/2, and CENTER
  all receive them without ever touching the MD directly. RIGHT is a pure
  consumer — it renders whatever CENTER relays.
- **GPS broadcasts** position, velocity, heading, IMU accel/gyro, and
  satellite count on its own ESP-NOW cycle.
- **BMS broadcasts** cell voltages, pack current, SOC, and temperatures.
- **MOS-4CH-A** runs the PID boost controller (dual-gain, 32-point RPM
  interpolation). It receives `BOOST_LIVE_DATA` from CENTER at ≥10 Hz and
  outputs a PWM duty cycle to the boost solenoid.
- **Relay nodes** respond to `SET_RELAY` (0x08) from CENTER to switch
  individual output channels on/off.
---

## 3. Powering up — what to expect

1. **Apply 5 V** to each pod (USB or vehicle harness). Boot takes ~2 seconds.
2. **CENTER** comes up first, opens its ESP-NOW peer table, and waits for
   slaves to announce.
3. Each slave (LEFT/RIGHT/GPS/BMS/POD1/POD2/MOS/RELAY) sends a HELLO frame
   containing its MAC and node ID. CENTER ACKs and adds it to the active peer
   list. Headless nodes (MOS/Relay) are visible in the CONFIG screen as tiles.
4. CENTER's health monitor watches each peer. The status flow per node is:
   - **AWAITING** — no HELLO yet (just powered on)
   - **ONLINE** — receiving data at the expected rate
   - **DEGRADED** — rate dropped below threshold (warning)
   - **OFFLINE** — no frames for >2 s (warning)
5. **LEFT** also opens its UART and waits for the MD HC-05 link. When the
   MD shows up, you'll see `status=MD: mdv2 Connected` and STX-frame
   counts climb in the LEFT diagnostic log.
6. Once everyone is ONLINE, the dashes show live data.

If a node goes red on the CENTER status row, check power and antenna
proximity first.

---

## 4. The CENTER console — your control surface

The CENTER node exposes an interactive console over its USB CDC-ACM port.
Open it from a Linux host:

```bash
. /home/sysadmin/Documents/esp-ide/esp-idf/export.sh
idf.py -C center -p /dev/ttyACM4 monitor
```

> Replace `/dev/ttyACM4` with whatever port your CENTER enumerates as.
> Use `udevadm info -q property /dev/ttyACM*` to identify by serial number.

Type a command at the `>` prompt and press Enter. **Do not type the same
command twice** — a duplicate `ota` or `reset` triggers a second ESP-NOW
burst mid-execution on the slave, causing a race condition.

| Command | What it does |
|---|---|
| `help` | Show all available commands with descriptions. |
| `status` | Print live node health table: every peer's state, data rate, and RSSI. |
| `peers` | List the full ESP-NOW peer MAC table and their registered node IDs. |
| `nvs dump` | Dump all NVS namespaces and their current key/value pairs. |
| `ota left` | Send `ENTER_BT_OTA` ×3 to LEFT over ESP-NOW — pod suspends LCD and starts BLE advertising as `OpenDash-LEFT-OTA`. |
| `ota right` | Same for RIGHT. |
| `ota gps` | Same for GPS. |
| `ota pod1` | Same for POD1. |
| `ota pod2` | Same for POD2. |
| `ota mos_4ch_a` | Same for MOS-4CH-A boost controller. |
| `ota relay_8ch_b` | Same for Relay-8CH-B. |
| `reset left` | Reboot LEFT remotely over ESP-NOW. |
| `reset right` | Reboot RIGHT remotely. |
| `boost set ...` | Push boost parameters to the active MOS controller. |
| `boost mode ...` | Set boost controller operating mode (OFF/LOW/MED/HIGH). |

Exit `idf.py monitor` with `Ctrl-]`. The CENTER keeps running.

---

## 5. Reading the pod displays

### LEFT, RIGHT, POD1, and POD2 (round 480×480)

Each pod runs a multi-page gauge UI. Pages are configured at build time in
the pod's own `main/ui_manager.c` (`s_gauge_pages[]` table — edit, rebuild,
flash) and currently include:

1. OIL PRESS (arc) + BOOST — MD-domain senders (`MD_OIL_PRESS`, `MD_BOOST`)
2. WATER (`COOLANT_TEMP`, ECU-domain) + GPS SPEED
3. RPM (arc, `MD_RPM`, shift-light enabled) + LAM (`MD_LAMBDA`)
4. ODO (trip + total)

Each gauge has:
- **Primary arc** with min/max ticks and a colored fill that tracks value
- **Secondary numeric** below the arc with units (°C/°F, kPa/BAR/PSI, etc.)
- **Min/Max overlay** — small chips showing session high/low
- **Shift-light** — full-arc flash when RPM is above 90 % of redline
- **Warning overlay** — full-screen colored flash for fault states

### POD1 and POD2 Safety Deployment System

Pod 1 and Pod 2 also serve as safety deployment detectors in the rollover parachute system:
- Each unit contains a QMI8658 IMU for detecting vehicle rollover
- They participate in the distributed safety architecture by voting on rollover detection
- They are part of the quorum required for autonomous parachute deployment
- They support both automatic rollover detection and manual override capabilities
  (e.g. coolant >115 °C, oil pressure <0.5 bar at >2 k RPM)

#### Switching pages

Press the **BOOT button** on the pod for a short press (<500 ms) to advance
to the next page. The page index wraps.

#### Reset odometer (LEFT/RIGHT)

Press and hold the **BOOT button** for **5 seconds**. The pod will beep
three times, zero the trip odometer, and immediately save to NVS.

#### Unit toggle

Currently compile-time. Edit `opendash_display_config.c`'s
`UNIT_PROFILE_*` constants and rebuild. Per-screen unit choice via the UI
is in the TODO.

### CENTER (4.3" main dash)

CENTER's role is system-wide overview, not gauges. It shows:
- A status row for every peer (ONLINE/DEGRADED/OFFLINE + rate)
- Optional larger numeric tiles configurable in `ui_manager.c`
- The pre-flight checklist on power-up

### GPS (1.75" AMOLED)

GPS shows a compact satellite map, heading, speed, fix age, and HDOP. It
also writes a CSV log to its SD card whenever a card is present and a fix
is valid.

---

## 6. Pre-flight checklist

CENTER runs a checklist at boot. Items can be configured in
`common/include/opendash_checklist.h`. The checklist won't dismiss until
every item passes; this is intentional — it's a safety gate.

Default items:
1. CAN bus link (if enabled)
2. MD telemetry rate >5 Hz
3. RTC initialised
4. GPS HDOP <5.0 (if GPS in build)
5. BMS pack voltage within range (if BMS in build)
6. All peer nodes ONLINE

Dismiss via short-press the CENTER touch screen (touch is currently
limited — see TODO 1.2).

---

## 7. Warnings and shift light

The warning system is two-layer:

- **Per-pod warnings** (LEFT/RIGHT): a full-screen colored flash. Red for
  critical thresholds (oil pressure, coolant, EGT), yellow for cautions
  (low fuel, battery low). The active warning auto-clears 3 s after the
  trigger condition clears.
- **Shift light**: when a configured RPM gauge exceeds 90 % of redline,
  the arc itself flashes white. At 100 %, the entire screen flashes.

Thresholds live in `opendash_data_model.h` and the per-page config.

---

## 8. Updating firmware

There are three paths, ordered by speed and convenience.

### 8.1 Wired (USB) — fastest, dev workflow

```bash
. /home/sysadmin/Documents/esp-ide/esp-idf/export.sh
idf.py -C right -p /dev/ttyACM1 flash    # or left / center / gps
```

~30–60 seconds. The pod resets and runs the new image. Requires physical
access to the USB port. The factory bootloader is preserved, so OTA still
works after a wired flash.

### 8.2 BLE (Bluetooth Low Energy) — wireless, works in the car

This is the everyday "I changed one line" path. See
[`ota-bluetooth.md`](./ota-bluetooth.md) for the full step-by-step.

Typical timings (June 2026, post 2M-PHY + PPCP fix):

| Node | Image size | Wall time | Throughput |
|---|---|---|---|
| LEFT | 2.4 MB | ~5 min | ~9 KB/s (2M PHY, rebuilt Jun 2026) |
| RIGHT | 2.4 MB | **4 m 31 s** | **~9.1 KB/s** |
| GPS / POD1 / POD2 | ~0.9–1.0 MB | <2 min | ~6–8 KB/s |
| Relay / MOS nodes | <1 MB | <90 s | ~8 KB/s |

Two ways to trigger:
1. **CENTER touch UI** (preferred): Config → OTA Flash → tap the node button.
2. **CENTER console** (fallback): `ota right` at the `>` prompt.

Requires a Linux laptop within ~1 m of the target pod. Bleak + BlueZ
(`pip install bleak`). CENTER must be alive to send the OTA trigger.

### 8.3 Android (planned)

A native Android app for OTA-in-the-pits is on the roadmap. See
[`ota-android-plan.md`](./ota-android-plan.md) for options being
considered.

---

## 9. SD-card logging (GPS)

When an SD card is inserted in the GPS pod, the firmware writes
`opendash-YYYYMMDD.csv` with one row every 200 ms containing:

- UTC + GPS time
- Lat / Lon / Altitude / Heading / Speed / HDOP / sats
- IMU accel (x/y/z) and gyro (x/y/z)
- Selected MD/VESC channels (RPM, boost, coolant, AFR, etc.)

Cards are mounted SDMMC 1-line at boot. Hot-swap is not supported —
insert before power-up. FAT32 is required. Files rotate at midnight UTC.

---

## 10. Troubleshooting cheat sheet

| Symptom | First thing to try |
|---|---|
| Pod boots but shows `AWAITING` forever | Power-cycle CENTER. ESP-NOW peer table is built on CENTER. |
| LEFT shows `MD: Waiting [mdv2]…` | Check HC-05 power and pairing. Re-plug the BT-serial dongle. |
| RIGHT keeps going DEGRADED then ONLINE | Antenna near a metal surface? Move the pod away from large metal. |
| Watchdog reboot on screen switch | Known issue, mitigated; if it returns, file a bug with the LEFT/RIGHT serial log. |
| OTA times out at "Connecting…" | Move the laptop within 1 m of the pod. Run `bluetoothctl power off; bluetoothctl power on` to clear stale BlueZ state. |
| OTA stalls mid-transfer | Just re-run the client. It auto-RESUMEs from the last verified offset. |
| Touch not working on CENTER | Known issue — GT911 INT pin conflict. See TODO 1.2. |
| Center console says `RIGHT: TX queue rejects` | Normal during boot before RIGHT advertises HELLO. Should clear within a few seconds. |

---

## 11. Where to look next

| Want to… | Read this |
|---|---|
| Build for the first time | [`QUICKSTART.md`](../QUICKSTART.md) + [`BUILD_DEPENDENCIES.md`](../BUILD_DEPENDENCIES.md) |
| Understand the wireless protocol | [`docs/espnow-protocol.md`](../docs/espnow-protocol.md) |
| See every data point on the bus | [`docs/data-points.md`](../docs/data-points.md) |
| Run a BLE OTA right now | [`wiki/ota-bluetooth.md`](./ota-bluetooth.md) |
| Plan an Android client | [`wiki/ota-android-plan.md`](./ota-android-plan.md) |
| Add a new gauge | [`PER_NODE_DISPLAY_CONFIG_SPEC.md`](../PER_NODE_DISPLAY_CONFIG_SPEC.md) |
| See the BLE-OTA root-cause investigation | [`BLE_OTA.md`](../BLE_OTA.md) |
