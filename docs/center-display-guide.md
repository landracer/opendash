<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# Center Display — Implementation Guide & Lessons Learned

> **Hardware:** Waveshare ESP32-S3-Touch-LCD-4.3 (800×480 IPS RGB565)
> **Framework:** ESP-IDF v6.1 + LVGL 9.2.2 + ESP-NOW
> **Last Updated:** March 2026

---

## Architecture Overview

The center display uses a **single LVGL screen** with 7 display modes
split into a normal cycle and a debug cycle:

| Mode | Index | Type | Description |
|------|-------|------|-------------|
| ENGINE | 0 | Arc + 6 sections | RPM arc, coolant, boost, oil, EGT, etc. |
| GPS | 1 | Arc + 6 sections | Speed arc, heading, alt/sats/HDOP/fix |
| MULTIDISPLAY | 2 | Arc + 6 sections | MD UART data (EGT1-4, O2/Lambda, MAF, MD RPM) |
| RELAY | 3 | Grid (7×4) | Touch-controlled relay/MOS toggles (DEBUG mode) |
| BMS | 4 | Grid (5×3) | rAtTrax BMS telemetry (SOC, voltage, VESC) (DEBUG mode) |
| OBD | 5 | Grid (5×3) | OBD-II data from MultiDisplay (DEBUG mode) |
| CONFIG | 6 | Grid/actions | Node status, OTA, self-test, reboot, debug entry |

**Mode navigation:**
- Normal cycle: `ENGINE -> GPS -> MULTIDISPLAY -> CONFIG`
- Debug cycle: `RELAY -> BMS -> OBD`
- Enter debug cycle: tap `DEBUG MODE` on CONFIG
- Exit debug cycle: long-press bottom status bar

---

## Critical Rules — DO's and DON'Ts

### LVGL Memory (CRITICAL)

| Rule | Why |
|------|-----|
| **DO** use `CONFIG_LV_MEM_SIZE_KILOBYTES=64` + `CONFIG_LV_MEM_POOL_EXPAND_SIZE_KILOBYTES=64` | 128KB total is sufficient. 256KB base overflows DRAM BSS by ~23KB. |
| **DON'T** set `CONFIG_LV_MEM_SIZE_KILOBYTES=256` | Causes linker error: `.bss will not fit in region `dram0_0_seg'` |
| **DON'T** set `CONFIG_LV_MEM_CUSTOM=y` | This is unknown/ignored in LVGL 9.2.2. Silently falls back to 64KB default pool. |
| **DO** keep only ONE grid in memory at a time | Each 5×3 grid = ~61 LVGL objects (1 container + 15 boxes × 4 widgets). Three grids = ~183 objects, which exhausts the 128KB pool. |
| **DON'T** create all grids at init | Was the original cause of splash screen stuck — 120+ objects overwhelmed the first DIRECT-mode render. |

### Destroy-on-Leave Grid Architecture (CRITICAL)

The center uses a **destroy-on-leave, create-on-enter** pattern for grid screens:

```
Entering RELAY mode:
  1. Destroy BMS grid container (if exists) → NULL + zero widget array
  2. Destroy OBD grid container (if exists) → NULL + zero widget array
  3. Create RELAY grid fresh

Entering ENGINE mode (non-grid):
  1. Destroy ALL grid containers
  2. Show arc + 6 data sections
