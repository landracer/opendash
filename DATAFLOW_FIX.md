<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# Data Flow — Pool / Pipe / Drain (2026-05)

> The system pushes more ESP-NOW frames into the radio than the radio can drain. Symptom: `channel_mgr` quarantines slave peers every ~2.4 s with "5 consecutive TX-queue rejects". Visual: brief flashes / dropouts of values in display boxes.
>
> This doc captures the diagnosis, the **surgical fix** (one source-side change, 15× drop in frame rate), and the **roadmap** for the proper per-peer flow-control plumbing we'll need once the bus has more talkers.

---

## 1. Diagnosis (the pool, the pipe, the drain)

**The pool** = all the data points that want to be on a slave's screen.
**The pipe** = ESP-NOW radio (one HW MAC, ~32 dynamic TX buffers shared, ACK-per-frame).
**The drain** = `esp_now_send` → MAC TX → ACK → done. Frame rate ceiling for small ACKed frames on a clean RF: ~300–500/s; under load (other peers, retries, BLE coexist): well below.

Measured load (`channel_mgr` log on `center` ACM4, idle 8 s sample):

| Leg | Pkts/s | Notes |
| --- | --- | --- |
| LEFT → CENTER (MD telemetry) | **~75** | 15 DPs × 5 Hz MD frame, **one packet each** |
| CENTER → RIGHT (forwarded) | **~75** | 1:1 forward of each LEFT DP |
| Background (status, GPS, BMS, OBD2) | ~10–30 | Variable |
| **Total bus frames/s** | **~150–180** | Headroom near zero |

`err=29310` vs `tx=7781` ≈ **79 % of `esp_now_send` calls return `ESP_ERR_NO_MEM`** (HW TX queue full). When 5 in a row fail per peer, `channel_mgr` quarantines that peer for exponential backoff. **That's the visual flash.**

### Root cause (one line)

`left/main/main.c::forward_md_data_to_center()` calls `send_data_point_to_center()` **15 times per MD UART frame**. Each call is a separate `esp_now_send`. Should be **one `SET_DATA_BATCH` frame** with all 15 DPs (the protocol and the center-side parser already support it).

### Why we got here

The 1-DP-per-packet pattern is **fine for low-rate sensors** (e.g. boost from a pod every 100 ms). It only becomes pathological when a single source emits many DPs per cycle. MD UART is the worst offender — 15 DPs per 200 ms tick.

---

## 2. Surgical fix (THIS PR)

**One change**, source side: replace the 15-call loop with one batch build.

```c
/* left/main/main.c — forward_md_data_to_center() rewritten */

static void forward_md_data_to_center(const opendash_md_data_t *md)
{
    if (!s_center_mac_known) return;

    /* Build batch payload: [count:1][dp_id:2][value:4]×N */
    uint8_t payload[1 + 15 * 6];  /* 91 bytes; well under 250 */
    uint16_t off = 0;
    payload[off++] = 15;
    #define PUT(dp, v) do { \
        payload[off++] = ((dp) >> 8) & 0xFF; payload[off++] = (dp) & 0xFF; \
        float _v = (v); memcpy(&payload[off], &_v, 4); off += 4; \
    } while (0)
    PUT(OPENDASH_DP_EGT1,            md->egt[0]);
    PUT(OPENDASH_DP_EGT2,            md->egt[1]);
    /* ... 13 more ... */
    #undef PUT

    opendash_i2c_msg_t msg;
    opendash_i2c_build_msg(&msg, OPENDASH_CMD_SET_DATA_BATCH, payload, off);
    uint8_t buf[OPENDASH_ESPNOW_MAX_DATA];
    uint16_t len = 0;
    if (opendash_i2c_serialize(&msg, buf, &len) == OPENDASH_OK) {
        opendash_espnow_send(s_center_mac, buf, len);
    }
}
```

**Expected impact:**

