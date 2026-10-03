<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OpenDash Inter-Node Protocol (ESP-NOW)

> **There is no wired inter-node bus in this system, and there never will be.**
> Nodes are separate ESP32-S3 boards in separate enclosures with no shared
> signal between them. All node-to-node traffic is **ESP-NOW** (WiFi
> peer-to-peer, no AP, no DHCP). I2C in this project is *only* a local,
> point-to-point peripheral bus inside a single enclosure - GT911/CST9217
> touch, PCF85063 RTC, TCA9554 IO expander, QMI8658 IMU, LC76G GNSS. The GPIOs
> once earmarked for a wired inter-node bus are used by those on-board
> peripherals. Do not describe I2C as a network transport anywhere in this
> repo, and do not reintroduce one.
>
> Historical note: before v0.4 the same frame was carried over a wired bus,
> which is why some older code comments still say "I2C". The frame is
> transport-agnostic; the transport is ESP-NOW.

Source of truth. Code beats prose - if this file and a header disagree, the
header wins and this file has a bug:

| Concern | File |
|---------|------|
| Frame + opcodes | `common/include/opendash_protocol.h`, `common/src/opendash_protocol.c` |
| Priority channels, retries, quarantine | `common/include/channel_config.h`, `common/src/channel_management.c` |
| Rate-based health | `common/include/node_health.h` |
| Logical node identity | `opendash_node_t` in `common/include/opendash_common.h` |
| Data point IDs | `common/include/opendash_data_points.h`, `docs/data-points.md` |
| Transport init / peers | `common/src/opendash_espnow.c` |
| End-to-end message journey | `docs/DATAFLOW.md` |

---

## 1. Frame format

Every message, in both directions:

```
+--------------+---------+------------+------------------+----------------+
| SYNC  0xAA   | CMD 1B  | LENGTH 1B  | PAYLOAD  0-248 B | CHECKSUM 1B    |
+--------------+---------+------------+------------------+----------------+
```

* `OPENDASH_MSG_SYNC = 0xAA`; header 3 bytes (`OPENDASH_MSG_HEADER_SIZE`);
  max frame 252 bytes (`OPENDASH_MSG_MAX_SIZE`).
* **CHECKSUM** = XOR of `SYNC`, `CMD`, `LENGTH` and every payload byte.
  `opendash_msg_validate()` rejects a frame with a bad sync byte or checksum;
  a rejected frame is dropped and logged, never queued.
* Helpers: `opendash_msg_build()`, `opendash_msg_validate()`,
  `opendash_msg_serialize()`, `opendash_msg_deserialize()`.

## 2. Addressing and discovery

* **On the wire** a node is its 48-bit WiFi MAC. `esp_now_add_peer()` pins the
  peer MAC and channel (`OPENDASH_ESPNOW_CHANNEL`). There are no bus addresses.
* **In code** a node is an `opendash_node_t` (`OPENDASH_NODE_CENTER`, `OPENDASH_NODE_LEFT`,
  `OPENDASH_NODE_RIGHT`, `OPENDASH_NODE_GPS`, `OPENDASH_NODE_BMS`,
  `OPENDASH_NODE_POD1`..`OPENDASH_NODE_POD8`, relay/MOS modules, ...).
  Center keeps the MAC-to-node mapping in its peer table; a slave learns
  Center's MAC from the first frame it receives.
* Discovery is **announce-driven**: a slave sends one `CHANNEL_MSG_ANNOUNCE` on
  boot. There is deliberately **no periodic PING and no polling** - nodes push
  on change, and absence of data is what signals a node is gone (see §4).

## 3. Priority channels

Four logical channels, each with its own queue and its own worker task on the
receiver, so a chatty low-priority producer cannot stall a critical one.

| Channel | Value | Dispatch interval | Retry limit | Task prio |
|---------|-------|-------------------|-------------|-----------|
| `CHANNEL_CRITICAL` | 0 | 10 ms | 3 | 5 |
| `CHANNEL_MEDIUM`   | 1 | 50 ms | 2 | 4 |
| `CHANNEL_LOW`      | 2 | 200 ms | 1 | 3 |
| `CHANNEL_CONTROL`  | 3 | 5 ms (command-driven) | 5 | 6 (highest) |

The dispatcher task runs at priority 4; channel task stack is 4096.

```c
#define CHANNEL_QUEUE_DEPTH            32    // inbound messages per channel
#define CHANNEL_QUEUE_ITEM_SIZE        192   // sized for a ~155 B batch packet
#define CHANNEL_MAX_DATA_POINTS        64    // tracked DPs for delta detection
#define CHANNEL_CMD_QUEUE_DEPTH        16    // outbound command queue (CH3)
#define CHANNEL_RETRY_BASE_MS          5     // backoff: 5, 10, 20 ms ...
#define CHANNEL_FORCE_SEND_DEADLINE_MS 150   // force-send outlasts a full TX queue
```

