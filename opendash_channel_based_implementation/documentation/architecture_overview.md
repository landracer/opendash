<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OpenDash Channel-Based ESP-NOW Architecture

> **⚠️ ARCHITECTURE RULE: NO POLLING / NO PINGING — EVER.**

## 1. Why Channels?

The legacy 20 ms PING-broadcast loop caused:

| Problem | Impact |
|---------|--------|
| O(N) traffic growth | Every new node added +1 PING + +1 ACK per cycle |
| Relay toggle lag | Seconds-long delays due to LVGL lock contention |
| Wasted CPU | Both cores saturated cycling through 18 nodes every 20 ms |
| False offline flaps | Missed PINGs ≠ offline, but old code treated them that way |

The channel-based architecture fixes all of these by flipping the model:

- **Nodes push data to Center when they have something new.**
- **Center pushes commands (relay ON/OFF, reboot) to nodes through the CONTROL channel.**
- **No discovery loop.  Nodes self-identify via ANNOUNCE on first contact.**

## 2. Four Priority Channels

```
┌──────────┐     ┌──────────────────────────────────────────────────┐
│          │ CH0 │ CRITICAL   10 ms   GPS, BMS, engine vitals      │
│  ESP-NOW │ CH1 │ MEDIUM     50 ms   Pods, gauge data, display    │
│  Master  │ CH2 │ LOW       200 ms   Relay feedback, diagnostics  │
│ (CENTER) │ CH3 │ CONTROL     5 ms   Relay ON/OFF, OTA, reboot   │
│          │     │            (immediate — highest FreeRTOS pri)    │
└──────────┘     └──────────────────────────────────────────────────┘
```

Each channel has:

- **Dedicated FreeRTOS queue** — messages never cross channels.
- **Own worker task** — different core affinity and stack priority.
- **Independent offline timeout** — CRITICAL = 2 s, MEDIUM = 5 s, LOW = 15 s, CONTROL = never.
- **Own retry policy** — exponential backoff, channel-specific max retries.

## 3. Data Flow

```
       GPS ──push──┐
       BMS ──push──┤       ┌──── CH0 queue ──── critical_task ──→ UI + gauge forward
                   ▼       │
  [ESP-NOW RX] → DISPATCHER ──→ CH1 queue ──── medium_task ──→ UI + pod relay
                   ▲       │
      POD1 ──push──┤       ├──── CH2 queue ──── low_task ──→ logging / diag
      POD2 ──push──┤       │
     RELAY ──push──┘       └──── CH3 queue ──── control_task ←── Touch UI
```

### Inbound Path (node → Center)

1. Node sends DATA_RESPONSE or STATUS_REPORT via ESP-NOW.
2. **Dispatcher task** receives from raw ESP-NOW queue (blocks until message arrives).
3. Dispatcher identifies sender by MAC → node registry lookup.
4. Unknown sender with ANNOUNCE / STATUS_REPORT → auto-registered.
5. Message routed to correct channel queue based on `NODE_DEFAULT_CHANNEL[]`.
6. Channel worker drains its queue, applies delta check, pushes to UI.

### Outbound Path (Center → node)

1. Touch callback or timer creates a typed command struct.
2. Public API (`espnow_master_send_relay_command()`) serialises and calls `channel_mgr_send_to_node()`.
3. Channel manager looks up the node's MAC, serialises, sends via `opendash_espnow_send()`.
4. On failure — exponential backoff retry (base 5 ms × 2^attempt).

### Offline Detection (data-absence model)

- Each channel has a timeout threshold (e.g. CRITICAL = 2000 ms).
- A 1 Hz timer scans all registered nodes: if `(now - last_seen) > threshold[ch]`, mark offline.
- **No PINGs.  No heartbeats from master.**  Nodes push; absence = offline.

## 4. Node Types and Channel Mapping

| Node               | Channel   | DP Range      | Capabilities |
|---|---|---|---|
| GPS                | CRITICAL  | 0x0601–0x06FF | PUSH, UART   |
| BMS                | CRITICAL  | 0x0501–0x05FF | PUSH, CAN    |
| LEFT pod           | MEDIUM    | 0x0201–0x02FF | PUSH, RECV   |
| RIGHT pod          | MEDIUM    | 0x0201–0x02FF | PUSH, RECV   |
| POD1–POD8          | MEDIUM    | 0x0201–0x02FF | PUSH, RECV   |
| RELAY_4CH          | LOW       | 0x0401–0x04FF | RELAY_CMD    |
| RELAY_8CH_A/B      | LOW       | 0x0401–0x04FF | RELAY_CMD    |
| MOS_4CH_A/B        | LOW       | 0x0401–0x04FF | RELAY_CMD    |

## 5. Task Map

| Task | Core | Priority | Stack | Description |
|---|---|---|---|---|
| `espnow_dispatch` | 0 | 4 | 8 KB | Drains raw RX, routes to channels |
| `ch_critical` | 0 | 5 | 6 KB | GPS/BMS/engine → UI + forward |
| `ch_medium` | 0 | 4 | 6 KB | Pod data → UI |
| `ch_low` | 1 | 3 | 6 KB | Diagnostics, relay state |
| `ch_control` | 0 | 6 | 6 KB | Relay commands, OTA, reboot |
| Timeout timer | — | — | — | 1 Hz stale-node check |

## 6. Protocol Frame (unchanged)

```
SYNC (0xAA) │ CMD (1 B) │ LEN (1 B) │ PAYLOAD (0–248 B) │ CHECKSUM (1 B)
```

Message types used by the channel system:

| Hex  | Name | Direction |
|------|------|-----------|
| 0x01 | DATA_POINT | node → Center |
| 0x02 | STATUS_REPORT | node → Center |
| 0x03 | RELAY_CMD | Center → relay |
| 0x04 | SYSTEM_CMD | Center → node |
| 0x05 | CONFIG | bidirectional |
| 0x06 | ANNOUNCE | node → Center |
| 0x07 | BATCH_DP | node → Center |

## 7. Key Design Decisions

1. **Push-only slave nodes** — Slaves never request data from master. They push when ready.
2. **One dispatcher, four workers** — Single point of deserialization prevents duplicate parsing.
3. **Delta tracking** — `channel_mgr_dp_changed()` suppresses duplicate UI updates (float epsilon 0.001).
4. **Relay commands on CONTROL** — Highest FreeRTOS priority (6) ensures <5 ms toggle latency.
5. **Low channel on core 1** — Keeps diagnostics from starving LVGL on core 0.
6. **Auto-registration** — No static MAC table needed.  Nodes self-identify on first ANNOUNCE.

## 8. Files

| File | Purpose |
|------|---------|
| `channel_config.h` | Tuning knobs — intervals, timeouts, retries, queue sizes |
| `node_definitions.h` | Node→channel map, capabilities, name tables |
| `channel_management.h/c` | Routing engine — queues, registry, delta tracking, stats |
| `espnow_master.h/c` | Master controller — dispatcher, workers, public API |
| `examples/channel_assignment_example.c` | Working code snippets |