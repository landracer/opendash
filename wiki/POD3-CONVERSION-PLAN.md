<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# POD3 Conversion Plan — Repurpose the GPS Node as a Standard Gauge Pod

> **Goal:** The LC76G GPS unit (Waveshare ESP32-S3-Touch-AMOLED-1.75) is unusable
> as a telemetry sensor (see `GPS-LC76G-POSTMORTEM.md`). Its firmware is
> effectively identical to pod1/pod2 except for the dead GNSS handler and the
> SD logger. This plan turns that exact board into **Pod 3** — a display-only
> ESP-NOW gauge pod, identical in role to `pod1/` and `pod2/`.
> **Status: PLANNED — not yet implemented.**

## 0. Why This Is Cheap

`gps/main` and `pod1/main` are the same firmware skeleton (same board, same
CO5300 QSPI AMOLED, same CST9217 touch, same QMI8658 IMU, same ui_manager).
Delta `gps/main` → pod template:

| gps/main has | pod1/main has | action for pod3 |
|---|---|---|
| `gps_handler.c/h` (LC76G I2C driver) | — | **drop** |
| `sd_logger.c/h` (+`fatfs`,`sdmmc`,`esp_driver_uart` deps) | — | **drop** |
| IMU-based rollover detector | same | keep |
| `OPENDASH_NODE_GPS` identity everywhere | `OPENDASH_NODE_POD1` | swap to `OPENDASH_NODE_POD3` |

Wire-protocol enums stay frozen: `OPENDASH_NODE_GPS = 3` remains in
`common/include/opendash_common.h` (do NOT renumber — `boostcontrol-staging`
rule: never break existing ESP-NOW opcodes/node IDs). `OPENDASH_NODE_POD3 = 7`
already exists in the enum, in `NODE_DEFAULT_CHANNEL` (CHANNEL_MEDIUM) and in
`NODE_EXPECTED_FREQ_HZ` (heartbeat mode, =1). The mesh layer needs no changes.

## 1. Scaffold `pod3/` (currently empty)

Copy from `pod1/` (the cleanest pod), excluding build artifacts:

```
pod3/CMakeLists.txt        (project(opendash_pod1) → project(opendash_pod3))
pod3/main/CMakeLists.txt   (drop fatfs/sdmmc/esp_driver_uart from REQUIRES,
                            drop gps_handler.c/sd_logger.c — pod1's list as-is)
pod3/main/main.c           (all OPENDASH_NODE_POD1 → OPENDASH_NODE_POD3,
                            TAG "opendash_pod1" → "opendash_pod3")
pod3/main/display_init.c/h (copy unchanged — same CO5300/CST9217 board)
pod3/main/ui_manager.c/h   (copy unchanged — same 6 screens)
pod3/main/imu_handler.c/h  (copy unchanged)
pod3/main/idf_component.yml, dependencies.lock, partitions.csv,
pod3/sdkconfig, pod3/sdkconfig   (copy unchanged)
```

Note: pod1 has **no** `splash_podN.h` — it compiles via the `__has_include`
fallback (`HAS_SPLASH 0`). pod3 inherits that behavior; no image work needed.
`OPENDASH_NODE_IS_POD()` already covers POD1–POD8, so pod3 is treated as a
gauge pod with no macro changes.

## 2. Center-Side Wiring (what makes POD3 a real mesh member)

1. **`center/main/main.c`**
   - `parse_node_name()` (~line 82): add `strcmp(name,"pod3")==0 →
     OPENDASH_NODE_POD3` (needed for serial-config commands).
   - `node_supports_ota()` (~line 56): add `case OPENDASH_NODE_POD3:`.
2. **`center/main/ui_manager.c`** config node table (~line 1016):
   - Keep `CONFIG_NODE_COUNT 11` by **replacing the `"GPS"` chip with
     `"POD3"`** in `config_nodes[]` and in the blink `node_map[]` (both keep
     index 2 — row1 comment becomes `LEFT RIGHT BMS POD1 POD2 POD3`; adjust
     `CONFIG_NODES_ROW1 6→5` and `CONFIG_NODES_ROW2 5→6`, keeping 11 total).
   - `opendash_config_load()/reset_defaults()` in pod3/main already key off the
     node enum; `opendash_display_config.c` line ~218 default-layout case
     already groups POD1/POD2/POD3 → shared pod default layout, no change.
   - Line ~1917 + ~2009 comments/strings: detector set "RIGHT/POD1/POD2" →
     "RIGHT/POD1/POD2/POD3".
