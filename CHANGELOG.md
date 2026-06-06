<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OpenDash Changelog

## [v0.4.1-beta] - 2026-06 — OTA Hardening, Full-Fleet Coverage & Protocol Polish

### Fixed — LEFT sdkconfig: wrong PPCP symbol prefix (silent throughput regression)

LEFT's initial OTA fix used the wrong sdkconfig key prefix
`CONFIG_BT_NIMBLE_PPCP_*` — silently ignored by ESP-IDF v6.1. The correct
prefix is `CONFIG_BT_NIMBLE_SVC_GAP_PPCP_*` (via the IDF `BT_NIMBLE_SVC_GAP`
Kconfig group). Result: BlueZ never read the Peripheral Preferred Connection
Parameters, so the connection interval stayed at its default 60 ms → ~3.5 KB/s
throughput. RIGHT had always used the correct prefix and achieved 9.1 KB/s.

LEFT rebuilt with full corrected config (`left/sdkconfig.defaults`):
```
CONFIG_BT_NIMBLE_SVC_GAP_PPCP_MIN_CONN_INTERVAL=6    # 7.5 ms
CONFIG_BT_NIMBLE_SVC_GAP_PPCP_MAX_CONN_INTERVAL=12   # 15 ms
CONFIG_BT_NIMBLE_SVC_GAP_PPCP_SLAVE_LATENCY=0
CONFIG_BT_NIMBLE_SVC_GAP_PPCP_SUPERVISION_TMO=3200   # 32 s
CONFIG_BT_NIMBLE_LL_CFG_FEAT_LE_2M_PHY=y             # 2× throughput
CONFIG_BT_NIMBLE_ACL_BUF_COUNT=32
CONFIG_BT_CTRL_PINNED_TO_CORE_1=y
CONFIG_BT_NIMBLE_PINNED_TO_CORE_1=y
```
Expected throughput post-rebuild: ~9 KB/s (≈5 min for 2.4 MB image, matching RIGHT).

### Added — `ble_ota.py` expanded command set

| Flag | Purpose |
|---|---|
| `--list` / `-l` | Scan only; print all `OpenDash-*-OTA` advertisers with RSSI, then exit. Use before flashing to confirm the node is advertising. |
| `--device PREFIX` / `-d` | Connect to first advertiser whose name starts with this prefix (e.g. `-d "OpenDash-MOS"`) instead of using the `--node` alias. |
| `--address MAC` / `-a` | Skip scan entirely; connect by known BLE MAC. Fastest path when MAC is known. |
| `--scan-timeout SEC` | Override the default 20 s scan window. |
| `--chunk-size N` | Bytes per BLE write (default 512, maximum negotiable). Do not reduce below 128. |

### Added — Full-fleet BLE OTA coverage

All 11 node families now document BLE OTA: center, left, right, gps, pod1, pod2,
relay-4ch-hd, relay-8ch-a, relay-8ch-b, mos-4ch-a, mos-4ch-b.

`BLE_OTA.md` completely rewritten to cover the full fleet:
- **Two trigger paths**: CENTER touch UI (Config → OTA Flash, preferred) and
  `ota <node>` console command (fallback / developer workflow).
- Per-node firmware binary path table.
- Complete GATT service reference (service `0x00FF`, characteristics `0xFF01`
  CTRL / `0xFF02` DATA / `0xFF03` STATUS, all state codes).
- `--list` discovery workflow — scan before you flash.
- Troubleshooting table covering all known failure modes.

### Fixed — CENTER demo data path verified safe

Verified that `OPENDASH_DATASRC_DEMO` does not feed the live gauge display
when MD / ESP-NOW sensor data is present. The demo generator does not
interfere with production sensor data in normal operation.

### In Progress — OTA handler divergence audit (GPS / POD1 / POD2)

Systematic comparison of `ENTER_BT_OTA` handling across all nodes:

| Node | CPU1 pinning | Correct PPCP prefix | 2M PHY | Suspend sequence | Status |
|---|---|---|---|---|---|
| LEFT | ✅ | ✅ (rebuilt Jun 2026) | ✅ | ✅ | **Hardened** |
| RIGHT | ✅ | ✅ | ✅ | ✅ | **Hardened** |
| GPS | ❌ | ❌ | ❌ | ❌ | Fragile — needs hardening |
| POD1 | ❌ | ❌ | ❌ | ❌ | Fragile — needs hardening |
| POD2 | ❌ | ❌ | ❌ | ❌ | Fragile — needs hardening |
| relay/mos | N/A (no RGB DMA) | Partial | N/A | N/A | Works reliably as-is |

GPS / POD1 / POD2 work in practice (no RGB DMA contention) but are at risk
of intermittent disconnects under high CPU load. Hardening tracked in TODO §1.2.