- LEFT → CENTER MD load: **75 → 5 pkts/s** (15×)
- CENTER → RIGHT forwarded MD load: **75 → 5 pkts/s** (center already re-batches BATCH input as a single forward)
- Total bus: **150–180 → 20–40 pkts/s**
- `esp_now_send` err rate: should drop from 79 % to near zero
- `channel_mgr` quarantine cycle: **expected to disappear** for RIGHT and LEFT
- Visual flashes / box dropouts: should resolve

This is **not** a rate limiter. It's a packing fix. **No data point is dropped. No rate is capped. Same latency** — actually slightly lower, because the radio isn't fighting itself.

---

## 3. Why the user's other instincts are right, but later (roadmap)

The surgical fix buys headroom but **does not eliminate the architectural fragility**. As more slaves come online (gps, pod1, pod2, BMS, relay boards) the bus will saturate again. Items below are tracked as future work:

### 3.1 Per-peer TX queue with last-value-wins coalescing (the "better pipe")

Today `channel_mgr_send_to_node()` is **synchronous** — it calls `esp_now_send`, sleeps in backoff on retry, blocks the dispatcher. Producer threads pay the radio's pain.

Target architecture:

```
producer (CH1 worker)            tx worker (per peer)
   │                                │
   ├─► peer_tx_post(RIGHT,          ├─► drain dirty set
   │       dp_id, value)            ├─► batch coalesced DPs
   │   (non-blocking,               ├─► esp_now_send() once
   │    overwrites prior            ├─► await ACK callback
   │    value for same dp_id)       └─► loop
   ▼
```

Properties:
- **Last-value-wins**: 100 RPM updates in 10 ms ≡ 1 update on the wire.
- **Per-peer parallelism**: a slow ACK on LEFT doesn't block RIGHT.
- **Producers never block**: dispatcher always returns in O(1).
- **Bounded memory**: per-peer DP map is fixed-size (~256 DP × 8 bytes = 2 KB).

### 3.2 Subscriber-aware forwarding (the "drop at source" generalised)

Today center forwards **every** DP to RIGHT regardless of whether right's current page renders it. Right's page 0 uses 2 DPs out of ~40. **~95 % of forwards are wasted radio bandwidth.**

Target: each slave publishes a small `OPENDASH_CMD_SUBSCRIBE` frame on page change containing a bitset / list of DP IDs it wants. Center filters its outbound at enqueue time.

### 3.3 Per-DP QoS classes (the "governor on slow stuff only")

Define three classes in the data model:
- **SAFETY** (RPM, BOOST, OIL_PRESS, COOLANT_TEMP): uncapped, highest priority queue.
- **UI_DRIVER** (SPEED, AFR, EGTs): cap at 20 Hz per DP (one update per 50 ms).
- **TELEMETRY** (cell voltages, ambient, GPS fix quality): cap at 2 Hz per DP.

Enforced by the per-peer queue (3.1) at insert time: if `now - last_sent_us[dp] < min_interval[class]`, **overwrite** instead of enqueueing a duplicate. Lossless for SAFETY, last-value for the rest.

### 3.4 Instrumentation

Per-peer stats already exist (`channel_stats_t`). Extend with:
- `coalesced_drops` — how many enqueues overwrote a not-yet-sent value
- `subscribe_filter_drops` — DPs dropped because peer didn't subscribe
- `tx_queue_high_water` per peer

Surface in center's Device Management UI (already has a status pane per node — see TODO #11 / #16 work).

---

## 4. Acceptance criterion

Surgical fix is "done" when:

1. `channel_mgr` quarantine WARN lines for RIGHT drop to ≤1 per minute (was ~25/min).
2. `CH1 flow: err=...` stays under 5 % of `tx`.
3. No visible flashes in 5 min of normal driving simulation.
4. Tearing baseline re-run shows no regression (independent investigation).

Roadmap items (3.1–3.4) are tracked separately in `TODO.md`; they are **not** required to declare the immediate bug fixed.
