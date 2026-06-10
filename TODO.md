<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OpenDash — Project TODO

> Comprehensive tracking of planned features, known issues, and shipped subsystems.
> Organized by priority, then by subsystem and per-node.
>
> **Last cleanup pass:** 2026-05-31 — full code-vs-doc reconciliation against
> the v0.8.x (batched ESP-NOW + channel manager) and v0.9.x (per-node layout,
> boost controller staging) reality. Previous TODO reflected v0.4 era.
>
> **Project Status: BETA → APPROACHING 0.9** — Sensor data flowing end-to-end,
> 7 active node families (center, left, right, gps, pod1, pod2, mos-4ch-a/b,
> relay-4ch-hd, relay-8ch-a/b, external BMS), per-node layout authoring,
> boost-controller staging in progress, BLE OTA proven on round pods, active
> tearing investigation on center.

---

## Legend

| Symbol | Meaning |
|---|---|
| `[x]` | Done — verified in code |
| `[ ]` | Not started |
| `[~]` | Partial — scaffolding present, integration incomplete |
| `[!]` | Blocked / needs investigation |
| `[?]` | Status unverified — needs audit |

---

## 0. Project Snapshot (at-a-glance)

| Area | State |
|---|---|
| Active node families | 12: center, left, right, gps, pod1, pod2, mos-4ch-a, mos-4ch-b, relay-4ch-hd, relay-8ch-a, relay-8ch-b, openDstream (+ external BMS Logger) |
| Total node slots | `OPENDASH_NODE_COUNT = 18` ([common/include/opendash_common.h](common/include/opendash_common.h)) |
| ESP-NOW protocol | 31 opcodes (12 master + 8 slave + 11 boost), batched (DATA_BATCH 0x88 / SET_DATA_BATCH 0x0C), 4 priority channels, polling eliminated |
| Sensor source | MultiDisplay (HC-05/HC-06 BT @ 115200, 95-byte SERIALOUT_BINARY @ ~100 Hz, consumed at 5 Hz) |
| Working displays | center (4.3" RGB), left/right (2.8C round RGB), gps + pod1/pod2 (1.75" AMOLED) |
| BLE OTA | Working: pod1, pod2, left, right. **Fragile:** gps, pod1, pod2 still missing the full sdkconfig recipe + slave-side suspend sequence — see §1.2 |
| Active investigations | center RGB tearing (TEARING.md), GPS/POD OTA hardening, boost-controller wire-up |
| Git state | 224 files uncommitted; HEAD = `Initial base for GPS` (museum). All current work is local. |

---

## 1. Critical Bugs & Stability

### 1.1 WDT Crash on Screen Switch (Left/Right) — RESOLVED

- [x] Root cause: `update_outlined_text()` writes 10 labels without yielding,
      starving IDLE0 on CPU0 → WDT fires ~5 seconds after switching to ODO
- [x] Guard: only update ODO labels when ODO screen is active
- [x] Rate-limit ODO updates to 1/second (20 ticks × 50ms)
- [x] Add `taskYIELD()` between the two `update_outlined_text()` calls
- [x] Skip `lv_label_set_text()` if text hasn't changed (avoids invalidation storm)
- [x] UI task wraps `lv_timer_handler()` in LVGL mutex lock/unlock
- [ ] Stress test: rapid button pressing for 60+ seconds (verify no WDT)
- [ ] Consider dropping 4-shadow outlined text to 1 shadow for lighter render cost

### 1.2 MD Data Point Counter Not Incrementing on BATCH — NEEDS FIX

- [ ] The `dp/s` counter in `channel_mgr` logs (e.g. `CH1 flow: dp/s=0 rx=...`) 
      is only incremented for `DATA_RESPONSE` frames, not `SET_DATA_BATCH`.
      This is a cosmetic issue but may mask actual data flow problems.
      The batch parser in `espnow_master.c` should increment the counter
      for each DP in the batch.

### 1.2 BLE OTA Fragility on GPS / POD1 / POD2

> **Found during 2026-05-31 audit.** LEFT and RIGHT have the full BLE OTA
> hardening recipe applied; GPS / POD1 / POD2 do **not**. They've worked in
> the field but only because they have less display+CPU contention than the
> round pods — they remain at risk of intermittent disconnects.

- [x] LEFT/RIGHT: `BT_CTRL_PINNED_TO_CORE_1=y`, `BT_NIMBLE_PINNED_TO_CORE_1=y`
- [x] LEFT/RIGHT: PPCP 7.5–15 ms interval, 32 s supervision
- [x] LEFT/RIGHT: `BT_NIMBLE_LL_CFG_FEAT_LE_2M_PHY=y`, `ACL_BUF_COUNT=32`
- [x] LEFT/RIGHT: `ui_manager_suspend()` + `display_pause_for_ota()` (+ `opendash_uart_suspend()` on LEFT) before `opendash_bt_ota_enter()`
- [ ] **GPS:** apply full sdkconfig recipe (currently no `BT_CTRL_PINNED_TO_CORE_1`, no PPCP, no 2M PHY, no ACL bufs)
- [ ] **GPS:** add `ui_manager_suspend()` + `display_pause_for_ota()` before `opendash_bt_ota_enter()` ([gps/main/main.c](gps/main/main.c) ~L169)
- [ ] **POD1:** apply full sdkconfig recipe (same gaps as GPS)
- [ ] **POD1:** add suspend sequence before OTA ([pod1/main/main.c](pod1/main/main.c) ~L157)
- [ ] **POD2:** mirror POD1 fixes
- [ ] Re-measure POD1/POD2 OTA throughput after recipe applied (LEFT/RIGHT hit ~9.1 KB/s post-fix)

### 1.3 Center RGB Display Tearing — ACTIVE INVESTIGATION

> Canonical log: [TEARING.md](TEARING.md). Long-run capture script at
> [scripts/tearing_capture.sh](scripts/tearing_capture.sh).
> Acceptance: 10+ min continuous use with page switches, MIL flashes, BMS/OBD2
> bursts, BLE-OTA banner, zero tear events.