### Known issues — targeted for v0.5.0

- **CENTER Config page OTA status**: per-node online indicator and OTA button
  active state not reliably reflecting live `node_health` state in the CONFIG
  screen (`center/main/ui_manager.c` ~L3509–3598).
- **Multi-click OTA race**: issuing `ota <node>` twice (or tapping the UI
  button twice) fires a second `ENTER_BT_OTA` burst mid-BT-init on the slave,
  causing an unpredictable state. A 10 s per-node lockout is needed in
  `center/main/espnow_master.c`.

---

## [v0.4.0-beta] - 2026-05 — BLE OTA on Round Pods (LEFT/RIGHT)

### Fixed — BLE OTA on RGB-driven displays (LEFT & RIGHT)
For weeks, BLE OTA worked on POD1/POD2/GPS but the round 480×480 pods
silently dropped the BLE link on connect, before NimBLE could log
`BLE connected`. Root cause was a **three-way starvation**: the RGB LCD
bounce-buffer DMA on CPU0 saturated the PSRAM bus, LVGL+UART tasks added
CPU contention, and the BT controller's link-layer ISR could not service
the HCI completion in time. BlueZ reported "Connected" optimistically,
then dropped the link a few seconds later.

The fix is a recipe — all parts are required, none alone is sufficient:

1. **OTA-mode shutdown sequence** in `ENTER_BT_OTA` handler:
   `ui_manager_suspend()` → `opendash_uart_suspend()` (LEFT only) →
   `display_pause_for_ota()`. The display function now calls
   `esp_lcd_panel_del()` and nulls the handle — it does not just blank
   the backlight. This frees the bounce DMA before BT initialises.
2. **Pin BT controller + NimBLE host to CPU1**:
   `CONFIG_BT_CTRL_PINNED_TO_CORE_1=y`,
   `CONFIG_BT_NIMBLE_PINNED_TO_CORE_1=y`. Without this, link-layer ISRs
   are starved by residual CPU0 activity.
3. **Throughput-optimised PPCP + LE 2M PHY + larger ACL pool**:
   `CONFIG_BT_NIMBLE_SVC_GAP_PPCP_*` (7.5 ms–15 ms interval, 32 s
   supervision), `CONFIG_BT_NIMBLE_LL_CFG_FEAT_LE_2M_PHY=y`,
   `CONFIG_BT_NIMBLE_ACL_BUF_COUNT=32`. The **2M PHY** is the biggest
   single win.
4. **Device-side OTA completion timeout** in `common/src/opendash_bt_ota.c`
   bumped from 5 min → 30 min to tolerate slow links and mid-OTA stalls.
5. **Client (`ble_ota.py`):** default chunk size doubled to 512 B (max
   negotiable), connect-retry with auto-RESUME from last server offset.

### Performance
| Pod | Bytes | Wall time | Throughput |
|---|---|---|---|
| LEFT  | 2,458,176 | ~11 min 40 s | ~3.5 KB/s (no 2M PHY) |
| RIGHT | 2,425,520 | **4 min 31 s** | **~9.1 KB/s (2M PHY enabled)** |

LEFT has now been rebuilt with the same RIGHT-class config and is
expected to match the ~4-5 min figure on next OTA.

### Operational
- New CENTER console command: `ota right` (and `ota left`, `ota gps`)
  sends `ENTER_BT_OTA` x3 over ESP-NOW.
- `ble_ota.py --node {left,right,gps,pod1,pod2}` auto-scans for the
  advertising name `OpenDash-<NODE>-OTA`; no MAC needed.
- Mid-OTA disconnects are handled: re-running the client picks up at
  the last verified offset via the OTA partition's persisted state.

### Reference
- Full recipe and root-cause analysis: [`BLE_OTA.md`](./BLE_OTA.md)
- New end-user wiki guide: [`wiki/ota-bluetooth.md`](./wiki/ota-bluetooth.md)
- Android OTA plan: [`wiki/ota-android-plan.md`](./wiki/ota-android-plan.md)

---

## [v0.3.0-beta] - 2026-07 — RTC, VESC Data Model, SD Logging, Boot Button

### Added — PCF85063 Real-Time Clock Driver
- **`opendash_rtc.c/h`** in common/ — dual-API driver supporting both
  legacy `driver/i2c.h` (left/right) and new `driver/i2c_master.h` (GPS).
- BCD encode/decode, OS bit validation, 24-hour mode, system clock sync
  via `settimeofday()`. Probes I2C address 0x51 at init.
- **Left + Right:** `opendash_rtc_init(I2C_MASTER_PORT)` → sync system clock.
- **GPS:** `opendash_rtc_init_master(display_get_i2c_handle())` → sync.
- **Center:** No PCF85063 on 4.3" Waveshare board (uses ESP-NOW time sync).

