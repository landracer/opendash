<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# Relay & MOS FET Controller Nodes

## Overview

OpenDash controls up to 5 headless ESP32 nodes for relay and MOS FET switching.
These boards have no display — they receive commands from Center via ESP-NOW and
drive GPIO outputs to control fans, pumps, lights, and other accessories.

---

## Node Summary

| Node | Type | Channels | GPIOs | Logic | Node ID |
|------|------|----------|-------|-------|---------|
| `relay-4ch-hd` | HD Relay | 4 | 12, 13, 14, 15 | Active-LOW | `OPENDASH_NODE_RELAY_4CH` |
| `relay-8ch-a` | Relay Bank A | 8 | 32,33,25,26,27,14,12,13 | Active-LOW | `OPENDASH_NODE_RELAY_8CH_A` |
| `relay-8ch-b` | Relay Bank B | 8 | 32,33,25,26,27,14,12,13 | Active-LOW | `OPENDASH_NODE_RELAY_8CH_B` |
| `mos-4ch-a` | MOS FET A | 4 | 16, 17, 26, 27 | Active-HIGH | `OPENDASH_NODE_MOS_4CH_A` |
| `mos-4ch-b` | MOS FET B | 4 | 16, 17, 26, 27 | Active-HIGH | `OPENDASH_NODE_MOS_4CH_B` |

All use standard ESP32-WROOM-32E (4MB flash, no PSRAM) connected via FTDI FT232 TTL.

---

## ESP-NOW Protocol

### SET_RELAY (Center → Node)

Bitmask format: `[0xCA, node_id, mask, seq]`

- `0xCA` = magic byte (distinguishes from legacy per-channel format)
- `mask` = 8-bit bitmask, bit N = channel N state (1=ON, 0=OFF)
- `seq` = monotonic sequence number for debug/dedup

### REQUEST_RELAY_STATUS (Center → Node)

Center sends this after 2.5 seconds of idle (no button presses) to verify
the node's actual GPIO state matches what center expects.

Payload: `[node_id]`

### RELAY_STATUS (Node → Center)

Compact audit response: `[0xCA, node_id, actual_mask]`

Center compares `actual_mask` vs its own `s_relay_masks[]`. On mismatch,
center re-sends the full mask (channel=0xFF sentinel = "resend current mask").

---

## State Audit System

```
User taps relay box on Center
  → optimistic UI update (immediate visual feedback)
  → SET_RELAY queued to relay_sender_task
  → ESP-NOW unicast to node
  → node applies mask, ACKs via ESP-NOW MAC layer

After 2.5s idle:
  → Center sends REQUEST_RELAY_STATUS to each active node
  → Node responds with actual GPIO mask
  → Center compares: MATCH → ✓ logged | MISMATCH → retry mask + update UI
```

The audit catches dropped ESP-NOW packets. Since ESP-NOW is ~95% reliable
over WiFi, occasional mismatches are expected during rapid toggling. The
retry mechanism ensures convergence.

---

## LED Status Indicator (MOS Boards Only)

Both `mos-4ch-a` and `mos-4ch-b` drive an LED on **GPIO 23** with a 3-second
cycle (60 ticks @ 50ms per tick):

| Phase (tick) | Behavior |
|---|---|
| 0–1 | Heartbeat pulse 1 (100ms ON) |
| 2–3 | Gap (OFF) |
| 4–5 | Heartbeat pulse 2 (100ms ON) |
| 6–11 | Pause (OFF) |
| 12+ | Activity flashes (see below) |

### Activity Flashes (based on active channel count)

| Active Channels | Pattern |
|---|---|
| 0 | Heartbeat only — no additional flashes |
| 1–3 | N quick flashes (200ms on / 200ms off each) |
| 4 (all on) | Long solid pulse (600ms) |

### Fault Mode

If center MAC has not been learned (no heartbeat received), the LED does
a **rapid steady flash** (100ms on/off) instead of the normal pattern.
This indicates the board is alive but has no center connection.

---

## Staggered Heartbeats

Each node sends a STATUS_REPORT at a unique interval to avoid collisions:

| Node | Interval |
|------|----------|
| GPS | 30s |
| Left | 35s |
| Right | 40s |
| BMS | 45s |
| relay-8ch-a | 50s |
| relay-8ch-b | 55s |
| relay-4ch-hd | 60s |
| mos-4ch-a | 65s |
| mos-4ch-b | 70s |

---

## Flashing via FTDI

These boards use an external FTDI FT232 TTL adapter (no onboard USB-to-serial).
The FTDI's RTS/DTR lines are not wired for auto-reset on most of these boards.

### Flash Procedure

1. Connect FTDI to `/dev/ttyUSB0`
2. Hold **BOOT/GPIO0** button on the board
3. Power cycle (unplug/replug) while holding BOOT
4. Release BOOT after ~1 second
5. Flash with esptool (from the build directory):

```bash
python3 -m esptool --chip esp32 --port /dev/ttyUSB0 --baud 460800 \
  --before no-reset --after no-reset \
  write-flash --flash-mode dio --flash-freq 40m --flash-size 4MB \
  0x1000 bootloader/bootloader.bin \
  0x8000 partition_table/partition-table.bin \
  0x10000 <app_name>.bin
```

6. Power cycle the board after flash completes

> **CRITICAL:** Must use `--before no-reset --after no-reset`. The default
> `idf.py flash` uses `--before default-reset` which toggles RTS/DTR and
> kicks the ESP32 out of bootloader mid-flash.

### Brownout Protection