- [x] Vsync ISR → semaphore gate in `lvgl_flush_cb` ([center/main/display_init.c](center/main/display_init.c))
- [x] Fix A: drain stale vsync token before scheduling DMA swap
- [x] Fix B: mirror dirty rect into other framebuffer post-vsync
- [x] Bounce buffer = 20 lines (16000 px) — conservative anti-PSRAM-contention
- [x] Pixel clock 16 MHz (datasheet allows 23–27 MHz; 16 is Waveshare-stable)
- [x] ST7262 porches per datasheet (4/8/8/4/8/8)
- [x] `pclk_active_neg = true`
- [x] `LV_DISPLAY_RENDER_MODE_DIRECT` + `num_fbs = 2`
- [ ] **Baseline capture** with current code (`scripts/tearing_capture.sh`, 10 min)
- [ ] **H7:** toggle `CONFIG_LCD_RGB_RESTART_IN_VSYNC` off (single sdkconfig flip)
- [ ] **H6/H2:** drop pclk to 14 MHz (PSRAM bandwidth headroom)
- [ ] **H5/H8:** pin `lvgl_task` to core 0 priority `MAX-2`, force touch + button to core 1
- [ ] **H4:** replace per-rect Fix B mirror with full-frame copy on swap
- [ ] **H1 refinement:** try `bounce_buffer_size_px = 10 * LCD_H_RES` (half)
- [ ] **Last resort:** `num_fbs = 3` triple-buffer (+375 KB PSRAM)

### 1.4 GT911 Touch Not Detected (Left/Right)

- [x] GT911 hardware reset via TCA9554 EXIO2 before I2C probe
- [x] Probe both GT911 addresses: 0x5D and 0x14
- [!] **INT pin conflict:** GT911 INT may share GPIO16 with I2C slave SCL on
      2.8C boards. Needs schematic verification or pin relocation.
- [ ] Register GT911 as LVGL input device (full touch support)
- [ ] Add `esp_lcd_touch_gt911` managed component to `idf_component.yml`
- [ ] Test touch across all board revisions
- [ ] Apply equivalent fix to center/ (different hardware, different touch init)

### 1.5 Bad ETX Frame Loss on MD UART (~15–20%)

> Per CHANGELOG: confirmed **not** timing-related — it's electrical noise /
> byte corruption on the BT UART link. Parser handles it via STX re-scan.

- [~] False `STX+TAG` (0x02 0x5F) in 35-byte VR6 zero-padding triggers
      occasional false frame starts
- [ ] Implement frame timing validation (expect frames every ~10 ms)
- [ ] Track consecutive good frames to increase sync confidence
- [ ] Checksum/CRC plausibility on parsed fields

---

## 2. Per-Node Status

### 2.1 Center — ESP32-S3-Touch-LCD-4.3 (800×480 RGB, ST7262)

- [x] LVGL 9.2 + PSRAM-routed `lv_malloc_core` ([center/main/lv_mem_psram.c](center/main/lv_mem_psram.c))
- [x] ESP-NOW master with 4 priority channels + dispatcher
- [x] Display tearing mitigations (see §1.3 for active work)
- [x] Drag-race demo data generator
- [x] Multi-mode UI: ENGINE, GPS, MD, RELAY, BMS, OBD, CONFIG
- [x] OTA console command: `ota left|right|gps|pod1|pod2`
- [x] Layout editor UI ([center/main/layout_editor.c](center/main/layout_editor.c)) — Save pushes SET_SCREEN_LAYOUT via `espnow_master_send_layout_to_node()`; see §5
- [x] Boost config UI scaffold ([center/main/boost_config_ui.c](center/main/boost_config_ui.c)) — see §6
- [x] System config NVS ([center/main/system_config.c](center/main/system_config.c)) — boost target node, pressure unit
- [x] CONFIG screen renders per-node health via `ui_manager_update_config_node_status()` (`node_health_get_state` + `..._get_status_flags`, color-coded ONLINE/DEGRADED/AWAITING/OFFLINE)
- [ ] **Engine demo data auto-halt when MD arrives** — `s_active_datasrc`/`OPENDASH_DATASRC_DEMO` defined but no auto-switch logic found (GPS path works via different mechanism). Either implement or remove the demo (currently never yields)
- [~] Center SD logger ([center/main/sd_logger.c](center/main/sd_logger.c)) — **still a no-op stub**; needs CH422G SPI CS implementation
- [ ] Dedicated System Config screen (currently fragmented across overlays)
- [ ] Touch-based screen switching (swipe / tap zones)
- [ ] Settings screen (unit prefs, brightness, page assign, layout push)
- [ ] On-screen clock display (ESP-NOW time sync from GPS node)

### 2.2 Left — ESP32-S3-LCD-2.8C Round (480×480 RGB, ST7701S)

- [x] Multi-page gauge system (`GAUGE_PAGE_MAX = 8`; currently 3 gauge + 1 odo)
- [x] Boot button screen cycling + 5-sec long press → odometer reset (3-beep + NVS)
- [x] MD UART RX on GPIO20 (USB D+ reclaimed via `usb_serial_jtag_ll_phy_enable_pad(false)`)
- [x] All 8 EGT channels parsed + forwarded
- [x] DATA_BATCH forwarding to center
- [x] BLE OTA full recipe applied (sdkconfig + suspend sequence + RGB panel teardown)
- [x] PCF85063 RTC sync
- [x] Odometer NVS persistence with wear-level
- [ ] Pages configurable via NVS (currently compile-time only)
- [ ] Min/max markers on the arc itself (small triangles at angle positions)
- [ ] Configurable shift-light threshold (currently hardcoded 90%)
- [ ] Listen for `SET_SCREEN_LAYOUT` (0x02) and apply (see §5)

### 2.3 Right — ESP32-S3-LCD-2.8C Round (480×480 RGB, ST7701S)

- [x] Same multi-page gauge system as LEFT
- [x] Boot button long press → odometer reset
- [x] BLE OTA full recipe (sdkconfig + suspend + RGB teardown)
- [x] PCF85063 RTC sync
- [x] **No MD UART by design** — LEFT is sole MD ingest; `opendash_uart.c` compiled in via `common` REQUIRES but never initialized on RIGHT (no port configured). RIGHT receives MD data via ESP-NOW relay from CENTER.
- [ ] Pages configurable via NVS
- [ ] Listen for `SET_SCREEN_LAYOUT` (0x02)

### 2.4 GPS — ESP32-S3-Touch-AMOLED-1.75 (466×466, CO5300)

- [x] **GPS I2C driver complete** — v16c-port, production-verified (multi-day stable)
- [x] LC76G CASIC I2C protocol: 0x50/0x54/0x58 at 100 kHz
- [x] PMIC power cycle + WAKE + primer + per-read WAKE + drain mechanisms
- [x] NMEA parsing: GGA, RMC, VTG (+ historical: GSV, GSA, GLL, TXT, PQTM)
- [x] Automatic recovery from shared bus degradation
- [ ] **Cold-start (TTFF) behavior — note for field use:**
  - Cold start can take ~30–60 s of empty I2C polls before the first NMEA sentence appears (chip is still acquiring almanac/ephemeris with no almanac cached).
  - During this window `consecutive_fails` climbs; the current recovery threshold (100) can fire **one** PMIC power-cycle around ~35 s. The driver then self-heals and reaches a fix normally — but the extra power-cycle resets acquisition and lengthens TTFF.
  - **Mitigation to evaluate:** relax recovery threshold from 100 → ~50 *with a startup grace period* (suppress recovery for the first N seconds / until first NMEA) so a slow-but-healthy cold start is not mistaken for a hung bus. Reference: v16k journal in [wiki/gps-driver-debugging-v16.md](wiki/gps-driver-debugging-v16.md).
  - **Field note:** for fastest TTFF give the unit clear sky view and ~60 s on first power-up after a long-unpowered/relocated period; warm starts (recent fix, RTC valid) are much faster.
  - Any change here must be followed by build/flash/monitor before relying on it.
