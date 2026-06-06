<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OpenDash Boost Controller

> **Heritage credit:** the runtime + map layout are direct descendants of the
> `RPMBoostController` written by Stephan Martin & Dominik Gummel for the
> [MultiDisplay](../multidisplay-firmware/multidisplay/) project (GPL-3.0).
> Wire format is intentionally compatible so MultiDisplay-era maps can be
> ported with zero loss.

## 1. Theory of Operation

The boost controller is a distributed closed-loop PWM driver for an electronic
boost-control solenoid (N75 / MAC valve). The control loop **lives on the
slave** (mos-4ch-a by default) so a stalled or rebooted center display can
never cause a boost spike. The center is a thin "map author + data fountain".

```
   ┌─────────────────────────┐         ESP-NOW (≥10 Hz LIVE_DATA)
   │      Center Display     │ ────────────────────────────────┐
   │   - LVGL map editor      │                                 ▼
   │   - DP cache (engine)    │            ┌──────────────────────────────┐
   │   - NVS (UI prefs)       │            │       MOS-4CH-A (slave)       │
   │                          │ ◄──────── │  - opendash_boost_compute()  │
   │   - Receives 5 Hz telem  │  TELEM    │    @ 50 Hz                    │
   └─────────────────────────┘            │  - PID overlay (a/c gains)   │
                                          │  - Safety overlay (overboost,│
                                          │    EGT, AFR, fuel, stale,    │
                                          │    throttle, mode==OFF)      │
                                          │  - LEDC PWM → N75 solenoid   │
                                          │  - NVS persist (params+maps) │
                                          └──────────────────────────────┘
```

Closed-loop rate: **50 Hz** (`boost_compute_task`).
Live data refresh: **10 Hz** (`boost_live_push_task`).
Telemetry feedback: **5 Hz** (`boost_telemetry_task`).
Stale-data lockout: any LIVE_DATA frame older than
`OPENDASH_BOOST_DATA_TIMEOUT_MS` (600 ms) forces duty → 0.

## 2. Hardware Wiring

| Signal               | Slave    | Pin                                    |
| -------------------- | -------- | -------------------------------------- |
| MOS Channel 0..3 PWM | MOS-4CH-A | per board (see `mos-4ch-a/main/main.c`) |
| N75 solenoid +12 V  | Vehicle  | Switched ignition                       |
| N75 solenoid return  | MOS ch.  | low-side switched by MOS                |

The default `output_channel` in `opendash_boost_default_params()` is **0**.
Change it via the System Config UI or by sending `BOOST_SET_PARAMS`.

## 3. Modes

`OPENDASH_BOOST_MODES = 2` map slots + an explicit OFF state.

| Mode   | Enum                          | Slot | Default peak setpoint        | Behaviour                                                          |
| ------ | ----------------------------- | ---- | ---------------------------- | ------------------------------------------------------------------ |
| OFF    | `OPENDASH_BOOST_MODE_OFF`     | —    | n/a                          | Output forced to 0 (wastegate fully open), `SAFE_MODE_OFF` bit set |
| NORMAL | `OPENDASH_BOOST_MODE_NORMAL`  | 0    | ~0.97 BAR (~14 PSI)          | Conservative "safe street" map, on by default after first boot     |
| RACE   | `OPENDASH_BOOST_MODE_RACE`    | 1    | ~2.48 BAR (~36 PSI)          | Aggressive map; spool fast, sustain near peak through redline      |

Switching modes pushes a single byte (`OPENDASH_CMD_BOOST_SET_MODE = 0x22`).
The slave hard-cuts at `overboost_bar = 2.80 BAR (~40 PSI)` regardless of slot.
Defaults are regenerated automatically whenever the slave reads a params blob
with a `version` field that does not match `OPENDASH_BOOST_PARAMS_VERSION = 2`.

## 4. Map Structure

Each `(mode, gear)` combination holds **two** 32-cell rows:

* **Duty row** — open-loop seed duty (`uint8_t` 0..255) at each RPM bin.
* **Setpoint row** — target manifold pressure in centi-bar (`uint16_t`).