```

**Why this works:**
- Only ~61 grid objects exist at any time (not ~183)
- Grid creation takes ~50-80ms (acceptable for screen transitions)
- `lv_obj_del()` recursively frees the container and all children

**Min/max values are SAFE:**
- Arc min/max: stored in `s_arc_min_val[]` / `s_arc_max_val[]` static arrays (indexed by mode)
- Section max: stored in `s_section_max[][]` static arrays
- Relay on/off state: stored in `relay_box_map[].is_on` static struct
- **None of these are LVGL objects** — they survive grid destruction

**Relay visual restoration:**
- `create_relay_grid()` reads `relay_box_map[idx].is_on` to set initial ON/OFF text and background color
- Toggling a relay updates both the LVGL widget AND the static struct
- Re-creating the grid after leaving and returning restores the correct visual state

### Display Rendering

| Rule | Why |
|------|-----|
| **DO** use DIRECT render mode with double framebuffer | Only viable mode for 800×480 RGB panel — partial buffer is too slow. |
| **DO** keep bounce buffer at 20× LCD_H_RES | Prevents PSRAM DMA contention artifacts. |
| **DO** wait for vsync in flush callback | `xSemaphoreTake(s_vsync_sem)` prevents writing to the displayed buffer. |
| **DON'T** call `vTaskDelay()` inside LVGL object creation | When called from an event handler, the LVGL mutex is already held. `vTaskDelay()` yields the task but the mutex stays locked, causing deadlocks or corruption. |
| **DON'T** create >60 LVGL objects in a single function without WDT feeding | The ui_task feeds WDT via `esp_task_wdt_add(NULL)` + `esp_task_wdt_reset()`. Heavy creation can trigger WDT if done outside the task. |

### Touch / Swipe Navigation

| Rule | Why |
|------|-----|
| **DO** use `LV_EVENT_GESTURE` for swipe detection | LVGL 9's built-in gesture system reliably distinguishes taps from swipes. |
| **DON'T** use manual PRESSED/RELEASED tracking for swipe | GT911 reports stale (0,0) coordinates on RELEASED. PRESSING events don't bubble to parent when child objects are focused. |
| **DON'T** read `lv_indev_get_point()` on LV_EVENT_RELEASED | Returns last known position, not lift position — causes phantom swipes. |
| **DO** use `lv_indev_get_gesture_dir()` | Returns `LV_DIR_LEFT`, `LV_DIR_RIGHT`, etc. Clean and reliable. |

### sdkconfig.defaults

The center's `sdkconfig.defaults` controls critical memory and peripheral settings:

```ini
# LVGL Memory — VALIDATED VALUES, DO NOT CHANGE
CONFIG_LV_MEM_SIZE_KILOBYTES=64
CONFIG_LV_MEM_POOL_EXPAND_SIZE_KILOBYTES=64

# PSRAM — Required for framebuffers
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
```

**Never set LV_MEM_SIZE to 256 or higher** — it will overflow DRAM.

### ESP-IDF Environment

| Rule | Why |
|------|-----|
| **DO** run `source /home/sysadmin/Documents/esp-ide/esp-idf/export.sh` before builds | Required for idf.py and all toolchain access. |
| **DO** flash center to `/dev/ttyACM1` | Port mapping: ACM0=left, ACM1=center, ACM2=gps, USB0=rAtTrax BMS |

---

## File Reference

| File | Purpose |
|------|---------|
| `center/main/ui_manager.c` | All LVGL UI: arc, sections, grids, mode switching, touch, warnings |
| `center/main/ui_manager.h` | Public API: init, start, update_value, next_screen, set_display_mode |
| `center/main/display_init.c` | LCD panel init, GT911 touch, LVGL display/input setup, vsync |
| `center/main/main.c` | App entry, splash screen, task creation |
| `center/main/espnow_master.c` | ESP-NOW master: node discovery, data forwarding, relay commands |
| `center/sdkconfig.defaults` | Build-time defaults (LVGL memory, PSRAM, WiFi) |

---

## Known Working Configuration (March 2026)

- ESP-IDF v6.1-dev
- LVGL 9.2.2 (managed component)
- 64KB LVGL pool + 64KB runtime expansion
- DIRECT render mode, double framebuffer in PSRAM
- 20-line bounce buffer (LCD_BOUNCE_BUFFER_SIZE = 20 * 800)
- 16 MHz pixel clock (conservative, per Waveshare reference)
- GT911 at I2C 0x5D, 400kHz, hardware reset via CH422G
- WDT feeding in ui_task via `esp_task_wdt_add/reset`
- Gesture-based screen navigation (LV_EVENT_GESTURE)
- Destroy-on-leave grid architecture (max 1 grid alive at a time)

---

## Debugging Tips

1. **Splash screen stuck / flickering lines:** Too many LVGL objects created at init. Defer grid creation.
2. **OBD/BMS grid creation hangs:** Memory pool exhausted. Ensure previous grids are destroyed first.
3. **Every tap changes screen:** Swipe handler bug — use LV_EVENT_GESTURE, not manual PRESSED/RELEASED.
4. **DRAM overflow at link time:** LV_MEM_SIZE too large. Use 64KB base + 64KB expand.
5. **WDT panic during render:** UI task isn't feeding watchdog. Add `esp_task_wdt_add(NULL)` + `esp_task_wdt_reset()`.
6. **Screen tearing (bottom rolls over top):** PSRAM bandwidth contention during heavy operations. Increase bounce buffer or reduce concurrent DMA activity.