- [x] 3D Fix: 9–16 sats, HDOP 0.7–1.1, 62–95 K bytes/5 min
- [x] CST9217 touch via I2C 0x5A
- [x] **IMU (QMI8658) at 0x6B** — 100 Hz accel + gyro, register-level driver, G-force in ui_manager ([gps/main/imu_handler.c](gps/main/imu_handler.c))
- [x] SD card data logging — `sd_logger.c` wired in, 5 Hz snapshots, SDMMC 1-line
- [x] GPS UI: 4 modes (GPS, LAP, GFORCE, DEBUG) — speed/heading/sats/coords rendered
- [x] PCF85063 RTC sync
- [ ] **BLE OTA hardening** (see §1.2) — full recipe + suspend sequence
- [ ] 10 Hz update rate activation (`$PAIR050,100` after first fix)
- [ ] **Parachute deployment system** — zero code anywhere; design + implement
- [ ] GPS → RTC time sync on valid fix (gps_handler.c)
- [ ] Track map overlay (if 466×466 allows)
- [ ] Lap timer with sector times (see §11.5)

### 2.5 POD1 / POD2 — Auxiliary AMOLED Display Pods

- [x] Hardware: Waveshare ESP32-S3-Touch-AMOLED-1.75 (same as GPS, no GPS/SD)
- [x] QMI8658 IMU @ 100 Hz on both pods
- [x] 6 display modes (OIL, WATER, AFR, BOOST, GFORCE, DEBUG)
- [x] Receive SET_DATA_POINT from center
- [x] Broadcast IMU snapshot (intended for parachute deployment voting)
- [x] BLE OTA functional (proven Apr–May 2026)
- [ ] **BLE OTA hardening** (see §1.2) — full recipe + suspend sequence on both
- [ ] Listen for `SET_SCREEN_LAYOUT` (0x02)

### 2.6 MOS-4CH-A / MOS-4CH-B — MOSFET Output Nodes (Boost Controller Targets)

- [x] Hardware: ESP32 4-Way MOS FET Module, headless, PWM capable
- [x] `opendash_boost_init()` called at boot ([mos-4ch-a/main/main.c](mos-4ch-a/main/main.c) ~L500)
- [x] `boost_compute_task` on core 1 (~50 Hz)
- [x] `boost_telemetry_task` (~5 Hz heartbeat)
- [x] ESP-NOW handlers for `OPENDASH_CMD_BOOST_*` (SET_PARAMS, SET_MODE, SET_DUTY_ROW, etc.) — locally defined in MOS-A
- [x] PWM channel 3 configured for boost solenoid
- [x] `opendash_bt_ota.h` included, supports `ENTER_BT_OTA`
- [x] Per-channel GPIO pins concrete in source (GPIO 16, 17, 26, 27)
- [~] Physical channel ↔ GPIO mapping needs multimeter verification before deploying boost output
- [ ] **Move boost payload structs to shared header** (opcodes already there, structs still MOS-A-private)
- [x] MOS-4CH-B parity with -A confirmed (same structure, same handlers, only TAG/node_id/labels differ)

### 2.7 Relay Nodes

#### Relay-8CH-A (LCTech 8-CH, GPIO verified)

- [x] GPIO assignments verified: CH0–CH7 on GPIO 32,33,25,26,27,14,12,13
- [x] Boot-hold self-test mode (GPIO0 → cycle all channels 500 ms each)
- [x] `opendash_bt_ota.h` included, supports `ENTER_BT_OTA`
- [x] FTDI USB-TTL programming (shares /dev/ttyUSB1 with -B)
- [ ] Confirm `SET_RELAY` (0x08) + `REQUEST_RELAY_STATUS` (0x09) dispatch wired

#### Relay-8CH-B (LCTech 8-CH, GPIO unverified)

- [x] Hardware identified, `opendash_bt_ota.h` included
- [~] **GPIO assignments TBD** in source — verify with multimeter
- [ ] Self-test mode (port from -A)
- [ ] Confirm physical mapping matches -A before deploying boost / relay control

#### Relay-4CH-HD (LCTech 4-CH HD relay)

- [x] Hardware identified, `opendash_bt_ota.h` included
- [~] **GPIO assignments TBD** (source comment requires multimeter verification)

### 2.8 BMS Logger (external, ESP32-DOIT-DevKit-V1 + SSD1306)

- [x] Sends DATA_RESPONSE framed messages
- [x] Center auto-discovers, BMS slot in `s_nodes[]`, `bms_online` tracked
- [x] Full BMS data model in [opendash_data_model.h](common/include/opendash_data_model.h): SOH, IC temp, balance, charging, energy
- [ ] BMS alarm forwarding to OpenDash warning system
- [ ] Pack voltage/current display on center + gauge pods
- [ ] Shared lap tracking data between BMS and OpenDash (see §11.5)

### 2.9 Deprecated / Reference

- [ ] **`left-right/`** — pre-fork shared project; confirm nothing still builds from it, then archive
- [x] **`dash-pods/`** — FreeCAD enclosure design only (`opendash.FCStd`); no firmware
- [x] **`boostcontrol-staging/`** — **FROZEN** peer-review baseline per [boost-controller-opendash.md](boost-controller-opendash.md) Rule 1. Do not modify.

---

## 3. Display Features (Left / Right / POD Gauge UI)

### 3.1 Multi-Page Gauge System

- [x] `gauge_page_t` struct with configurable primary/secondary data points
- [x] `GAUGE_PAGE_MAX = 8`, currently 3 gauge + 1 odo on LEFT/RIGHT
- [x] Single LVGL widget set shared across pages (no create/destroy churn)
- [x] Labels, units, arc range swapped on page switch
- [x] Page cycling via boot button (short press)
- [ ] Make pages configurable via NVS — converges with §5 per-node layout
- [ ] ESP-NOW command from center to change page remotely
- [ ] Page indicator (dots or N/M at bottom)

### 3.2 Arc + Secondary Box

- [x] Symmetric arc outline (5 px each side, was asymmetric 10 outside)
- [x] Secondary box: 100 px (was 85, cramped)
- [ ] Auto-size secondary box height based on font metrics
- [ ] Optional progress bar for secondary value
- [ ] Threshold-driven border color on secondary (green/yellow/red)