All FTDI-powered boards have `CONFIG_ESP_BROWNOUT_DET=n` in their
`sdkconfig.defaults`. The FTDI USB 5V rail cannot supply enough current
for ESP32 + relay/MOS driver board, causing brownout reboot loops without
this setting.

After modifying sdkconfig.defaults, you must regenerate:
```bash
rm sdkconfig && idf.py fullclean && idf.py build
```

---

## Center UI (Relay Control Grid)

The center display has a 7×4 grid of toggle boxes accessed via DEBUG MODE:

- **Row 1:** relay-4ch-hd (boxes 0–3), relay-8ch-a (boxes 4–11)
- **Row 2:** relay-8ch-b (boxes 12–19), mos-4ch-a (boxes 20–23), mos-4ch-b (boxes 24–27)

Touch a box to toggle → immediate optimistic UI update → ESP-NOW command sent.
Orange border = node offline (command still queued).

---

## Boost Integration (MOS-4CH-A/B only)

The MOS-4CH-A and MOS-4CH-B nodes support boost control functionality with:

- **Boost PWM output** on `MOS_BOOST_PWM_CHANNEL=3` (GPIO27, CH3)
- **50 Hz compute task** (`boost_compute_task`) for closed-loop control
- **32-point RPM maps × 6 gears × 3 modes** (OFF/NORMAL/RACE)
- **Dual-gain PID** (aggressive/conservative) for responsive control
- **NORMAL defaults**: 14 psi, RACE defaults: 36 psi
- **NVS persistence** with schema version 2
- **Safety flags** (8 bits) for monitoring system health
- **Paginated 2×16 cell editor** in Center UI for map configuration
- **Mode toggle** in Center UI for switching between OFF/NORMAL/RACE

### Safety Override

The boost controller implements several safety mechanisms:
- **Channel ownership** with conflict resolution via `prev_mask & ~mask` pattern
- **BLE OTA handler** kills all PWM+FETs before entry to prevent conflicts
- **Parachute fire** is idempotent (no effect if already fired)
- **ARM state** is never NVS-persisted (always boots DISARMED)
- **Center MAC discovery filter** excludes SYSTEM command to prevent GPS TIME_SYNC from hijacking relay/parachute unicast routing

## Parachute Deployment Actuator

The MOS-4CH-A/B nodes support parachute deployment with:

- **Thresholds**: deploy=45°, warning=25°, sustain=200ms, rate=300°/s
- **LATCH (default) or PULSE mode** via `OPENDASH_PARACHUTE_FLAG_FIRE_PULSE`
- **Distributed voting** with rollover detector
- **Channel conflict safeguards** for shared GPIO channels (CH0=GPIO16, CH1=GPIO17, CH2=GPIO26, CH3=GPIO27)
- **ARM state** is transient (never NVS-persisted — always boots DISARMED)
- **Config NVS-persisted** for all system parameters

### Protocol

| Direction | CMD | Opcode | Payload |
|-----------|-----|--------|---------|
| C→S | `PARACHUTE_SET_CONFIG` | `0x27` | `opendash_parachute_config_t` |
| C→S | `PARACHUTE_SET_ARM` | `0x28` | `uint8_t` (0=DISARMED, 1=ARMED) |
| C→S | `PARACHUTE_PULL_ALL` | `0x29` | `uint8_t` (0=DISARMED, 1=ARMED) |
| C→S | `PARACHUTE_DEPLOY` | `0x2A` | `uint8_t` (0=DISARMED, 1=ARMED) |
| C→S | `PARACHUTE_CALIBRATE` | `0x2B` | `uint8_t` (0=DISARMED, 1=ARMED) |
| S→C | `PARACHUTE_STATUS` | `0x95` | `opendash_parachute_status_t` |
| S→C | `PARACHUTE_VOTE` | `0x96` | `opendash_parachute_vote_t` |

### Channel Conflict Safeguards

All three systems (boost, relay, parachute) share the same 4 GPIO channels:
- CH0 = GPIO16
- CH1 = GPIO17
- CH2 = GPIO26
- CH3 = GPIO27

The system implements several safeguards to prevent conflicts:
1. **Boost compute task** releases channels it no longer owns via `prev_mask & ~mask` pattern
2. **BLE OTA handler** kills all PWM+FETs before entry to prevent conflicts
3. **Parachute fire** is idempotent (no effect if already fired)
4. **ARM state** is never NVS-persisted (always boots DISARMED)
5. **Center MAC discovery filter** excludes SYSTEM command to prevent GPS TIME_SYNC from hijacking relay/parachute unicast routing

## Relevant Source Files

| File | Purpose |
|------|---------|
| `common/include/opendash_relay.h` | Shared relay/MOS driver API |
| `common/src/opendash_relay.c` | GPIO init, mask apply, channel control |
| `common/include/opendash_common.h` | Node type definitions, `OPENDASH_NODE_IS_RELAY()` macro |
| `common/include/opendash_protocol.h` | CMD_SET_RELAY, CMD_REQUEST_RELAY_STATUS, CMD_RELAY_STATUS, PARACHUTE_* opcodes |
| `common/include/opendash_boost.h` | Boost controller API |
| `common/src/opendash_boost.c` | Boost controller implementation |
| `common/include/opendash_parachute.h` | Parachute controller API |
| `mos-4ch-a/main/main.c` | MOS-A slave runtime — dispatch + compute / telem tasks |
| `mos-4ch-b/main/main.c` | MOS-B slave runtime (mirrors MOS-A) |
| `center/main/espnow_master.c` | relay_sender_task, audit logic, RELAY_STATUS handler |
| `center/main/ui_manager.c` | Relay grid, touch callbacks, `ui_manager_update_relay_status()` |

---

*Last updated: April 2026*
