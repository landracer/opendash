<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OpenDash Relay / MOS — ESP-NOW Protocol Guide

> **Wiki:** How-to, dos, and don'ts for all relay and MOS FET slave nodes.  
> **Boards covered:** relay-8ch-a, relay-8ch-b, relay-4ch-hd, mos-4ch-a, mos-4ch-b

---

## Architecture — Silent Slave Design

Every relay/MOS node is a **pure silent slave**:

- Receives `SET_RELAY` → applies state atomically → **no response**
- Ignores `PING` (does NOT broadcast RELAY_STATUS back)
- Sends one 10-second heartbeat broadcast for center discovery
- **Master (center) owns all relay state** — the node never reports back

This design was adopted after the original "respond to everything" pattern caused:

| Symptom | Root Cause |
|---|---|
| Relay clicks taking 1–2 seconds | ESP-NOW TX queue (10 slots) full from PING storm |
| `ESP_ERR_ESPNOW_NO_MEM` in logs | 16+ PINGs/sec filling queue before any relay packet could be sent |
| Silent relay drops | relay packet arrived at a full queue, dropped with no error shown on UI |

**The fix:** PING rate 2 s (center-side) + relay ignores PING + dedicated `relay_sender_task` with retry.

---

## Packet Format

### New Mask Format (preferred)

```
SET_RELAY payload: [ 0xCA, node_id, relay_mask, seq ]
```

| Byte | Value | Notes |
|------|-------|-------|
| `[0]` | `0xCA` | Magic — distinguishes mask format from legacy |
| `[1]` | `OPENDASH_NODE_*` | Target node ID — node ignores if not its own |
| `[2]` | 8-bit mask | Bit N = channel N state (1=ON, 0=OFF) |
| `[3]` | seq counter | Monotonically increasing, wraps at 255 |

All 8 channels (or 4 for 4-ch boards) are set atomically in one packet.  
Master calls `espnow_master_send_relay_command()` which queues to `s_relay_cmd_queue`.

### Legacy Per-Channel Format (still supported)

```
SET_RELAY payload: [ node_id, channel, state, pwm_duty ]   ← 4-byte
SET_RELAY payload: [ channel, state ]                       ← 2-byte
```

Used by the center UI for PWM control (MOS boards). Works alongside mask format.

### Heartbeat (slave → broadcast)

```
STATUS_REPORT payload: [ node_id, 0x01, 0x00 ]
```

Broadcast every 10 seconds. Center uses this for node discovery only.

---

## Node IDs

| Board | `OPENDASH_NODE_*` constant | Address |
|-------|---------------------------|---------|
| relay-8ch-a | `OPENDASH_NODE_RELAY_8CH_A` | `0x10` |
| relay-8ch-b | `OPENDASH_NODE_RELAY_8CH_B` | `0x11` |
| relay-4ch-hd | `OPENDASH_NODE_RELAY_4CH` | see common |
| mos-4ch-a | `OPENDASH_NODE_MOS_4CH_A` | see common |
| mos-4ch-b | `OPENDASH_NODE_MOS_4CH_B` | see common |

---

## GPIO Assignments

### relay-8ch-a / relay-8ch-b — LCTech 8-channel ESP32 module

Both boards are **identical hardware**. GPIOs verified with multimeter.

| Channel | GPIO | Notes |
|---------|------|-------|
| CH0 | 32 | |
| CH1 | 33 | |
| CH2 | 25 | |
| CH3 | 26 | |
| CH4 | 27 | |
| CH5 | 14 | |
| CH6 | 12 | |
| CH7 | 13 | |

- **Polarity:** `OPENDASH_RELAY_ACTIVE_LOW` (GPIO LOW energizes relay)
- **Type:** `OPENDASH_RELAY_TYPE_RELAY`
- **Programming port:** `/dev/ttyUSB1` via FTDI (shared — swap wires for each board)

### relay-4ch-hd — LCTech 4-CH HD Relay

**GPIOs TBD** — discover with multimeter when hardware arrives.  
Update `relay_config` channels in `relay-4ch-hd/main/main.c`.  
Labels: RAD FAN 1, RAD FAN 2, WATER PUMP, FUEL PUMP.

### mos-4ch-a / mos-4ch-b — 4-Way MOS FET Module

**GPIOs TBD** — discover pinout when installed.  
Update `mos_config` channels in the respective `main/main.c`.  
- **Polarity:** `OPENDASH_RELAY_ACTIVE_HIGH` (MOS FET modules are typically active-high)
- **PWM:** 1 kHz, 8-bit (0–255 duty cycle)

---

## Flashing Relay Boards (LCTech via FTDI)

LCTech relay boards use ESP32 WROOM-32E with NO auto-reset circuit.  
The FTDI adapter does not wire DTR/RTS to BOOT/EN.  
**You must manually enter download mode every time you flash.**

### Flash Procedure

1. Hold the **BOOT** button (GPIO0) on the relay board
2. Press and release **RESET** (or briefly disconnect/reconnect power)
3. Release **BOOT**
4. Immediately run:

```bash
cd /home/sysadmin/Documents/rAtTrax-Dash/opendash/<board>
source ~/Documents/esp-ide/esp-idf/export.sh
idf.py -p /dev/ttyUSB1 flash
```

5. Press RESET once more after flash completes to boot normally

### If Flash Fails — "Wrong boot mode detected (0x13)"

The chip is NOT in download mode. Retry step 1–3.  
`0x13` means GPIO0 was HIGH at boot → normal run mode. Needs GPIO0 LOW to enter ROM loader.

### Port Sharing (relay-8ch-a vs relay-8ch-b)

Both boards share `/dev/ttyUSB1`. Physically swap the FTDI wires between boards.  
After flash, use BLE OTA (`OPENDASH_SUBCMD_ENTER_BT_OTA`) for subsequent updates.

---

## Center-Side Configuration

File: `center/main/espnow_master.c`

### Key Parameters

```c
#define RELAY_PING_INTERVAL_MS   2000   /* DO NOT lower this — was the root cause */
#define MAX_NODES                16
```

### relay_sender_task

A dedicated FreeRTOS task (priority 5, pinned to core 0) drains `s_relay_cmd_queue`.  
- Blocks on queue with `portMAX_DELAY` — zero CPU when idle
- Retries up to 5× on `ESP_ERR_ESPNOW_NO_MEM` with 5 ms backoff
- Uses **local stack-allocated** `tx_buf` — no shared buffer race condition

### s_relay_masks[] Array

```c
static uint8_t s_relay_masks[MAX_NODES];  /* index = node_id */
```

Center maintains full relay state. When a button is tapped:
1. UI toggles the bit in `s_relay_masks[node_id]`
2. Queues `{ node_id, mask }` to `s_relay_cmd_queue`
3. `relay_sender_task` picks it up and sends `[0xCA, node_id, mask, seq]`
4. Node applies all bits silently

---

## Do's

- **DO** keep `RELAY_PING_INTERVAL_MS` at 2000 ms or higher — 50 ms caused the original failure
- **DO** add new boards by assigning a unique `OPENDASH_NODE_*` constant and registering the MAC on center
- **DO** use the mask format for all relay boards — it's atomic and eliminates partial-state race
- **DO** update GPIO config (`gpio_num`, `enabled = true`) before flashing to a real board
- **DO** use BLE OTA for firmware updates after initial FTDI flash
- **DO** check self-test mode (hold GPIO0 at boot on 8-ch boards) to verify wiring before wiring to car
- **DO** set `enabled = false` and `gpio_num = -1` for channels with unknown GPIOs during staging

## Don'ts

- **DON'T** add `send_relay_status()` or any broadcast back after receiving SET_RELAY — this recreates the storm
- **DON'T** send STATUS_REPORT responses to PING — let the heartbeat handle discovery
- **DON'T** put relay command sends in the main espnow loop without a queue — race on `s_tx_buf`
- **DON'T** use `ESP_ERR_ESPNOW_NO_MEM` as a silent failure — always retry with backoff
- **DON'T** share a single `s_tx_buf[]` between the espnow loop and the relay sender — they run on different tasks
- **DON'T** flash LCTech boards without manually pressing BOOT+RESET first — no auto-reset circuit
- **DON'T** increase PING rate without also increasing the TX queue size or throttling relay sends

---

## Adding a New Relay/MOS Board

1. Assign a new node ID constant in `common/include/opendash_common.h`
2. Copy `relay-8ch-a/` (relay) or `mos-4ch-a/` (MOS) as the base firmware
3. Change node ID constant (`OPENDASH_NODE_*`) and TAG string
4. Update GPIO config in `relay_config` / `mos_config` after physical pinout discovery
5. Build and flash with manual BOOT+RESET procedure
6. Center will discover the node automatically on next heartbeat (within 10 s)
7. Add button/label mapping in `center/main/ui_manager.c` relay grid

---

## Debugging

### Quick Health Check from Center Monitor

```
I (xxx) espnow_master: Node 0x10 ONLINE (relay-8ch-a)
I (xxx) relay_sender: Relay 0x10 mask 0x05 sent ESP_OK
```

### Relay Not Responding

1. Check center logs for `ESP_ERR_ESPNOW_NO_MEM` — if present, PING rate is too high
2. Verify node heartbeat appearing in center log every ~10 s
3. Confirm relay board powered and booted (RESET after flash, not stuck in bootloader)
4. Check `s_relay_masks[node_idx]` is being set — add a log to `espnow_master_send_relay_command()`

### Relay Fires Once Then Stops

- Node probably crashed — check for stack overflow (increase task stack if needed)
- Or GPIO conflict — verify no two channels share a GPIO

### Build Fails with Undefined `send_relay_status`

The staged boards (mos-4ch-a/b, relay-4ch-hd) have `send_to_center()` and `send_relay_status()` removed.  
If you see this error, the dispatch_message update was not applied — re-apply from the silent-slave pattern.