### Added — VESC & BMS Data Model (opendash_data_model.h)
- Full **VESC 0x0600 range** (25 data points): eRPM, current, duty, Ah,
  Ah_charged, Wh, Wh_charged, FET temp, motor temp, current_in, PID pos,
  tacho, V_in, ADC1-3, PPM, RPM, power_in, power_motor, fault code.
- **Wheel speed:** FL/FR/RL/RR RPM (0x0620-0x0623) + average (0x0624).
- **BMS extended block:** SOH (0x0409), IC temp (0x040A), balance status
  (0x040B), charging state (0x040C), energy charged (0x040D).

### Added — GPS SD Card Logging
- Wired `sd_logger.c` into GPS CMakeLists + main.c (was orphaned code).
- Added `fatfs` and `sdmmc` components to GPS build.
- SD logger init/start in app_main, `sd_logger_log_snapshot()` called in
  data_broadcast_task at 5 Hz with GPS + IMU + engine data.
- SDMMC 1-line mode: CLK=GPIO2, CMD=GPIO1, D0=GPIO3.

### Added — Boot Button Long Press (Right Pod)
- Replicated left's 5-second long press → odometer reset to right pod.
- `button_long_press_cb_t` typedef + `display_register_long_press_cb()` API.
- `button_read_task` upgraded from simple debounce to held-counter approach
  with long press detection (500 ticks × 10ms = 5s).
- 3-beep buzzer feedback + immediate NVS save + UI update.

### Changed — Common Component Build
- `esp_driver_i2c` added to common REQUIRES (for RTC header `driver/i2c_master.h`).

### Documentation
- TODO.md updated with RTC, VESC, SD logging, MD Hz findings.
- rAtTrax-BMS TODO.md updated with complete OpenDash data point ID mapping
  table (30+ entries) and concrete ESP-NOW integration plan.
- VESC CAN IDs in BMS project flagged as non-standard (0x401/0x402 vs
  standard extended EID format).

---

## [v0.2.0-beta] - 2026-03-20 — MultiDisplay Integration / Beta Release

> First end-to-end sensor data flowing from MultiDisplay hardware through
> Bluetooth to all OpenDash display units. Project officially enters Beta.

### Added — MultiDisplay UART Integration
- **Binary frame parser** (`opendash_uart.c`) — Parses SERIALOUT_BINARY
  95-byte frames: STX(0x02) TAG(0x5F) payload[93] ETX(0x03). Little-endian
  AVR field extraction via `rd_u16()`, `rd_s16()`, `rd_u32()` helpers.
- **GPIO20 (USB D+) UART RX** — Reclaimed J9 DP pin from USB-Serial/JTAG
  PHY via `usb_serial_jtag_ll_phy_enable_pad(false)`. ROM bootloader
  re-enables automatically for flashing — zero user impact.
- **All 8 EGT channels** parsed, forwarded to Center, and pushed to UI.
  Data point IDs: EGT1-EGT4 (0x0112-0x0115), EGT5-EGT8 (0x0118-0x011B).
- **Full sensor extraction:** RPM, boost, throttle, lambda, LMM, battery
  voltage, 3×VDO temperature, 3×VDO pressure, vehicle speed, gear,
  N75 duty cycle, requested boost, EFR turbo speed, knock, case temp.
- **Connection state machine:** WAITING → RECEIVING → TIMEOUT with
  automatic HC-05 reconnect attempt on data loss.
- **UART diagnostic heartbeat** — 5-second interval logs byte count,
  STX detection count, and connection status.
- **GPIO activity probe** — Startup diagnostic samples RX pin 2000 times
  to confirm signal presence before UART configuration.
- **Toggleable debug logging** — `OPENDASH_UART_DEBUG` compile flag in
  `opendash_uart.h` (0=production quiet, 1=verbose per-frame output).
- `esp_hal_usb` added to common component PRIV_REQUIRES.

### Added — ESP-NOW Data Relay
- Left gauge pod now forwards all MD data points to Center via ESP-NOW
  DATA_RESPONSE messages (previously only EGT1 and EGT2).
- Center receives and displays: RPM (sweeps on arc), boost, EGTs, lambda,
  battery voltage, oil pressure/temperature, mass air flow.
- Center forwards received data to Right gauge pod.

### Added — Documentation
- `UART_CONNECTION.md` — Complete rewrite. GPIO20 pin assignment, full
  binary protocol byte map with offset defines, HC-05 wiring, data flow
  architecture diagram, receiver state machine description.
- `SERIAL_PROTOCOL.md` (in multidisplay repo) — Canonical 400+ line binary
  protocol specification: complete byte map, parsing pseudocode, scaling
  tables, EGT details, build variant differences, receiver state machine.