RPM bins span `0 .. OPENDASH_BOOST_RPM_MAX` (**16 000**) linearly across
`OPENDASH_BOOST_MAP_POINTS` (**32**) cells, so bin spacing is
`16000 / 31 ≈ 516 RPM`.

A complete map set is therefore `2 modes × 6 gears = 12` duty rows + 12
setpoint rows + 1 throttle reduction curve + 1 params blob.

### Pull / push protocol

| Direction | CMD                              | Opcode | Payload                             |
| --------- | -------------------------------- | ------ | ----------------------------------- |
| C→S       | `BOOST_LIVE_DATA`                | `0x20` | `opendash_boost_live_t` (≥10 Hz)    |
| C→S       | `BOOST_SET_PARAMS`               | `0x21` | `opendash_boost_params_t`           |
| C→S       | `BOOST_SET_MODE`                 | `0x22` | `uint8_t mode`                      |
| C→S       | `BOOST_SET_DUTY_ROW`             | `0x23` | `opendash_boost_duty_row_t`         |
| C→S       | `BOOST_SET_SETP_ROW`             | `0x24` | `opendash_boost_setpoint_row_t`     |
| C→S       | `BOOST_SET_THROTTLE`             | `0x25` | `opendash_boost_throttle_curve_t`   |
| C→S       | `BOOST_PULL_ALL`                 | `0x26` | empty — slave echoes every map row  |
| S→C       | `BOOST_TELEMETRY`                | `0x90` | `opendash_boost_telemetry_t` (≥5 Hz)|
| S→C       | `BOOST_PARAMS_REPORT`            | `0x91` | `opendash_boost_params_t`           |
| S→C       | `BOOST_DUTY_REPORT`              | `0x92` | `opendash_boost_duty_row_t`         |
| S→C       | `BOOST_SETP_REPORT`              | `0x93` | `opendash_boost_setpoint_row_t`     |
| S→C       | `BOOST_THROTTLE_REPORT`          | `0x94` | `opendash_boost_throttle_curve_t`   |

All frames go over ESP-NOW (250-byte max). The opcodes live in
`opendash_i2c_protocol.h` because that header is the shared wire-format
catalogue — the actual transport is `opendash_espnow_send()`.

## 5. Safety Flags

Reported in `opendash_boost_telemetry_t.safety_flags`:

| Bit | Macro                         | Action when set            |
| --- | ----------------------------- | -------------------------- |
| 0   | `OPENDASH_BOOST_SAFE_OVERBOOST` | Output × 0.5             |
| 1   | `OPENDASH_BOOST_SAFE_EGT_WARN`  | Output × 0.75            |
| 2   | `OPENDASH_BOOST_SAFE_EGT_CRIT`  | Output × 0.5             |
| 3   | `OPENDASH_BOOST_SAFE_AFR_LEAN`  | Output → 0               |
| 4   | `OPENDASH_BOOST_SAFE_FUEL_LOW`  | Output → 0               |
| 5   | `OPENDASH_BOOST_SAFE_DATA_STALE` | Output → 0              |
| 6   | `OPENDASH_BOOST_SAFE_THROTTLE`  | Output → 0 (below gating) |
| 7   | `OPENDASH_BOOST_SAFE_MODE_OFF`  | Output → 0               |

Multiple flags compound multiplicatively where they attenuate.

## 6. NVS Layout

| Namespace  | Key             | Owner   | Contents                                |
| ---------- | --------------- | ------- | --------------------------------------- |
| `"boost"`  | `params`        | slave   | `opendash_boost_params_t` blob (incl. version 2)  |
| `"boost"`  | `d_<m>_<g>`     | slave   | 32 × `uint8_t`                                    |
| `"boost"`  | `s_<m>_<g>`     | slave   | 32 × `uint16_t`                                   |
| `"boost"`  | `throt`         | slave   | `opendash_boost_throttle_curve_t` (32 points)     |
| `"boost_ui"` | `boost_target` | center | `opendash_node_t` target slave node                |
| `"boost_ui"` | `page`        | center  | `uint8_t` last-viewed editor page (0 or 1)         |

## 7. Authoring Maps via the UI

