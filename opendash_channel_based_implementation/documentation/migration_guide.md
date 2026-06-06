<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# Migration Guide: Polling → Channel-Based ESP-NOW

> **⚠️ ARCHITECTURE RULE: NO POLLING / NO PINGING — EVER.**
> This migration is a one-way door.  Do not re-introduce polling loops.

## What Changes

| Aspect | Old (Polling) | New (Channel-Based) |
|--------|--------------|---------------------|
| Discovery | 20 ms PING broadcast, all nodes reply | Nodes send ANNOUNCE on boot — one-time |
| Data flow | Master requests, slave responds | Slave pushes data when it changes |
| Offline detect | 3 missed PINGs → offline | Data-absence timeout per channel |
| Relay toggle | Command queued behind poll cycle (seconds lag) | CONTROL channel, <5 ms latency |
| Network traffic | O(N) every 20 ms | O(events) — 80-90% reduction |
| Task model | Single 20 ms superloop | 1 dispatcher + 4 channel workers + timer |

## Prerequisites

- ESP-IDF v5.3+ (slaves) / v6.1 (center)
- Backup current working firmware (`center-bk/`, `gps-bk/`)
- All nodes must be reflashed with channel-aware firmware

## Step-by-Step Migration

### Step 1: Copy New Files

```bash
# Into center/main/
cp channel_config.h       ../center/main/
cp node_definitions.h     ../center/main/
cp channel_management.h   ../center/main/
cp channel_management.c   ../center/main/
cp espnow_master.h        ../center/main/
cp espnow_master.c        ../center/main/
```

### Step 2: Update CMakeLists.txt

Add the new source files to the center build:

```cmake
idf_component_register(
    SRCS
        "main.c"
        "espnow_master.c"
        "channel_management.c"
        # ... existing sources
    INCLUDE_DIRS
        "."
        "../../common/include"
)
```

### Step 3: Remove Old Polling Code

In `app_main()` or wherever the old ESP-NOW master task is created:

```c
/* DELETE this: */
// xTaskCreatePinnedToCore(espnow_master_task, "espnow_master", ...);

/* REPLACE with: */
ESP_ERROR_CHECK(espnow_master_init());
ESP_ERROR_CHECK(espnow_master_start());
```

Remove any `espnow_ping_all()`, `espnow_poll_nodes()`, or similar functions.

### Step 4: Update Slave Nodes

Each slave must be updated to:

1. **Send ANNOUNCE on boot** (includes node_type byte in payload).
2. **Push data points** via `DATA_RESPONSE` instead of waiting for requests.
3. **Remove PING/ACK handling** — don't respond to PINGs (there won't be any).

Example slave boot sequence:

```c
void app_main(void)
{
    opendash_espnow_init(MY_NODE_TYPE);

    /* Send ANNOUNCE to master so it auto-registers us */
    uint8_t payload[1] = { MY_NODE_TYPE };
    opendash_i2c_msg_t msg;
    opendash_i2c_build_msg(&msg, CHANNEL_MSG_ANNOUNCE, payload, 1);

    uint8_t tx_buf[32];
    uint16_t tx_len = 0;
    opendash_i2c_serialize(&msg, tx_buf, &tx_len);
    opendash_espnow_broadcast(tx_buf, tx_len);

    /* Now start pushing data when ready */
    xTaskCreate(sensor_push_task, "sensor", 4096, NULL, 3, NULL);
}
```

### Step 5: Build → Flash → Monitor

```bash
# Center
cd center && idf.py -p /dev/ttyACM0 build flash monitor

# GPS slave
cd gps && idf.py -p /dev/ttyACM1 build flash monitor

# Left gauge
cd left && idf.py -p /dev/ttyACM2 build flash monitor
```

Monitor for at least 60 seconds — check for:
- [ ] `"Channel-based master running — 4 channels, zero polling"` in center log
- [ ] `"<NODE> ONLINE (RSSI=...)"` messages as slaves connect
- [ ] No `PING` or `poll` in any log output
- [ ] Relay toggles respond in <100 ms from touch

### Step 6: Validate

```
✅  GPS data appears on center display within 2 seconds of boot
✅  BMS data appears on center display
✅  LEFT + RIGHT gauges receive forwarded data
✅  Relay toggle from touch UI responds in <100 ms
✅  Unplugging a node shows "OFFLINE" within the channel timeout
✅  Reconnecting a node shows "ONLINE" without reboot
✅  `channel_mgr_log_stats()` shows non-zero rx_count, zero dropped
```

## Rollback

If the migration fails:

1. Flash the backup firmware from `center-bk/` / `gps-bk/`.
2. File a bug with the monitor log attached.
3. **Do NOT re-introduce polling as a workaround.**

## API Quick Reference

| Function | Purpose |
|----------|---------|
| `espnow_master_init()` | Init transport + channel manager |
| `espnow_master_start()` | Spawn dispatcher + workers + timer |
| `espnow_master_get_status()` | Read node online/offline snapshot |
| `espnow_master_send_data_point()` | Push DP to a specific node (delta-checked) |
| `espnow_master_send_relay_command()` | Toggle relay channel (with retry) |
| `espnow_master_send_system_subcmd()` | Send system command (reboot, OTA, etc.) |
| `channel_mgr_get_stats()` | Per-channel traffic statistics |
| `channel_mgr_log_stats()` | Dump all channel stats to ESP_LOG |

## Tuning

All timing/threshold knobs are in `channel_config.h`:

| Define | Default | What it controls |
|--------|---------|-----------------|
| `CHANNEL_CRITICAL_INTERVAL_MS` | 10 | Critical worker wake rate |
| `CHANNEL_MEDIUM_INTERVAL_MS` | 50 | Medium worker wake rate |
| `CHANNEL_LOW_INTERVAL_MS` | 200 | Low worker wake rate |
| `CHANNEL_CONTROL_INTERVAL_MS` | 5 | Control worker wake rate |
| `CHANNEL_CRITICAL_OFFLINE_MS` | 2000 | GPS/BMS timeout |
| `CHANNEL_MEDIUM_OFFLINE_MS` | 5000 | Pod timeout |
| `CHANNEL_LOW_OFFLINE_MS` | 15000 | Relay/diag timeout |
| `CHANNEL_QUEUE_DEPTH` | 32 | Per-channel queue items |
| `CHANNEL_CRITICAL_MAX_RETRIES` | 3 | Retry count |
| `CHANNEL_RETRY_BASE_MS` | 5 | Backoff base interval |