* `CHANNEL_*_INTERVAL_MS` is the **maximum latency** between a frame arriving
  and the app processing it. It is not a poll interval.
* Backoff is exponential from `CHANNEL_RETRY_BASE_MS`. `CHANNEL_CONTROL` has no
  offline timeout - it is command-driven.
* A send to a node marked `online == false` is forced to `max_retries = 0`, so
  a dead peer can never block a worker task.
* `channel_mgr_send_to_node()` routes automatically via `opcode_to_channel()`.
  Boost and parachute config/live-data opcodes route to `CHANNEL_CRITICAL`, so
  an operator command can never queue behind telemetry backlog.
* `CHANNEL_MSG_*` envelope tags (distinct from `OPENDASH_CMD_*` opcodes):
  `0x01 DATA_POINT`, `0x02 STATUS_REPORT`, `0x03 RELAY_CMD`, `0x04 SYSTEM_CMD`,
  `0x05 CONFIG`, `0x06 ANNOUNCE`, `0x07 BATCH_DP`.

## 4. Node health (no heartbeats)

`node_health.h` is the primary online/offline detector. It is rate-based, not
heartbeat-based:

```c
#define NODE_HEALTH_WINDOW_MS        1000   // measurement window
#define NODE_HEALTH_OFFLINE_WINDOWS  3      // 3 missed windows = ~3 s silence
#define NODE_HEALTH_DEGRADED_RATIO   0.25f  // < 25% of expected rate = DEGRADED
#define NODE_HEALTH_MISSED_RATIO     0.05f  // < 5% in a window = missed
#define NODE_HEALTH_ONLINE_WINDOWS   2      // good windows to re-upgrade

## 5. Opcode map

Defined in `common/include/opendash_protocol.h`. High bit set = Slave to Master.
Struct payloads live in `opendash_boost.h` and `opendash_parachute.h`.

### Master (Center) to Slave

| Opcode | Name | Payload |
|--------|------|---------|
| `0x01` | `SET_DATA_POINT` | `[dp_id:2][value:4]` |
| `0x02` | `SET_SCREEN_LAYOUT` | `[section:1][dp_id:2]...` |
| `0x03` | `SET_ALARM` | `[dp_id:2][lo:4][hi:4][flags:1]` |
| `0x04` | `SET_BRIGHTNESS` | `[level:1]` (0-255) |
| `0x05` | `CHECKLIST_UPDATE` | `[item_id:1][status:1]` |
| `0x06` | `REQUEST_DATA` | `[dp_id:2]` |
| `0x07` | `SYSTEM` | `[subcmd:1][params...]` - see section 6 |
| `0x08` | `SET_RELAY` | `[channel:1][state:1][pwm_duty:1]` |
| `0x09` | `REQUEST_RELAY_STATUS` | none; slave replies `RELAY_STATUS` |
| `0x0A` | `OBD_COMMAND` | `[obd_cmd:1]` (`0x43` clear DTCs, `0x56` request VIN) |
| `0x0B` | `AUDIO_ALERT` | `[sound_id:1][priority:1][duration_ms:2]` |
| `0x0C` | `SET_DATA_BATCH` | same layout as `DATA_BATCH` |
| `0x20` | `BOOST_LIVE_DATA` | `opendash_boost_live_t` (>=10 Hz) |
| `0x21` | `BOOST_SET_PARAMS` | `opendash_boost_params_t` |
| `0x22` | `BOOST_SET_MODE` | `[mode:1]` |
| `0x23` | `BOOST_SET_DUTY_ROW` | `opendash_boost_duty_row_t` |
| `0x24` | `BOOST_SET_SETP_ROW` | `opendash_boost_setpoint_row_t` |
| `0x25` | `BOOST_SET_THROTTLE` | `opendash_boost_throttle_curve_t` |
| `0x26` | `BOOST_PULL_ALL` | none; slave dumps full config |
| `0x27` | `PARACHUTE_SET_CONFIG` | `opendash_parachute_config_t` |
| `0x28` | `PARACHUTE_SET_ARM` | `[armed:1]` (0=disarm, 1=arm) |
| `0x29` | `PARACHUTE_PULL_ALL` | none; requests STATUS echo |
| `0x2A` | `PARACHUTE_DEPLOY` | none; manual/vote fire request (interlocked) |
| `0x2B` | `PARACHUTE_CALIBRATE` | none; zero/cal roll to current resting angle |

### Slave to Master

| Opcode | Name | Payload |
|--------|------|---------|
| `0x81` | `DATA_RESPONSE` | `[dp_id:2][value:4][timestamp:4]` |
| `0x82` | `STATUS_REPORT` | `[node_id:1][flags:2]` - see section 7 |
| `0x83` | `CHECKLIST_STATUS` | `[item_id:1][status:1]` |
| `0x84` | `ALARM_TRIGGERED` | `[dp_id:2][value:4]` |
| `0x85` | `RELAY_STATUS` | `[num_channels:1][ch0_state:1]...[chN_state:1]` |
| `0x86` | `DTC_REPORT` | `[count:1][code0:5]...` - 5 ASCII chars per code, max 16 |
| `0x88` | `DATA_BATCH` | `[count:1][dp_id:2][value:4] x count` (max 41) |
| `0x90` | `BOOST_TELEMETRY` | `opendash_boost_telemetry_t` (>=5 Hz) |
| `0x91` | `BOOST_PARAMS_REPORT` | `opendash_boost_params_t` (echo) |
| `0x92` | `BOOST_DUTY_REPORT` | `opendash_boost_duty_row_t` (echo) |
| `0x93` | `BOOST_SETP_REPORT` | `opendash_boost_setpoint_row_t` (echo) |
| `0x94` | `BOOST_THROTTLE_REPORT` | `opendash_boost_throttle_curve_t` (echo) |
| `0x95` | `PARACHUTE_STATUS` | `opendash_parachute_status_t` (echo) |
| `0x96` | `PARACHUTE_VOTE` | `opendash_parachute_vote_t` (rollover vote, broadcast) |
| `0xFF` | `NAK` | `[error_code:1]` |

`DATA_BATCH` / `SET_DATA_BATCH` are why the bus survives 50 Hz telemetry: one
packet carries a whole UART frame of DPs - max `(248-1)/6 = 41` entries - instead
of dozens of single-DP packets.

A MOS node **NVS-persists its own** boost and deployment config. Center pushes an
update and reads back the echo packet to verify it landed. ARM is a separate,
**non-persisted** toggle: a node always rebooted disarmed. `PARACHUTE_VOTE` is
broadcast, not unicast, because the rollover decision is a fleet vote.

## 6. SYSTEM sub-commands

Carried inside `OPENDASH_CMD_SYSTEM` (`0x07`) as its first payload byte.

| Sub-cmd | Value | Meaning |
|---------|-------|---------|
| `OPENDASH_SUBCMD_REBOOT` | `0x01` | Reboot the node |
| `OPENDASH_SUBCMD_OTA_START` | `0x02` | Prepare for OTA update |
| `OPENDASH_SUBCMD_FACTORY_RESET` | `0x03` | Reset all settings to defaults |
| `OPENDASH_SUBCMD_PING` | `0x04` | Ping / heartbeat (legacy - see §2; discovery no longer uses it) |
| `OPENDASH_SUBCMD_TIME_SYNC` | `0x05` | `[hour][min][sec][day][month][year_lo][year_hi][fix_valid]` |
| `OPENDASH_SUBCMD_ENTER_BT_OTA` | `0x06` | Tear down ESP-NOW, start BLE GATT OTA service |
| `OPENDASH_SUBCMD_SELF_TEST` | `0x07` | Run self-test (relay/MOS nodes: cycle all channels) |

`ENTER_BT_OTA` is why OTA works with no AP: the node drops ESP-NOW, advertises
GATT, and the phone (not a server) pushes the image over BLE. See
`wiki/ota-bluetooth.md`.

## 7. STATUS_REPORT flags

Slaves OR these bits into `status_payload[1]` before sending; Center reads them
to drive the Device Management UI - e.g. show "OTA-MODE" instead of "ONLINE".

| Flag | Value | Meaning |
|------|-------|---------|
| `OPENDASH_STATUS_FLAG_RUNNING` | `0x01` | Node is alive and processing |
| `OPENDASH_STATUS_FLAG_ERROR` | `0x02` | Node reports an internal error |
| `OPENDASH_STATUS_FLAG_BLE_OTA` | `0x04` | Node is entering / in BLE OTA mode |

## 8. Rules when changing this protocol

1. **Add a DP, not a new opcode**, if the data is just another value. Add a
   `DP_ID_*` in `opendash_data_points.h` and route it through
   `opendash_disp_submit()` so a pod that is offline at send time gets it when
   it reconnects.
2. **Add a new opcode** only for a new *kind* of exchange. Register it in
   `opcode_to_channel()` in `channel_management.c` or it will default-route and
   a command will sit behind telemetry backlog.
3. Never widen `CHANNEL_QUEUE_ITEM_SIZE` without checking it still covers the
   largest serialized packet; too large starves internal RAM and ESP-NOW task
   creation fails.
4. Multi-byte values are little-endian and travel as raw `float`/`uint` bytes.
   There is no TLV layer and no compression - the frame is 252 bytes and the
   packet is exactly one frame.
5. Never reintroduce polling or a wired inter-node bus. If a field must appear
   on a display, it is pushed on change.