3. **`center/main/espnow_master.c`**
   - Handshake fan-out (~line 978): add
     `channel_mgr_send_to_node(OPENDASH_NODE_POD3, tx, tl);` (both in the 3×
     boot handshake loop and the steady-state SET_DATA_POINT broadcast).
   - `espnow_master_get_status()` / `s_node_status`: add `pod3_online`
     (`node_health_is_alive(OPENDASH_NODE_POD3)`) and surface it where
     pod1/pod2 online flags are consumed.
4. **Safety-critical decision — parachute voting quorum.** The pods' IMUs are
   rollover *detectors*: `common/include/opendash_rollover.h` defines
   `OPENDASH_ROLLOVER_DETECTORS` = RIGHT, POD1, POD2 with
   `OPENDASH_ROLLOVER_DETECTOR_COUNT 3` (unanimous vote required to fire).
   **Default decision: add `OPENDASH_NODE_POD3` to `OPENDASH_ROLLOVER_DETECTORS`
   and raise `..._DETECTOR_COUNT` to 4**, mirroring how POD1/POD2 were added.
   Same board, same QMI8658, same duty. Center vote aggregation in
   `espnow_master.c` (~line 123 comment + vote handling) must accept POD3
   VOTE (0x96) messages. ⚠️ User veto point — say so if pod3 should instead
   be display-only (non-voting); that is the smaller change.

`node_health.c` already defaults POD3 to heartbeat mode (=1 pps expected);
verify no code path still *requires* GPS-alive after this change (GPS chip
leaves the config UI, so its permanent "offline" badge is gone — good).

## 3. Retire `gps/`

- After pod3 boots and verifies, `git mv gps/ archive/gps-lc76g-frozen/` (the
  whole frozen node, summaries and vendor demo included) so the live tree has
  no ghost node. `gps-bk/` and the `v15*_summary.txt` files go with it.

## 4. Documentation Sync

- `readme.md` node table: replace the "GPS / Telemetry" row with **Pod 3**
  (same board link, 466×466 AMOLED, directory `pod3/`); fix the repo-tree
  listing (`gps/` line → `pod3/`), and doc-link list.
- `docs/architecture.md`: node table row GPS — mark **retired** (already
  FROZEN) and note the board now runs `pod3/` firmware as POD3.
- `PROJECT_INDEX.md`, `FEATURES_AND_SENSORS.md`, `QUICKSTART.md`: sweep for
  GPS-node references; GPS-data-point screens (LAT/LON/SATS/HEAD/TIME) become
  legacy screen types — leave enums, note retired.
- `wiki/gps-unit.md`: banner → superseded by `GPS-LC76G-POSTMORTEM.md` and
  this conversion.

## 5. Build / Flash / Verify

1. `cd pod3 && idf.py build` — must pass before touching hardware.
2. `idf.py -p /dev/ttyACM0 flash` on the old GPS board (verify port: it is the
   board previously flashed as the gps node).
3. Serial: boot log shows `Node: OPENDASH_NODE_POD3 (ESP-NOW)`; center
   discovers it ("Center discovered @ …" on pod3; POD3 chip goes online on the
   center CONFIG screen).
4. Functional: swipe/boot-button cycles OIL_TEMP → WATER → AFR → BOOST →
   GFORCE → DEBUG with live data from center; center boost-config page
   changes reach pod3; **rollover test**: rotate pod3 physically, verify the
   4-detector unanimous vote logic (deploy sim/DEBUG screen shows 4/4).
5. Regression: pod1/pod2 unchanged behavior; BMS still votes; GPS chip gone
   from config grid.

## 6. Out Of Scope / Frozen

- No LC76G code migration, no SD logger, no GPS screen revival. The module is
  a logger with a locked 1 Hz mouth — done forever.
- No `common/` enum renumbering, no wire-format changes.
- POD4–POD8 remain "not deployed" placeholders.

## 7. Open Questions (need user sign-off)

1. **Detector quorum:** POD3 joins the unanimous rollover voting set
   (3→4 detectors)? Recommended: yes, identical hardware to POD1/POD2.
2. Screens: keep pod1's default screen set (OIL/WATER/AFR/BOOST/GFORCE/DEBUG)
   as pod3's defaults? (center can reconfigure live either way.)
3. Confirm the physical slot/label "Pod 3" is simply where this unit goes —
   no hardware changes (the LC76G stays onboard, simply never communicated
   with; GPIOs it used go idle).