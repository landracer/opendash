<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OpenDash Dataflow Architecture

**Status:** as-built, post-batching v0.8.x
**Audience:** peer reviewers, contributors, and the next agent who has to debug this
**Last validated:** CENTER capture `dp/s = 81–135 err=0 qHW=0` after the LVGL flicker fix

This document describes how a single sensor reading travels from the engine harness to a pixel on the driver's display, end-to-end. It is intentionally tactical, not aspirational — every stage corresponds to real code in the tree.

---

## 1. Topology

```
                  ┌──────────────────────────┐
                  │      MultiDisplay        │   (designer2k2 board)
                  │  EGT × 8, RPM, lambda,   │
                  │  OBD-II PIDs, VDO speed  │
                  └────────────┬─────────────┘
                               │ UART 115200, ~5 frames/sec
                               ▼
                  ┌──────────────────────────┐
                  │       LEFT  (slave)      │  ESP32-S3-LCD-2.8C round
                  │  uart_parser_task        │  Node 0x10
                  │   → dp_id, value         │
                  │  forward_md_data_to_     │
                  │   center() — BATCHES     │
                  └────────────┬─────────────┘
                               │ ESP-NOW · CH1 MEDIUM
                               │ OPENDASH_CMD_DATA_BATCH (0x88)
                               │ 1 packet / UART frame, ~5 pps
                               ▼
                  ┌──────────────────────────┐
                  │     CENTER  (master)     │  ESP32-S3-Touch-LCD-4.3
                  │  espnow_dispatcher_task  │  Node 0x01
                  │   → channel queues       │
                  │  channel_medium_task     │
                  │   → ui_manager_update    │
                  │   → re-batch to RIGHT    │
                  └─────┬──────────────┬─────┘
                        │              │ ESP-NOW · CH1
                        │              │ OPENDASH_CMD_SET_DATA_BATCH (0x0C)
                        │              ▼
                        │   ┌──────────────────────┐
                        │   │   RIGHT  (slave)     │
                        │   │   ui_manager_update  │  Node 0x11
                        │   └──────────────────────┘
                        │
                        ▼
              ┌───────────────────┐
              │  LVGL render task │  (display_lvgl_lock + lv_timer_handler)
              │  lvgl_flush_cb    │  → ESP-LCD RGB → ST7262 800×480 panel
              └───────────────────┘
```

Other slaves (GPS 0x12, BMS 0x20, RELAY/MOS pods) feed CENTER on the same channel architecture but with their own producers.

---

## 2. Wire Protocol — `common/include/opendash_i2c_protocol.h`

A single ESP-NOW payload is one **opendash_i2c_msg_t** frame. The name is historical — we no longer use I²C — but the framing was kept so the same encoder/decoder serves both transports.

```
┌──────┬──────┬──────┬──────────────┬──────────┐
│ SYNC │ CMD  │ LEN  │  PAYLOAD     │ CHECKSUM │
│ 0xAA │ 1B   │ 1B   │  0–248 B     │   1B     │
└──────┴──────┴──────┴──────────────┴──────────┘
                       └─ length defined by LEN ─┘
```

### Opcodes that move telemetry

| Opcode | Name | Direction | Payload |
|--------|------|-----------|---------|
| `0x01` | `SET_DATA_POINT` | master → slave | `[dp_id:2][value:4]` |
| `0x0C` | `SET_DATA_BATCH` | master → slave | `[count:1][dp_id:2][value:4]×count` |
| `0x81` | `DATA_RESPONSE` | slave → master | `[dp_id:2][value:4]` |
| `0x88` | `DATA_BATCH` | slave → master | `[count:1][dp_id:2][value:4]×count` |

A **batch** packet replaces what used to be N individual packets for the same UART frame. Maximum entries per batch: `(248 − 1) / 6 = 41`. A typical MD frame produces ~25 entries, well within that budget.

### Why batching matters

Pre-batching, a single 25-PID MD UART frame caused 25 separate ESP-NOW transmissions inside a few milliseconds. That:

* hammered the WiFi MAC's CSMA backoff,
* drowned the CH1 worker queue (which was sized for events, not torrents),
* and produced a dispatcher that spent more time copying than dispatching.

After batching, the CH1 worker sees ~5 packets/second carrying ~125 dp/sec — the MAC is mostly idle, and queue high-water sits at 0.

---

## 3. Channel Architecture — `common/include/channel_config.h`, `common/src/channel_management.c`

Every ESP-NOW message rides on one of four logical channels. Each channel has its own queue and its own worker task on the receiving node, so a noisy low-priority producer can't block a critical one.