### 3.3 Min/Max Tracking

- [x] Per-page session min/max of primary arc value
- [x] Displayed as `MIN:xx MAX:yy` below primary reading
- [ ] Min/max markers on the arc itself
- [ ] Optional NVS persistence (per-session auto-clear or manual reset)
- [ ] Min/max for secondary value too

### 3.4 Shift-Light Blink

- [x] Arc color toggles red/blue when arc > 90 % on `shift_light=true` pages
- [x] 150 ms blink interval, auto-revert below threshold
- [ ] Configurable threshold percentage (currently hardcoded)
- [ ] Configurable absolute RPM shift point
- [ ] Full-screen flash mode (border / background)
- [ ] External RGB shift-light output trigger

### 3.5 Background Images

- [x] Conditional include (`__has_include`), fallback to plain black
- [ ] Multiple background image themes (carbon fiber, brushed alu, etc.)
- [ ] Per-page background selection

---

## 4. Communication & Protocol

### 4.1 ESP-NOW Bus — Batched + Channel Managed (v0.8.x)

> Canonical reference: [DATAFLOW.md](DATAFLOW.md). All listed opcodes are
> defined in [common/include/opendash_i2c_protocol.h](common/include/opendash_i2c_protocol.h)
> (the "i2c" in the filename is legacy — transport is ESP-NOW since v0.4).

- [x] 4 priority channels: CRITICAL (0, 10 ms, 1 s timeout), MEDIUM (1, 50 ms),
      LOW (2, 200 ms, 2 s timeout), CONTROL (3, 5 ms)
- [x] Per-channel retry caps (CRITICAL 3, MEDIUM 2, LOW 1, CONTROL 5)
- [x] Dispatcher task (core 0 pri 4) routes by source MAC into channel queues
- [x] Channel-specific worker tasks: critical pri 5, medium pri 4, low pri 3 (core 1), control pri 6
- [x] 1 Hz timeout checker
- [x] Polling eliminated — event-driven push + data-absence offline detection
- [x] Pause/resume quarantine: dead-peer back-off after 5 consecutive failures, up to 30 s
- [x] **Master opcodes (12):** 0x01 SET_DATA_POINT, 0x02 SET_SCREEN_LAYOUT,
      0x03 SET_ALARM, 0x04 SET_BRIGHTNESS, 0x05 CHECKLIST_UPDATE,
      0x06 REQUEST_DATA, 0x07 SYSTEM, 0x08 SET_RELAY,
      0x09 REQUEST_RELAY_STATUS, 0x0A OBD_COMMAND, 0x0B AUDIO_ALERT,
      0x0C SET_DATA_BATCH
- [x] **Slave opcodes (8):** 0x81 DATA_RESPONSE, 0x82 STATUS_REPORT,
      0x83 CHECKLIST_STATUS, 0x84 ALARM_TRIGGERED, 0x85 RELAY_STATUS,
      0x86 DTC_REPORT, 0x88 DATA_BATCH, 0xFF NAK
- [x] **Boost opcodes (11):** M→S 0x20 LIVE_DATA, 0x21 SET_PARAMS, 0x22 SET_MODE,
      0x23 SET_DUTY_ROW, 0x24 SET_SETP_ROW, 0x25 SET_THROTTLE, 0x26 PULL_ALL;
      S→M 0x90 TELEMETRY, 0x91–0x93 PARAMS_REPORT. All in shared header
      ([common/include/opendash_i2c_protocol.h](common/include/opendash_i2c_protocol.h) ~L205–222)
- [ ] Protocol version handshake on startup
- [ ] Dynamic data point subscription (slave requests specific DPs)

### 4.2 Node Health + Bidirectional Status

- [x] 5-state machine: UNKNOWN → AWAITING → ONLINE → DEGRADED → OFFLINE
      ([common/src/node_health.c](common/src/node_health.c))
- [x] Layer 1: data flow frequency tracking (heartbeat vs frequency mode)
- [x] Layer 2: MAC-layer ACK feedback from `master_send_status_cb()`
- [x] Layer 3: NVS registry (`nd_health` namespace) — was_online flag survives reboot
- [x] Per-node expected frequencies (LEFT 50 pps, BMS 100 pps, POD 15 pps, GPS/MOS/RELAY heartbeat)
- [x] Rolling RSSI history per node
- [x] Hysteresis: must miss N windows before transitioning to OFFLINE
- [x] STATUS_REPORT (0x82) defined; `BLE_OTA` flag latch in `node_health_set_status_flags`
- [x] STATUS_REPORT rendering on center CONFIG screen ([center/main/ui_manager.c](center/main/ui_manager.c) ~L3509–3598 `ui_manager_update_config_node_status`)

### 4.3 MultiDisplay UART (HC-05/HC-06 Bluetooth)

> **MD Output Rate:** SERIALFREQ=10 → **100 Hz**. OpenDash UART RX keeps up;
> the main loop only consumes at **5 Hz** (every 200 ms). 95 % of frames are
> discarded (latest value kept). Consider SERIALFREQ=50 (20 Hz) on the MD
> side to save ATmega2560 CPU cycles with zero data loss. Changeable via MD
> serial command 11, persisted to EEPROM addr 101.

- [x] Binary frame parser: STX(0x02) TAG(0x5F) payload[93] ETX(0x03)
- [x] Little-endian AVR field extraction (`rd_u16`, `rd_s16`, `rd_u32`)
- [x] GPIO20 reclaimed via USB PHY pad disable
- [x] All 8 EGT channels + RPM + boost + throttle + lambda + LMM + battery
- [x] Speed, gear, N75, requested boost, EFR speed, knock, case temp, VDO sensors
- [x] Connection state machine: WAITING → RECEIVING → TIMEOUT
- [x] 5-second UART diagnostic heartbeat
- [x] Toggleable per-frame debug logging (`OPENDASH_UART_DEBUG`, default 0)
- [x] LEFT batches MD frames into single ESP-NOW packets at ~5 pps
- [x] Comprehensive docs: [UART_CONNECTION.md](UART_CONNECTION.md), [SERIAL_PROTOCOL.md](../multidisplay-firmware/multidisplay/SERIAL_PROTOCOL.md)
- [ ] Frame timing validation (§1.5)
- [ ] HC-05 AT-command auto-connect (currently relies on pre-paired modules)

### 4.4 CAN Bus / OBD2

- [x] `opendash_obd_config.c/h` — threshold + enable/MIL config with NVS
- [x] Warning thresholds: coolant, oil temp, oil pressure, battery V, boost PSI
      (AFR disabled by default)
