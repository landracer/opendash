<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# Center Display Tearing — Debug Session

> **Scope:** ESP32-S3-Touch-LCD-4.3 (800×480 RGB IPS, ST7262) running as `center`.
> **Symptom (user-reported, 2026-05):** intermittent horizontal tearing, often bleeding bottom-half pixels over the top half. Comes and goes; can persist for many seconds at a time. **Other devices (`left`, `right`, `gps`, `pod1/2`) show no tearing — only `center`.**
>
> This document is the canonical intake / log for the tearing investigation. Every attempt — successful, failed, or inconclusive — goes in the **Attempts Log** at the bottom. Don't delete entries; mark them resolved or superseded.

---

## 1. Intake

| Field | Value |
| --- | --- |
| Device | `center/` (Waveshare ESP32-S3-Touch-LCD-4.3) |
| Panel | ST7262, 800×480 RGB565, 24-bit RGB interface wired as 16-bit |
| MCU | ESP32-S3, 240 MHz, dual-core, Octal PSRAM @ 80 MHz |
| ESP-IDF | v6.1-dev-2441-gffb63db38b-dirty |
| LVGL | 9.2.2 |
| Render mode | `LV_DISPLAY_RENDER_MODE_DIRECT`, `num_fbs = 2` (HW framebuffers in PSRAM) |
| Pixel clock | 16 MHz, `pclk_active_neg = true` |
| Bounce buffer | `20 * LCD_H_RES` = 16000 px = 32 KB |
| Vsync gate | `s_vsync_sem` given from ISR, taken in `lvgl_flush_cb` after `draw_bitmap` |
| Existing mitigations | Fix A (drain stale vsync token before draw), Fix B (mirror dirty rect into other FB), reduced `LVGL_BUFFER_HEIGHT=20`, touch task on core 1 |
| Port for monitor | `/dev/ttyACM4` @ 115200 |

### Observed pattern

- Tearing is **bottom-over-top** — i.e., the tear line walks upward, with the upper region showing what should be the lower half of the *next* frame.
- Intermittent. Reproduces especially during full-screen redraws (page switches, animated arcs, MIL/warning flashes).
- Not correlated with backlight PWM.
- Other RGB-panel devices (`left`, `right` — both 480×480) use the same vsync-gate pattern and don't tear.

### Hypothesis matrix (live)

| # | Hypothesis | Confidence | Notes |
| --- | --- | --- | --- |
| H1 | `bounce_buffer_size_px != 0` + `num_fbs == 2` → vsync semaphore fires on bounce-refill boundary, not real vsync | **Disproven** | Tried `bounce_buffer_size_px = 0`; got horizontal chop/wave (PSRAM bandwidth starvation) within seconds. Reverted. |
| H2 | PSRAM bandwidth contention when LVGL render + bounce-DMA + UI-task allocations collide | High | Center is the only device with PSRAM-routed LVGL heap (`lv_mem_psram.c`) AND active background allocations (BMS, OBD2, ESP-NOW). 480×480 devices have ~½ the pixel rate. |
| H3 | `LCD_RGB_REFRESH_TASK_PERIOD_MS=16` lets refresh task drift relative to LVGL render cycle, occasionally allowing draw_bitmap to take effect mid-frame | Medium | Re-check: is the refresh task even the swap arbiter when `num_fbs=2`? It shouldn't be — swap is HW. But the *refill window* for bounce-buffer mode IS scheduled by this task. |
| H4 | Fix B (post-vsync mirror memcpy) runs while panel is already scanning the just-swapped FB; if memcpy crosses the active scan line we corrupt the displayed buffer | Medium-High | Mirror writes into `other` (the buffer NOT being scanned) so this *should* be safe. But verify: are FB pointers stable, or does ESP-IDF rotate them? |
| H5 | LVGL flush is called from `lv_timer_handler` on a task whose stack/priority drops it briefly below the vsync window | Medium | `lvgl_task` priority should be ≥ refresh task. Need to log priorities. |
| H6 | `pclk_active_neg = true` combined with ST7262 typical timings is marginal at our pclk; tear is a sampling artifact, not a buffer swap artifact | Low | Other ESP32-S3 RGB designs use neg pclk fine. Tear pattern doesn't match (sampling-edge tear would be vertical column noise, not horizontal bottom-bleed). |
| H7 | `CONFIG_LCD_RGB_RESTART_IN_VSYNC=y` is interacting badly with `num_fbs=2` (the option is designed for single-FB rebound) | Medium | Worth toggling. Documented as "use when you see tearing in single-buffer" — possibly counterproductive here. |
| H8 | UI thread does a slow operation (BLE log, SD card write, ESP-NOW queue flush) on the same core as the LCD DMA / cache, starving the refresh task | High | `center` runs more concurrent subsystems than any other node. |