1. Open **System Config → Boost** on the Center display.
2. Verify **Target node** — switch between `MOS_4CH_A` / `MOS_4CH_B`. The
   selection persists to NVS (`boost_ui/boost_target`).
3. Press **Pull from slave** if the cells read all-zero — this triggers
   `BOOST_PULL_ALL` (0x26), prompting the slave to echo every map row back.
4. Pick **Mode** (OFF / NORMAL / RACE) and **Gear** (1..6).
5. The 32-point map is split into two editor pages of 16 cells:
   * **Page 1** — 0 … ~7 750 RPM (spool ramp)
   * **Page 2** — ~8 000 … 16 000 RPM (sustain + redline taper)
   Tap the **Page 1 / Page 2** toggle (top of the cell grid) to switch.
   The last-viewed page persists to NVS (`boost_ui/page`).
6. Tap a Duty or Setpoint cell to bump it `+5`. Long-press to bump `-5`.
7. The edit is unicast to the slave immediately. The slave's next 5 Hz
   telemetry frame should reflect the change.

## 8. Calibration Procedure

1. Set mode **OFF** while the engine is at idle. Confirm `duty = 0` and
   `SAFE_MODE_OFF (0x80)` in the telemetry strip.
2. Switch to **NORMAL** at low load. Setpoint should track without
   overshooting beyond 0.05 BAR. NORMAL caps near 14 PSI by default.
3. Increase throttle through the RPM range while observing the live
   gauge. If the engine hits overboost the `OVERBOOST` flag should
   light and duty should collapse — verify before extending the map.
4. Tune the Duty row first (open-loop seed), then trim with the
   Setpoint row + PID overlay. Walk Page 1 (spool) before Page 2 (sustain).
5. Repeat per gear, then promote a proven set to **RACE** by raising
   the setpoint row (default RACE peak ≈ 2.48 BAR / 36 PSI).
6. Overboost cut is `params.overboost_bar = 2.80 BAR (~40 PSI)` — this
   is the absolute ceiling regardless of mode.

## 9. Pre-Configured Defaults

On first boot (or whenever the persisted params blob fails the
`version == OPENDASH_BOOST_PARAMS_VERSION` check) the slave regenerates
all maps from the defaults baked into `common/src/opendash_boost.c`:

| Field                       | Default                                  |
| --------------------------- | ---------------------------------------- |
| `mode`                      | `NORMAL`                                 |
| `use_pid`                   | `1`                                      |
| `output_channel`            | `0` (MOS channel 0)                      |
| `aKp / aKi / aKd`           | `4.0 / 1.0 / 0.20` (aggressive)          |
| `cKp / cKi / cKd`           | `1.0 / 0.25 / 0.05` (conservative)       |
| `aggressive_threshold`      | `0.50` (error in BAR)                    |
| `conservative_threshold`    | `0.85`                                   |
| `overboost_bar`             | `2.80` BAR (~40 PSI hard cut)            |
| `egt_warn_c / critical_c`   | `880 / 950 °C`                           |
| `afr_lean_limit`            | `16.0`                                   |
| `fuel_pressure_min_kpa`     | `200`                                    |
| `throttle_min_pct`          | `25 %`                                   |
| `rpm_min`                   | `2000`                                   |
| NORMAL peak setpoint        | ~0.97 BAR (~14 PSI), held 4 k…6.5 k RPM   |
| RACE peak setpoint          | ~2.48 BAR (~36 PSI), held 5 k…7.5 k RPM   |
| Duty `gear_bias`            | `+6` per gear (low gears traction-limited) |
| Duty `mode_bias` (RACE)     | `+30` PWM counts on top of NORMAL shape  |
| Throttle reduction          | `0 % @ TPS 0`, ramps to `100 % @ TPS 30 %`|

See the duty/setpoint shape tables in `opendash_boost_default_duty_row()`
and `opendash_boost_default_setpoint_row()` for the full 32-point curves.

## 10. BLE OTA

The slave runs the shared `opendash_bt_ota` stack. To enter OTA mode
without a button, send `OPENDASH_CMD_SYSTEM` (`0x07`) with sub-cmd
`OPENDASH_SUBCMD_ENTER_BT_OTA` (`0x06`). Both `mos-4ch-a` and
`mos-4ch-b` advertise as `OpenDash-MOS-A` / `OpenDash-MOS-B`.