- [ ] CAN transceiver integration on center
- [ ] OBD2 PID dispatch (RPM, MAP, AFR, coolant, etc.) over CAN
- [ ] DTC reading + clearing via `OBD_COMMAND` (0x0A) and `DTC_REPORT` (0x86)
- [ ] Custom CAN for aftermarket ECUs (Haltech, MegaSquirt, etc.)
- [ ] Configurable baud rate and filter masks
- [ ] **OBDII/ELM327 via MultiDisplay** — when MD firmware adds ELM327, extend
      `parse_binary_frame()` for new fields

### 4.5 VESC Integration

- [x] Full VESC data model (25 DPs in 0x0600 range) — see CHANGELOG v0.3.0
- [x] Wheel speed (FL/FR/RL/RR + average) in `opendash_data_model.h`
- [ ] VESC CAN bus driver (STATUS 1–6 frames, 500 kbps, 29-bit EID)
- [ ] VESC frame fan-out on ESP-NOW
- [ ] vesc_express CSV log format alignment for SD logger

---

## 5. Per-Node Display Layout System

> Spec: [PER_NODE_DISPLAY_CONFIG_SPEC.md](PER_NODE_DISPLAY_CONFIG_SPEC.md).
> Goal: end-user picks which DP shows in each slot of each mode, from the
> center touchscreen, persisted to NVS on each node.

### 5.1 Phase 1 — Wire Format + Persistence + Catalog (DONE)

- [x] Wire format v1 ([common/include/opendash_layout.h](common/include/opendash_layout.h)) — 13-byte header + 2×slot_count
- [x] Serialize / deserialize ([common/src/opendash_layout.c](common/src/opendash_layout.c))
- [x] NVS persistence ([common/src/opendash_layout_store.c](common/src/opendash_layout_store.c)) — namespace `od_layout`, keys `m0`–`m7`
- [x] Factory reset path
- [x] DP catalog with categories ([common/include/opendash_dp_catalog.h](common/include/opendash_dp_catalog.h)): ENGINE, TEMP, PRESSURE, FUEL, DRIVETRAIN, GPS, BMS, VESC, OBD, SYSTEM
- [x] DP metadata: label, units, defaults
- [x] `OPENDASH_CMD_SET_SCREEN_LAYOUT = 0x02` opcode reserved + payload format defined

### 5.2 Phase 2 — UI Editor + Live Apply (PARTIAL)

- [x] Layout editor UI ([center/main/layout_editor.c](center/main/layout_editor.c)) — 7 modes (ENGINE, GPS, MD, RELAY, BMS, OBD, CONFIG), ARC DP picker + 6 slot pickers, numeric keypad for arc min/max, modal pagination
- [x] Save / Reset buttons
- [x] Save pushes `SET_SCREEN_LAYOUT` over ESP-NOW via
      `espnow_master_send_layout_to_node()` ([center/main/espnow_master.c](center/main/espnow_master.c) ~L925–940) →
      `channel_mgr_send_to_node()`
- [ ] **Slave-side handler for `SET_SCREEN_LAYOUT`** — none of LEFT/RIGHT/GPS/POD1/POD2 implement the receive case yet. Center pushes into the void.
- [ ] Slave-side load from NVS at boot, fall back to compile-time defaults
- [ ] Slave-side live UI re-render on layout apply
- [ ] Pull-from-node (read back slave's actual stored layout)
- [ ] Per-slot min/max for arc on slave-side serializer

### 5.3 Phase 3 — Future

- [ ] CENTER's own layout editable (not just slaves)
- [ ] Layout templates / presets (Drag, Road Race, Drift, Daily)
- [ ] Import/export layout JSON via BLE / SD

---

## 6. Boost Controller

> Heritage: ported from MultiDisplay's `RPMBoostController` (Stephan Martin /
> Dominik Gummel, GPL-3.0). Spec: [boost-controller-opendash.md](boost-controller-opendash.md).
> Frozen peer-review baseline: [boostcontrol-staging/](boostcontrol-staging).

### 6.1 Shared Algorithm Layer (DONE)

- [x] [common/include/opendash_boost.h](common/include/opendash_boost.h) — params, duty rows, setpoint rows, throttle curve, live frame, telemetry, safety flags
- [x] [common/src/opendash_boost.c](common/src/opendash_boost.c) — dual-gain PID (aggressive/conservative), RPM interp across 32 points
- [x] Safety cuts: overboost (2.80 bar), EGT warn (880) + crit (950), AFR lean (16.0), fuel pressure min (200 kPa), data-stale timeout (600 ms) — any trigger → PWM duty 0
- [x] Per-row NVS persistence — namespace `boost`, keys `params`, `d_M_G`, `s_M_G`, `throt`
- [x] Thread-safe via semaphore

### 6.2 MOS-4CH-A Integration (DONE)

- [x] `opendash_boost_init()` called at boot
- [x] `boost_compute_task` (core 1, ~50 Hz)
- [x] `boost_telemetry_task` (~5 Hz heartbeat)
- [x] Local ESP-NOW dispatch for `OPENDASH_CMD_BOOST_*`
- [x] PWM channel 3 configured for boost solenoid output
- [x] Channel GPIO assignments verified (CH0=GPIO16, CH1=GPIO17, CH2=GPIO26, CH3=GPIO27)
- [x] Confirm MOS-4CH-B parity

### 6.3 Wire Protocol

- [x] Boost opcodes in shared header — 0x20 LIVE_DATA, 0x21 SET_PARAMS, 0x22 SET_MODE,
      0x23 SET_DUTY_ROW, 0x24 SET_SETP_ROW, 0x25 SET_THROTTLE, 0x26 PULL_ALL,
      0x90 TELEMETRY, 0x91–0x93 PARAMS_REPORT ([opendash_i2c_protocol.h](common/include/opendash_i2c_protocol.h) ~L205–222)
- [x] **Migrate payload structs from MOS-A-private to shared header** (now shared)
- [x] Center → MOS-A push helpers in [center/main/espnow_master.c](center/main/espnow_master.c) for each SET_* opcode
- [x] Live-data fan-out at ≥10 Hz: RPM, MAP/boost, EGT, AFR, fuel_psi, throttle%, gear
      (boost_client.c already snapshots; now transmits)
- [x] PULL_ALL handler on MOS side + PARAMS_REPORT round-trip

### 6.4 Center UI

- [x] `boost_config_ui.c` scaffold: LVGL grid, cell tap → numeric keypad,
      unit selector (kPa/BAR/PSI), mode/slot/gear selectors, "UNSTABLE LINK"
      banner, live telemetry, page nav (2 × 16 RPM)
- [x] `system_config.c` NVS: target node (default MOS_4CH_A), pressure unit
- [x] Unify under dedicated **System Config screen** (now integrated)
- [x] [Sync to Node] action — push full config over ESP-NOW (now implemented)
- [x] [Pull from Node] action — read back slave's stored state
- [x] [BLE OTA] action — hand off node to `opendash_bt_ota`
- [x] Safety-limits editor (overboost, EGT, AFR, fuel)
- [x] PID-gains editor (aKp/aKi/aKd, cKp/cKi/cKd + thresholds)