---

## 2. Problem statement

Center displays intermittent bottom-over-top frame tearing despite:

- `num_fbs = 2` (true double-buffer)
- `LV_DISPLAY_RENDER_MODE_DIRECT` (LVGL renders into the back FB)
- Vsync-gated flush (`xSemaphoreTake(s_vsync_sem, 100ms)` after `draw_bitmap`)
- Drain-stale-token fix (Fix A)
- Cross-FB mirror of dirty rect (Fix B)

This indicates either **(a)** the vsync gate is not aligned with the actual buffer-switch event the panel uses, **(b)** the back-FB content is being touched *after* the swap has been latched, or **(c)** the FB swap itself is being preempted by another DMA client (PSRAM bus contention).

**Acceptance criterion for "fixed":** 10+ minutes of continuous use including page switches, MIL flashes, BMS/OBD2 packet bursts, and BLE-OTA-banner activation, with zero observed tear events. Verified via long-run serial capture + visual inspection.

---

## 3. Plan of action (next attempts, in order)

Run **one change at a time**, flash, run the long-capture, mark the result in the log. Reverting is cheaper than chasing two variables.

1. **Capture baseline** (current code, unchanged) — run `scripts/tearing_capture.sh` for 10 min while exercising the UI, save log as `tearing-logs/baseline-<date>.log`. Confirms tearing reproduces with current code so we can A/B against changes.
2. **Disable `CONFIG_LCD_RGB_RESTART_IN_VSYNC`** (H7) — single sdkconfig flip, no source change.
3. **Drop pclk to 14 MHz** (H6/H2 combined) — reduces PSRAM bandwidth requirement by 12.5 %.
4. **Halve LVGL render priority churn**: pin `lvgl_task` to core 0 explicitly with priority `configMAX_PRIORITIES - 2`, move touch + button tasks fully to core 1 — currently mixed. (H5/H8)
5. **Replace Fix B memcpy with full-frame copy on swap** — instead of per-rect mirror inside `flush_cb`, copy the full FB once we know the swap completed. Eliminates the "did the mirror race the scanline?" question (H4). Cost ~1.5 ms at 16 MHz pclk.
6. **Try `bounce_buffer_size_px = 10 * LCD_H_RES`** (half) — does the PSRAM-contention noise come back gradually, and is there a sweet spot between 0 and 20 lines? (H1 refinement)
7. **Last resort:** switch to `num_fbs = 3` (triple-buffer). Costs +375 KB PSRAM but removes the back-FB-being-touched-during-swap class entirely.

---

## 4. Long-run serial capture

Use [scripts/tearing_capture.sh](scripts/tearing_capture.sh) — see file for usage. While it's running, **when you see a tear** type a description into a second terminal:

```bash
echo "$(date -Iseconds) TEAR — full-screen page switch BMS→OBD2, bottom 1/3 bled over top" \
    >> tearing-logs/operator-notes.log
```

This gives us a side-by-side correlation between log lines (RSSI changes, ESP-NOW packets, allocations) and visual tear events. Without timestamps on tear events the serial log is just noise.