- `BLUETOOTH_PAIRING.md` — End-user guide for HC-05 master + HC-06 slave
  Bluetooth pairing with AT command sequences.
- `FEATURES_AND_SENSORS.md` — Complete sensor capability matrix showing
  everything MultiDisplay provides and OpenDash can display.
- `docs/LAP_TRACKING_PLAN.md` — Detailed design for BonoGPS-inspired
  on-device lap timing: line crossing detection, sector timing, predictive
  lap delta, u-blox UBX configuration.
- Updated `TODO.md` — Comprehensive overhaul: checked off completed items,
  added §1.3 display flicker fix, §1.4 Bad ETX frame loss, §5.2 UART
  protocol section, §5.4 VESC/BMS integration, §10.3 sensor matrix,
  §10.5 lap tracking plan, §10.6 OBDII/ELM327 future.

### Changed
- `OPENDASH_UART_DEBUG` default: 1 → 0 (eliminates 100Hz log spam that
  caused left display rendering flicker).
- "STX+TAG detected" log level: `ESP_LOGI` → `ESP_LOGD`.
- EGT max calculation: was `max(egt[0], egt[1])`, now scans all 8 channels.
- `forward_md_data_to_center()`: expanded from 2 EGTs to all 8.
- Center demo data: engine demo auto-halts when real MD sensor data arrives
  via ESP-NOW (GPS demo already had this pattern for GPS node).

### Fixed
- **Left display flicker/blanking** — Caused by `ESP_LOGI` on every parsed
  frame at 100Hz (~100 log prints/second saturating UART0 console and
  starving LCD refresh). Fixed by setting `OPENDASH_UART_DEBUG=0`.
- **GPIO44 bus contention** — CH343P UART-to-USB bridge on J10 permanently
  drives GPIO44 HIGH, preventing external UART input. Switched to GPIO20.
- **Missing EGT channels** — Only EGT[0] and EGT[1] were forwarded and
  displayed. Now all 8 channels forwarded, displayed, and logged.

## [Unreleased]

### Added — GPS I2C Driver (v15L2 Production)
- **LC76G GPS I2C driver** — Stable production code (`gps_handler.c` v15L2)
- CASIC I2C protocol implementation: 0x50 (write), 0x54 (read), 0x58 (data write)
- Six-mechanism I2C communication stack:
  - PMIC power cycle (5s off / 5s boot wait)
  - I2C WAKE (CW config + 0x58 dummy write)
  - Activation via `transmit_receive(0x50)`
  - **Primer** (TxRx + data_req + drain read) — at boot AND recovery
  - **Per-read WAKE** (CW+0x58 after every successful data read)
  - **Re-prime + drain** on RX failures — prevents avail poisoning
- Automatic recovery: PMIC power cycle after 100 consecutive RX failures
- Bus reset with 50 ms delay (tuned optimal for shared bus)
- NMEA parsing: GGA, RMC, GSV, GSA, GLL, VTG, TXT, PQTM
- 3D Fix verified: 9-16 sats, HDOP 0.7-1.1, continuous 100-125s flow
- 62,000-95,000 bytes per 5-minute run

### Added — Documentation
- `wiki/LC76G-I2C-GPS-Driver-Guide.md` v2.0.0 — sole authoritative GPS reference
- `wiki/LC76G-10Hz-Spec-Breakout.md` — 10 Hz logging capability analysis
- Cross-reference annotations in `gps_handler.c` → wiki guide sections
- ARCHIVE headers on 11 historical/deprecated files
- Comprehensive troubleshooting: [2C2C2C2C], [4D4D4D4D], bus degradation

### Changed
- `gps/README.md` — Complete rewrite (UART→I2C, correct addresses, CST9217)
- `gps/INTENSIVE_TODO.md` — Fixed I2C addresses (0x28/0x2A → 0x50/0x54/0x58)
- `docs/hardware.md` — GPS pins corrected (UART→I2C CASIC)

### Added
- Enhanced UI implementation for Left/Right gauge pods
- Real-time data point display capability
- Proper I2C data handling for all display units
- Support for multiple data points per unit
- Dynamic unit formatting based on data type
- Arc gauge visualization for data points

### Changed
- Left/Right UI manager now properly handles data updates from Center unit
- Replaced placeholder UI with functional data display
- Improved data point mapping and formatting
- Enhanced error handling and logging

### Fixed
- Data display now updates properly with real values from I2C system
- Arc gauge now responds to data point values
- UI layout now properly displays data point information

## [v0.1.0] - 2026-02-21

### Added
- Initial project structure and baseline implementations
- Center display with multi-mode UI
- GPS/Telemetry unit with specialized layouts
- Left/Right gauge pods with basic UI
- I2C communication protocol implementation
- Data model and display configuration system

### Changed
- Project documentation and README files

### Fixed
- Initial build and compilation issues