| Channel | Priority | Target latency | What rides it |
|---------|----------|----------------|---------------|
| **CH0 CRITICAL** | highest | 10 ms | engine kill, GPS lap-trigger, BMS fault |
| **CH1 MEDIUM**   | high    | 50 ms | gauge telemetry (RPM, EGT, lambda, OBD-II, BMS cells) |
| **CH2 LOW**      | normal  | 200 ms | relay/MOS feedback, diagnostics |
| **CH3 CONTROL**  | highest+1 | 5 ms | relay ON/OFF, OTA, reboot |

Sizes (hard-tuned after the batching rewrite):

```c
#define CHANNEL_QUEUE_DEPTH       32     // per channel
#define CHANNEL_QUEUE_ITEM_SIZE   192    // bytes — must hold the largest batch packet
```

`CHANNEL_QUEUE_ITEM_SIZE = 192` is **load-bearing**. Batch packets serialize to ~155 B; the previous 128-byte item buffer overflowed the dispatcher's stack-allocated copy and produced StoreProhibited boot loops.

### Routing rules

* `channel_mgr_send_to_node(node, ...)`: looks up the node's known MAC; if `node->online == false`, sets `max_retries = 0` so a worker task isn't blocked by exponential backoff to a dead node. Cold-start broadcast fallback is the slave's responsibility (see §4).

---

## 4. Producer side — LEFT pod (`left/main/main.c`)

### UART parsing

* `uart_parser_task` reads MultiDisplay frames at 115200 8N1.
* For each frame it dispatches each PID into `forward_md_data_to_center()` via a `BATCH_ADD(dp_id, value)` macro.
* When the frame is complete, the macro flushes the accumulated batch as a **single** `OPENDASH_CMD_DATA_BATCH`.

### Sending

`send_response_to_center(buf, len)`:

* If the center MAC is known → unicast on CH1.
* If `s_center_mac_known == false` → **broadcast** on CH1.

The broadcast fallback exists because LEFT may boot before CENTER. Without it, LEFT silently buffers data and the screen looks like the system is dead. A broadcast is "wasteful" (every node hears it), but the early-boot window is short and the cost is invisible at full operational rate.

### Receiving

LEFT also receives `OPENDASH_CMD_SET_DATA_BATCH` from CENTER (used when CENTER fans out fused data — e.g., GPS speed merged with MD RPM). The handler iterates and updates LEFT's local widgets the same way CENTER does.

---

## 5. Consumer side — CENTER (`center/main/espnow_master.c`)

### Stage A: `espnow_dispatcher_task` (core 0)

* Sits on the raw ESP-NOW receive callback queue.
* Decodes only enough to read the channel byte, then `xQueueSend()`s the raw bytes onto the matching channel queue.
* Hard bounds-check before memcpy: `copy_len = min(evt.len, CHANNEL_QUEUE_ITEM_SIZE)`. This is the safety net that catches misaligned senders — if it ever truncates, the deserializer below will reject the frame and log it.

### Stage B: per-channel worker tasks

Four tasks: `channel_critical_task`, `channel_medium_task`, `channel_low_task`, `channel_control_task`. Each one:

1. Dequeues from its own queue (no cross-channel head-of-line blocking).
2. Calls `opendash_i2c_deserialize()` to validate SYNC/LEN/CHECKSUM.
3. Switches on `cmd`:
   * `DATA_RESPONSE` → take LVGL lock, push one value to UI, release lock, forward as `SET_DATA_POINT` to RIGHT.
   * `DATA_BATCH` → take LVGL lock **once**, iterate all `count` entries pushing each to UI, release lock, **re-pack** the same payload as `SET_DATA_BATCH` and forward to RIGHT in one packet.

The single-lock-per-batch pattern is critical: `display_lvgl_lock(5)` competes with the LVGL render task, so the fewer acquisitions per second, the smoother the rendering.

### Stage C: telemetry

Outside the inner drain loop, every 1 second the worker logs:

```
CH1 flow: dp/s=N rx=N tx=N err=N qHW=N
```

This proves liveness even when no packets are arriving, which made debugging the batched rewrite tractable.

---

## 6. UI side — `center/main/ui_manager.c` + `display_init.c`

### `ui_manager_update_value(dp_id, value)`

Single entry point. Steps:

1. `dp_cache_store(dp_id, value)` — every value is cached so mode changes can repaint immediately without waiting for the next packet.
2. Look up `current_mode` in `mode_dp_maps[]` to find which widget (if any) this DP feeds in the active screen.
3. Apply unit conversion (`opendash_convert_temp/pressure/speed`) per `current_layout` settings.
4. Format and call `lv_label_set_text` and/or `lv_arc_set_value` on the matching widget.
5. Update per-mode min/max.

If the active mode doesn't display this DP, the function still caches the value but does no LVGL work.

### Mode set