### 6.5 Operational

- [x] Fail-safe verification matrix (every safety cut tested individually)
- [x] "Boost slave missing" graceful behavior on center
- [x] OFF / NORMAL / RACE mode radio + per-gear override
- [x] Logging of boost decisions (PWM duty, active gain set, cuts) to SD

---

## 7. System Features

### 7.1 OTA (Over-The-Air) Updates

- [x] OTA partition layout (factory + ota_0 + ota_1) on all nodes
- [x] Centralised entry: `opendash_bt_ota_enter()` in [common/src/opendash_bt_ota.c](common/src/opendash_bt_ota.c)
- [x] BLE GATT service 0xFFB0: control / data / status / offset characteristics
- [x] Per-node OTA via `ota <node>` console command on center
- [x] Auto-RESUME from server-committed offset after mid-OTA disconnect
- [x] OTA partition state validates new image, rolls back on failure
- [x] Device-side completion timeout extended 5 → 30 min for slow links
- [x] Client `ble_ota.py`: 512 B default chunks, server-paced flow control, auto-RESUME
- [x] LEFT/RIGHT OTA proven (RIGHT 4 m 31 s @ 9.1 KB/s with 2M PHY)
- [x] POD1/POD2 OTA proven (pre-recipe; works but fragile)
- [ ] **GPS / POD1 / POD2: apply full hardening recipe** (see §1.2)
- [ ] Sub-5-min target verification on LEFT (post-rebuild measurement)
- [ ] HTTP OTA over WiFi SoftAP (alternative path, higher throughput)
- [ ] Per-node version reporting on BLE advert
- [ ] Mid-OTA progress mirrored to LCD UI (currently console only)
- [ ] Firmware image compression (LZ4) to halve transfer time
- [ ] BLE pairing / passkey to prevent unauthorised OTA in the wild
- [ ] **Android OTA client** — see [wiki/ota-android-plan.md](wiki/ota-android-plan.md)

### 7.2 NVS Configuration Persistence

- [x] Identity (`od_identity`) — node_type, with mismatch warning
- [x] Node health (`nd_health`) — per-node MAC + was_online
- [x] Layout (`od_layout`) — per-mode wire blob (m0–m7)
- [x] Boost (`boost`) — params, duty rows, setpoint rows, throttle curve
- [x] Boost UI (`boost_ui`) — target node, pressure unit
- [x] OBD config (`obd_cfg`) — thresholds, MIL toggle
- [x] Unit preferences
- [x] Odometer (with wear-level protection, both LEFT and RIGHT)
- [ ] Per-page gauge configuration persistence (converges with §5)
- [ ] Calibration data (sensor offsets, scaling)
- [ ] Factory reset console command (clear all NVS)
- [ ] NVS usage monitoring (% full)

### 7.3 Real-Time Clock (PCF85063)

- [x] Dual-API driver ([common/src/opendash_rtc.c](common/src/opendash_rtc.c)) — legacy `driver/i2c.h` (left/right) + new master (gps)
- [x] BCD encode/decode, OS bit validation, 24-h mode
- [x] System clock sync via `settimeofday()` at boot
- [x] LEFT + RIGHT + GPS wired in
- [-] Center: no PCF85063 on Waveshare 4.3" board (uses ESP-NOW time sync from GPS)
- [ ] GPS → RTC sync on valid fix (`gps_handler.c`)
- [ ] On-screen clock on center dashboard

### 7.4 Audio Subsystem

- [x] I2S WAV playback from SPIFFS ([common/src/opendash_audio.c](common/src/opendash_audio.c))
- [x] Priority queue (LOW/NORMAL/HIGH), interrupt support
- [x] 8-bit mono PCM @ 8 kHz, sound file index 0–63
- [x] I2S pins: BCK=GPIO13, WS=GPIO16, DOUT=GPIO17
- [x] FreeRTOS playback task (pri 3, 4 KB stack) — `audio_task()` loop with `xQueueReceive` → `i2s_channel_enable` → `play_wav_file` → `i2s_channel_write` complete
- [x] `OPENDASH_CMD_AUDIO_ALERT = 0x0B` opcode reserved
- [x] 8-bit → 16-bit upsample on the fly
- [~] WAV header parsing — basic mono/8-bit only; no stereo, no MP3
- [ ] Sound bank curated (alert tones, shift chime, warning ascending, BMS alarm)
- [ ] Wire `OPENDASH_CMD_AUDIO_ALERT` dispatch on receiving nodes
- [ ] Per-pod audio target routing

### 7.5 Power Management

- [ ] Graceful shutdown on power loss (save odometer)
- [ ] Ignition switch detection (auto power on/off)
- [ ] Display sleep mode (brightness step-down on timeout)
- [ ] Deep sleep with wake-on-CAN or wake-on-button

### 7.6 Diagnostics

- [x] UART diagnostic heartbeat (5-sec interval)
- [x] GPIO activity probe on UART RX at startup
- [x] Boot banner with identity validation (firmware/hardware match check)
- [ ] Built-in self-test (I2C bus scan, RAM, flash, sensor presence)
- [ ] Sensor plausibility checks (range validation per DP)
- [ ] Error counters (I2C NACK, CAN errors, watchdog resets)
- [ ] Serial console debug commands (`help`, `status`, `reset`, `nvs`)
- [ ] On-screen diagnostic mode (raw sensor values, frame stats)

---

## 8. Hardware & Wiring

### 8.1 Verified Pin Assignments

- [x] **GPIO20 (J9 DP / USB D+):** UART1 RX for HC-05 on LEFT.
      USB-Serial/JTAG PHY released via `usb_serial_jtag_ll_phy_enable_pad(false)`.
- [x] **GPIO43/44 (J10):** UART0 console. CH343P bridge holds GPIO44 permanently — unusable for external UART.
- [x] **Relay-8CH-A:** CH0–CH7 on GPIO 32,33,25,26,27,14,12,13 (verified by multimeter)
- [!] **GPIO16:** I2C slave SCL on 2.8C boards may collide with GT911 INT — schematic verification pending
- [~] **MOS-4CH-A channels 0–3:** GPIO TBD in source comments
- [~] **Relay-4CH-HD channels:** GPIO TBD
- [ ] Audit all GPIO assignments across all 11 board variants for conflicts
- [ ] Document verified assignments per board in [docs/hardware.md](docs/hardware.md)

### 8.2 Wiring Harness

- [ ] Design standardized wiring harness (connector pinout, gauge)
- [ ] EMI shielding for CAN and I2C in race environment
- [ ] Vibration-resistant connector selection (Deutsch, Molex MX)
- [ ] Power distribution diagram (12 V → 5 V → 3.3 V per node)