Quick flash:

```bash
idf.py -C mos-4ch-a build
python ble_ota.py --device-name OpenDash-MOS-A \
                  --binary mos-4ch-a/build/mos-4ch-a.bin
```

Service UUID: `0x00FF` — characteristics `0xFF01..0xFF03` (control / data
/ status). See `BLUETOOTH_PAIRING.md` for the chunk protocol and the
**Flashing a MOS Boost Controller via BLE OTA** appendix in
`boost-controller-opendash.md` for the full procedure (including the FTDI
fallback path).

## 11. Troubleshooting

| Symptom                              | Likely cause                              | Action                                                              |
| ------------------------------------ | ----------------------------------------- | ------------------------------------------------------------------- |
| `duty=0`, `flags=0x20` (`DATA_STALE`) | LIVE_DATA not arriving                    | Confirm center is online; check `g_boost_target_node`               |
| `duty=0`, `flags=0x40` (`THROTTLE`)   | TPS below `throttle_min_pct`              | Lower threshold via `BOOST_SET_PARAMS` or floor it                  |
| `duty=0`, `flags=0x80` (`MODE_OFF`)   | Mode is OFF                               | Send `BOOST_SET_MODE` with NORMAL or RACE                           |
| Cells read all 0 after Pull          | Slave hasn't persisted maps (cold flash)  | First boot regenerates defaults; re-Pull after a power cycle        |
| Cells reset to defaults unexpectedly | Params version mismatch (`!= 2`)          | Re-flash latest `common` so `OPENDASH_BOOST_PARAMS_VERSION` agrees  |
| Setpoint never reached                | Open-loop duty too low                    | Increase Duty row in the active gear/RPM band; verify PID enabled   |
| Overboost flag latching               | Hardware sticking solenoid / map too hot | Drop Setpoint row, inspect mechanical wastegate, lower base duty    |
| Page 2 cells look empty               | Editor still on Page 1                    | Tap the **Page 1 / Page 2** toggle above the cell grid              |

## 12. Source Map

| File                                          | Role                                             |
| --------------------------------------------- | ------------------------------------------------ |
| [common/include/opendash_boost.h](../common/include/opendash_boost.h)   | Public API + wire structs (`MAP_POINTS=32`, `MODES=2`, `PARAMS_VERSION=2`) |
| [common/src/opendash_boost.c](../common/src/opendash_boost.c)           | Default 32-point maps, NVS, `opendash_boost_compute()`                       |
| [mos-4ch-a/main/main.c](../mos-4ch-a/main/main.c)                       | MOS-A slave runtime — dispatch + compute / telem tasks                       |
| [mos-4ch-b/main/main.c](../mos-4ch-b/main/main.c)                       | MOS-B slave runtime (mirrors MOS-A, `BOOST_PWM_CHANNEL=3`)                   |
| [center/main/boost_client.h](../center/main/boost_client.h)             | Center API                                                                   |
| [center/main/boost_client.c](../center/main/boost_client.c)             | 10 Hz live push + map authoring                                              |
| [center/main/boost_config_ui.c](../center/main/boost_config_ui.c)       | LVGL System Config screen — Page 1 / Page 2 editor                            |
| [center/main/system_config.c](../center/main/system_config.c)           | NVS-backed boost target selection                                             |
| [common/src/opendash_bt_ota.c](../common/src/opendash_bt_ota.c)         | Shared BLE GATT OTA service (used by MOS-A/MOS-B)                            |
| [common/include/opendash_i2c_protocol.h](../common/include/opendash_i2c_protocol.h) | Opcode catalogue (`BOOST_* 0x20..0x26`, `SYSTEM 0x07 + ENTER_BT_OTA 0x06`) |
| [boost-controller-opendash.md](../boost-controller-opendash.md)         | Design doc — Appendix B safe defaults, Appendix C MOS BLE OTA procedure       |

---

*Doc version 1.1 — updated for 32-point maps, NORMAL/RACE modes, paged UI, MOS-B OTA.*