| Mode | Layout |
|------|--------|
| `ENGINE`, `GPS`, `MULTIDISPLAY` | Center arc + 6 text sections + min/max |
| `RELAY`, `BMS`, `OBD`, `CONFIG` | Grid of boxes; arc hidden |

Grids are **created on entry, destroyed on leave** to keep the LVGL object count low and avoid heap fragmentation in PSRAM.

### Render path — the part that was flickering

```c
lv_display_set_buffers(disp, fb0, fb1, fb_size, LV_DISPLAY_RENDER_MODE_DIRECT);
```

LVGL 9 DIRECT mode draws only **dirty rectangles** into the back buffer. With two framebuffers in PSRAM, after the buffer swap the new back buffer holds frame N−2 in any region not redrawn this cycle. The next frame's invalidations briefly expose those stale pixels → visible flicker that scales with how often values change.

The fix in `lvgl_flush_cb`:

```c
esp_lcd_panel_draw_bitmap(panel, x1, y1, x2+1, y2+1, px_map);
xSemaphoreTake(s_vsync_sem, ...);
/* mirror the dirty rect into the OTHER framebuffer */
void *other_fb = (px_map == fb0) ? fb1 : fb0;
for (int y = y1; y <= y2; y++) memcpy(dst+y*pitch, src+y*pitch, line_bytes);
lv_display_flush_ready(disp);
```

This is the canonical ESP-IDF pattern for LVGL 9 DIRECT + 2 FBs. Cost is one bounded memcpy per dirty rect per frame; benefit is zero flicker regardless of update rate.

---

## 7. Timing, end-to-end (typical)

| Stage | Latency |
|-------|---------|
| Sensor → MD ADC + frame assembly | ~200 ms (5 fps UART frame rate) |
| MD UART → LEFT parser | < 5 ms |
| LEFT batch flush → ESP-NOW TX | < 5 ms |
| Air-time + CENTER RX cb → dispatcher queue | < 2 ms |
| CH1 worker dequeue → `ui_manager_update_value` | < 1 ms |
| Next LVGL render cycle | < 33 ms |
| LCD vsync flip | 25 ms (40 Hz panel) |
| **Total worst case** | **~270 ms** from sensor edge to lit pixel |

The MD UART frame rate dominates. Anything faster requires moving sensors directly onto an OpenDash node (e.g., direct CAN ingestion on the LEFT pod, planned).

---

## 8. Failure modes & their telemetry

| Symptom | Where to look |
|---------|---------------|
| `dp/s = 0` | LEFT not transmitting, or CENTER MAC unknown and broadcast fallback failed |
| `err > 0` and rising | `channel_mgr_send_to_node` is failing — usually RIGHT offline; should be benign with `max_retries=0` |
| `qHW` climbs | CH1 worker can't keep up; suspect LVGL lock contention or expensive UI handlers |
| StoreProhibited at boot | A new opcode produces packets > `CHANNEL_QUEUE_ITEM_SIZE`; bump it |
| Flicker on values | `lvgl_flush_cb` regression — verify the FB mirror loop is still present |
| Display tearing | `s_vsync_sem` not being given by `on_vsync_event` — check the IRQ callback registration |

---

## 9. What is **not** in this design (intentionally)

* **No polling, no pinging.** Slaves push when they have data; absence of data within a per-node timeout is what flips them offline.
* **No ACKs above the ESP-NOW MAC layer.** Lost packets are tolerated — the next batch carries fresh values anyway.
* **No global throttling on the CENTER UI.** The user explicitly rejected throttling; UI updates run at the full producer rate. Smoothness is achieved by batching at the producer, not by dropping at the consumer.
* **No screen-to-screen synchronization protocol.** RIGHT receives the same `SET_DATA_BATCH` CENTER processed; if RIGHT is offline that batch is dropped at the MAC and tried again on the next batch.

---

## 10. Open questions for review

1. **GPS / BMS not yet batched.** They still send per-DP `DATA_RESPONSE`. Their rates are low enough that this is fine today, but for symmetry the same batch handler should be added on those producers.
2. **No back-pressure signal.** If a worker ever falls behind, the queue silently drops oldest. We log `qHW` so this is observable, but there's no producer-side rate limit. Fine while `qHW=0`, worth revisiting if it ever climbs.
3. **One UI lock for the whole batch.** Holding the LVGL lock through 25 widget updates means renders can stall by ~5 ms. We could split a batch in half if this ever shows up as visible jank, but right now the render loop is comfortably ahead.
4. **Device Mgmt screen does not yet drive per-node PID assignment.** It is a status view only. The data path is ready (`SET_SCREEN_LAYOUT = 0x02` is reserved); the UI to author and persist a layout is the missing piece.