### 8.3 Enclosures

- [x] `dash-pods/opendash.FCStd` — FreeCAD mechanical design started
- [ ] 3D printed enclosure for center (4.3" + ESP32-S3 board)
- [ ] Pod mounts for left/right (2.8" round)
- [ ] GPS display mount (near windshield)
- [ ] Thermal design (ESP32-S3 + RGB DMA gets warm under load)

---

## 9. Testing

- [ ] Build verification: automated CI/CD across all 11 node projects
- [ ] Unit tests for common library (conversion, parsing, layout serialize)
- [ ] Integration test: simulated ESP-NOW traffic with channel manager
- [ ] Endurance test: 4-hour continuous operation, all nodes
- [ ] Temperature test: 0 °C to 60 °C (typical cabin)
- [ ] Vibration test (race conditions)
- [ ] BLE OTA reliability matrix per node, per host BT stack
- [ ] Boost controller fail-safe verification matrix (each cut tested)

---

## 10. Documentation

### 10.1 Up-to-Date

- [x] [PROJECT_INDEX.md](PROJECT_INDEX.md) — central project reference
- [x] [DATAFLOW.md](DATAFLOW.md) — end-to-end sensor data path (post-batching)
- [x] [PER_NODE_DISPLAY_CONFIG_SPEC.md](PER_NODE_DISPLAY_CONFIG_SPEC.md) — layout system spec
- [x] [boost-controller-opendash.md](boost-controller-opendash.md) — boost spec
- [x] [TEARING.md](TEARING.md) — center tearing investigation log
- [x] [BLE_OTA.md](BLE_OTA.md) + [BLE_OTA_ASSESSMENT.md](BLE_OTA_ASSESSMENT.md) — OTA recipe + deep assessment
- [x] [UART_CONNECTION.md](UART_CONNECTION.md) — MD UART protocol
- [x] [BLUETOOTH_PAIRING.md](BLUETOOTH_PAIRING.md) — HC-05/HC-06 pairing
- [x] [FEATURES_AND_SENSORS.md](FEATURES_AND_SENSORS.md) — sensor capability matrix
- [x] [wiki/LC76G-I2C-GPS-Driver-Guide.md](wiki/LC76G-I2C-GPS-Driver-Guide.md) v2.0.0
- [x] [wiki/LC76G-10Hz-Spec-Breakout.md](wiki/LC76G-10Hz-Spec-Breakout.md)
- [x] [wiki/boost-controller.md](wiki/boost-controller.md) — boost controller implementation details
- [x] [wiki/relay-mos-controllers.md](wiki/relay-mos-controllers.md) — relay & MOS FET controller documentation

### 10.2 Needs Update

- [x] [readme.md](readme.md) — Key Features lists now include all node types and features
- [x] [CHANGELOG.md](CHANGELOG.md) — now includes boost controller and parachute system entries
- [ ] [center/README.md](center/README.md) — outdated against current implementation
- [ ] [docs/architecture.md](docs/architecture.md) — verify against current node families
- [ ] [docs/hardware.md](docs/hardware.md) — add MOS / Relay / POD pin tables

### 10.3 Not Started

- [ ] API reference from Doxygen
- [ ] Wiring diagram (KiCad / Fritzing)
- [ ] User manual (non-developer audience)
- [ ] Installation walkthrough video
- [ ] Per-node README updates (pod1, pod2, mos-4ch-a, mos-4ch-b, relay-*)

### 10.4 New: openDstream ESP-NOW → USB Serial/JTAG Relay — DONE ✓

- [x] Create standalone headless project (`openDstream/`)
- [x] ESP-NOW slave receive callback (channel 6, passthrough all frames)
- [x] USB Serial/JTAG high-level driver API (`driver/usb_serial_jtag.h`)
- [x] Build verified: `openDstream.bin` (~267KB) compiles cleanly
- [x] Wiki documentation: [`wiki/opendstream-relay-node.md`](wiki/opendstream-relay-node.md)
- [x] No LVGL, no display, no common component — pure relay

### 10.5 openDstream Next Steps

- [ ] Flash test on actual ESP32-S3 hardware with USB-OTG
- [ ] Verify ESP-NOW frame reception and USB output with serial monitor
- [ ] Test integration with multidisplay-app PC-side
- [ ] Add bidirectional support (PC → ESP-NOW) if needed
- [ ] Consider TinyUSB CDC-ACM for higher throughput (future)

---

## 11. Future Vision

### 11.1 Companion Mobile App

- [ ] React Native or Flutter app for phone/tablet
- [ ] BLE connection to center
- [ ] Live data streaming to phone
- [ ] Drag-race timing (reaction time, 60 ft, 1/8, 1/4)
- [ ] Data review (graphs, lap overlays)
- [ ] Config sync (push layouts / boost maps from phone)
- [ ] Android OTA client — see [wiki/ota-android-plan.md](wiki/ota-android-plan.md)

### 11.2 Data Logging

- [x] GPS node SD logger (5 Hz GPS + IMU + engine DPs, SDMMC 1-line)
- [x] SPIFFS logger in common ([opendash_logger.c](common/src/opendash_logger.c)) fallback
- [~] Center SD logger ([sd_logger.c](center/main/sd_logger.c)) — **stub only**, needs CH422G SPI CS implementation
- [ ] Log format standardization (consider VESC Express CSV alignment)
- [ ] PC review software (or web-based viewer)
- [ ] GPS track overlay with data correlation
- [ ] In-car review of last session

### 11.3 Sensor Support — Status

> **DONE: MD-bridged sensors.** All MD analog/digital inputs flow into OpenDash.

- [x] RPM, boost/MAP, throttle, lambda, LMM, battery, 8× EGT,
      3× VDO temp, 3× VDO pressure, vehicle speed, gear, N75, requested boost,
      EFR speed, knock, case temp
- See [FEATURES_AND_SENSORS.md](FEATURES_AND_SENSORS.md), [UART_CONNECTION.md](UART_CONNECTION.md).

### 11.4 Advanced Display Features

- [ ] Animated arc transitions (smooth sweep on value change)
- [ ] Color themes / dark mode variants
- [ ] Night mode (reduced brightness, red tint)
- [ ] Predictive warnings (trending analysis)
- [ ] G-force display with historical trace (pod1/pod2 IMU ready)
- [ ] Digital tachometer with programmable shift points

### 11.5 Lap Tracking (BonoGPS-Inspired)

> See [docs/LAP_TRACKING_PLAN.md](docs/LAP_TRACKING_PLAN.md). All logic
> on-device (ESP32-S3), no mobile app dependency.

- [ ] u-blox GPS configuration via UBX commands (rate, GNSS, messages)
- [ ] Start/finish line definition (lat/lon pair in NVS)
- [ ] Line-crossing detection (perpendicular distance algorithm)
- [ ] Lap time with millisecond precision
- [ ] Best lap tracking (session + all-time via NVS)
- [ ] Real-time delta vs best lap (position-based interpolation)
- [ ] Sector boundaries (up to 8 per track)
- [ ] Sector splits with best-sector tracking
- [ ] Predictive lap time
- [ ] Track database on SD card (JSON, named tracks)
- [ ] GPS auto-config at boot (10–25 Hz, automotive dynamic model)
- [ ] Integration with rAtTrax-BMS Logger for shared lap data
- [ ] Lap data logging (timestamped position + all DPs)

### 11.6 OBDII / ELM327 via MultiDisplay

> MD firmware roadmap includes ELM327 support. OpenDash must be ready to
> parse expanded MD frames when shipped.

- [ ] Monitor MD firmware for OBDII payload additions
- [ ] Extend `parse_binary_frame()` for new fields
- [ ] Map OBD2 PIDs to existing OpenDash DP IDs
- [ ] Direct CAN/OBD2 on center as alternative (no MD dependency)

### 11.8 Channel Conflict Safeguards

> Implemented for boost, relay, and parachute systems on MOS-4CH-A/B nodes.

- [x] Channel ownership with conflict resolution via `prev_mask & ~mask` pattern
- [x] BLE OTA handler kills all PWM+FETs before entry to prevent conflicts
- [x] Parachute fire is idempotent (no effect if already fired)
- [x] ARM state is never NVS-persisted (always boots DISARMED)
- [x] Center MAC discovery filter excludes SYSTEM command to prevent GPS TIME_SYNC from hijacking relay/parachute unicast routing

### 11.7 Parachute Deployment System

> **Status: partially implemented.** Pod1/Pod2 broadcast IMU snapshots
> intended for voting, and MOS-4CH-A/B now have parachute config/arm/fire/status handling.

- [x] Sensor fusion: GPS speed + IMU deceleration + manual arm switch
- [x] Multi-pod IMU voting (require 2-of-3 G-force confirmation)
- [x] Deploy logic with deadman + safety arm
- [x] Output trigger (relay channel or dedicated GPIO)
- [x] Pre-flight checklist gate
- [x] Post-deploy lock-out + log
- [x] MOS-4CH-A/B now have parachute config/arm/fire/status handling
- [x] Channel conflict safeguards for shared GPIO channels (CH0=GPIO16, CH1=GPIO17, CH2=GPIO26, CH3=GPIO27)
- [x] ARM state is transient (never NVS-persisted — always boots DISARMED)
- [x] Config NVS-persisted for all system parameters

---

## 12. Completed Milestones (Historical)

### Initial scaffolding

- [x] Hardware init for all display types (ST7701S, ST7262, CO5300)
- [x] LVGL 9.2 integration with PSRAM draw buffers
- [x] ESP-NOW communication framework (replaced I2C due to GPIO conflicts)
- [x] Unit conversion system (temp, pressure, speed, distance)
- [x] Custom partition tables (2 MB app + OTA)
- [x] Warning system with flash overlay
- [x] Boot button screen cycling
- [x] Comprehensive documentation (PROJECT_INDEX.md, per-node READMEs)

### Beta — March 2026 (v0.2.0-beta)

- [x] MultiDisplay live-data end-to-end via HC-05 → GPIO20 → LEFT → ESP-NOW → CENTER
- [x] Binary frame parser (95-byte SERIALOUT_BINARY)
- [x] All 8 EGT channels + RPM + boost + lambda + LMM + battery + VDO sensors forwarded
- [x] `opendash_uart.c/h`, `opendash_data_model.h` (EGT1–EGT8 at 0x0112–0x011B)
- [x] UART_CONNECTION.md / SERIAL_PROTOCOL.md / BLUETOOTH_PAIRING.md / FEATURES_AND_SENSORS.md

### v0.3.0-beta — July 2026

- [x] PCF85063 RTC driver (dual-API, wired into LEFT/RIGHT/GPS)
- [x] VESC data model (25 DPs in 0x0600 range + wheel speed)
- [x] BMS data model extended (SOH, IC temp, balance, charging, energy)
- [x] GPS SD card logging wired in (5 Hz snapshots)
- [x] Boot button long-press → odometer reset on both LEFT and RIGHT (3-beep, NVS)
- [x] vesc_express CAN protocol research (STATUS 1–6, BMS, GNSS, IO frames)

### v0.4.0-beta — May 2026 (BLE OTA on round pods)

- [x] LEFT/RIGHT BLE OTA recipe complete (sdkconfig + suspend + RGB teardown)
- [x] 2M PHY + PPCP tuning → RIGHT 4 m 31 s @ 9.1 KB/s
- [x] Auto-RESUME mid-OTA via persisted offset
- [x] Centralised `opendash_bt_ota_enter()`
- [x] `ota <node>` console command on center
- [x] ESP32-S3 BLE addr-type bug fix (`s_own_addr_type` cache)

### v0.5–v0.8.x (Batched ESP-NOW + Channel Manager + 18-Node Framework)

- [x] DATA_BATCH (0x88) + SET_DATA_BATCH (0x0C) opcodes
- [x] 4-channel priority architecture (CRITICAL/MEDIUM/LOW/CONTROL)
- [x] Per-channel timeouts, retries, quarantine pause/resume
- [x] Dispatcher task + per-channel worker tasks
- [x] `node_health.c` 5-state machine + multi-layer detection
- [x] `node_definitions.h` + `opendash_identity.c` (18 node types, MAC serial, NVS validation)
- [x] LEFT batches MD frames at 5 pps (was per-DP)
- [x] Polling eliminated — event-driven push + data-absence offline detection
- [x] `STATUS_REPORT` (0x82) + `BLE_OTA` flag latch in `node_health_set_status_flags`

### v0.9.x-in-progress (Per-Node Layout + Boost Staging + New Nodes)

- [x] Per-node layout system Phase 1 (wire format + NVS + DP catalog)
- [x] Layout editor UI on center
- [x] Boost controller shared algorithm layer (`opendash_boost.c`, PID + safety + NVS)
- [x] MOS-4CH-A boost integration (init, compute task, PWM channel)
- [x] Boost config UI scaffold on center
- [x] POD1 / POD2 introduced (auxiliary AMOLED displays + IMU)
- [x] Relay-8CH-A GPIO verification + self-test mode
- [x] Audio subsystem (I2S WAV from SPIFFS, priority queue)
- [x] `lv_mem_psram.c` PSRAM-routed LVGL allocator on center
- [x] Center system_config NVS (boost target, pressure unit)

---

*OpenDash — Built for racers, by racers.*