The script:
- Sets `stty raw 115200` (won't fight with `idf.py monitor`)
- Prefixes every line with a millisecond timestamp
- Rotates logs every 50 MB
- Drops a separator line every 60 s so you can scroll to "tear at 12:34:56" quickly

---

## 5. Attempts log

> Append-only. Newest at top. When an attempt is superseded, link forward instead of deleting.

### 2026-05-31 — TODO #14 trial: `bounce_buffer_size_px = 0`

- **Hypothesis:** H1 (bounce-refill boundary triggering false vsync gate)
- **Change:** `center/main/display_init.c` — `panel_config.bounce_buffer_size_px = 0`. Build + flash + boot OK.
- **Result:** Horizontal chop / "waveform" artifacts within seconds of UI activity. Display unusable. Tearing not assessable because chop dominated.
- **Conclusion:** PSRAM bandwidth contention is real — bounce buffer is load-bearing on this hardware at 16 MHz pclk. **Reverted.** Header comment in `display_init.c` updated.
- **Next:** H7 (toggle `LCD_RGB_RESTART_IN_VSYNC`) — see plan step 2.

### 2026-05-24 — Fix A + Fix B applied

- **Hypothesis:** Two-bug model: (A) stale vsync token from prior frame allows flush to return before real next vsync; (B) DIRECT mode + 2 FBs leaks frame N-2 pixels into areas not invalidated.
- **Change:** `lvgl_flush_cb` now does `xSemaphoreTake(0)` drain before `draw_bitmap`, then `xSemaphoreTake(100 ms)` after. Plus per-rect memcpy of dirty area into the OTHER FB after vsync.
- **Result:** Reduced tearing frequency materially but did **not** eliminate it. Bottom-over-top tear still appears intermittently, especially during heavy redraws.
- **Conclusion:** Necessary but not sufficient. Tearing is multi-causal.

### 2025-?? — Initial DIRECT-mode + num_fbs=2 + vsync gate (pattern copied from left/right)

- **Hypothesis:** Single-FB partial-mode with PSRAM was the root cause of all visual issues.
- **Change:** Adopted ESP-IDF's documented tear-free pattern.
- **Result:** Eliminated stripe artifacts and "humming". Tearing reduced but never eliminated.

---

## 6. Reference: the working pattern on `left` / `right`

For comparison, the 480×480 devices use the same vsync-gate pattern with these differences:

| Setting | `center` | `left` / `right` |
| --- | --- | --- |
| Resolution | 800×480 = 384 000 px/frame | 480×480 = 230 400 px/frame |
| Pixel clock | 16 MHz | 18 MHz |
| Bounce buffer | 20 lines × 800 = 16 000 px | 20 lines × 480 = 9 600 px |
| `pclk_active_neg` | true | false |
| `CONFIG_LCD_RGB_RESTART_IN_VSYNC` | y | y (verify) |
| Concurrent subsystems | OBD2 + BMS + ESP-NOW master + BLE + SD + UI | UI only (slave receives data, no high-rate publishers) |
| LVGL heap routing | PSRAM (`lv_mem_psram.c`) | Internal SRAM (default) |
| Pixel rate at refresh | ~23 MB/s (RGB565) | ~13.8 MB/s |

The two big asymmetries vs the tear-free devices are **(1) PSRAM-routed LVGL heap** and **(2) ~1.7× the pixel rate plus 4× the background subsystem traffic**. Both feed H2 / H8.

---

## 7. Open questions

- Does the tear pattern change if we disable BMS RX? (Isolates H8)
- Does the tear pattern change if BLE is disabled? (Isolates H8)
- What's `lvgl_task` priority vs the ESP-IDF refresh task priority? (Need to log)
- Can we sample `esp_lcd_rgb_panel_get_frame_buffer` pointers at boot and confirm they're stable across calls? (Confirms Fix B target math)
