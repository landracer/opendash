<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# Boost Controller — OpenDash Implementation Instructions

> **Audience:** advanced ESP-IDF / LVGL devs working in [opendash](./readme.md).
> **Scope:** turn the staged boost-controller scaffolding into a shipping subsystem
> with on-dash map authoring, mode switching, OTA distribution, and runtime PID
> control on a MOS relay node. Replaces the legacy "program from a desktop app"
> workflow (MultiDisplay's `N75OptionsDialog`) with a self-contained
> dash-driven flow.
> **Heritage:** PID/maps mathematics ported from MultiDisplay's
> `RPMBoostController` (Stephan Martin / Dominik Gummel — GPL-3.0).
> See [`boostcontrol-staging/README.md`](./boostcontrol-staging/README.md).

---

## 0. Ground Rules — Do Not Break OpenDash

These rules are **non-negotiable**. PRs violating them will be rejected.

1. **Do not modify any file under `boostcontrol-staging/`.** It is the frozen
   peer-review baseline. Compare against it.
2. **Do not delete or break existing ESP-NOW opcodes** in
   [`common/include/opendash_i2c_protocol.h`](./common/include/opendash_i2c_protocol.h).
   Append new ones, never renumber existing ones.
3. **Boost transport rides ESP-NOW only.** Do not bring back I2C/UART for this
   subsystem (radio is the system bus — see [`readme.md`](./readme.md) §
   Communication).
4. **MOS-A boot defaults must keep current behavior** for non-boost channels.
   Boost mode is opt-in per-node, off by default.
5. **Center must remain functional with the boost slave missing or stale.**
   Treat boost as a soft-attached peripheral.
6. **Fail-safe wins every tie.** Any stale data, lean AFR, low fuel pressure,
   or mode==OFF must collapse PWM duty to `0` (wastegate fully open). See
   [`common/src/opendash_boost.c`](./common/src/opendash_boost.c) — the
   `cut:` label and `OPENDASH_BOOST_SAFE_*` flags are already correct; do not
   loosen them.
7. **Every code change must build clean for all 5 nodes** (`center`, `left`,
   `right`, `gps`, `mos-4ch-a`) before merge.
8. **PID-on-MOS only.** The center display never closes the boost loop — it
   only authors maps and forwards live data. The loop closes on the relay
   node so that if center reboots, boost still behaves.

---

## 1. Where We Are Today

| Layer | File | Status |
|---|---|---|
| Shared header — slave API | [`common/include/opendash_boost.h`](./common/include/opendash_boost.h) | ✅ Complete: params, duty rows, setpoint rows, throttle curve, live frame, telemetry, safety flags |
| Shared impl — PID + safety + NVS | [`common/src/opendash_boost.c`](./common/src/opendash_boost.c) | ✅ Complete: dual-tuning PID, RPM interp, throttle reduction, overboost/EGT/AFR/fuel cuts, per-row NVS persistence |
| ESP-NOW opcode catalogue | [`common/include/opendash_i2c_protocol.h`](./common/include/opendash_i2c_protocol.h) | ❌ **No boost opcodes yet** (only DP/Layout/Alarm/Relay/etc.) |
| MOS-A application | [`mos-4ch-a/main/main.c`](./mos-4ch-a/main/main.c) | ❌ No call to `opendash_boost_init()`, no ESP-NOW dispatch for boost frames, no PWM-out task, no live-data sink |
| Center ESP-NOW master | [`center/main/espnow_master.c`](./center/main/espnow_master.c) | ❌ No boost push helpers, no live-data fanout to MOS-A |
| Center UI — System Config | `center/main/ui_manager.c` | ❌ No System Config screen at all — must be added |
| Boost authoring UI (maps, modes, OTA) | n/a | ❌ Must be built |

The staging C surface (header + impl) is the spec. **Build the wire and UI to
match it. Do not refactor the staged surface unless a defect is found — and
then update [`boostcontrol-staging/`](./boostcontrol-staging/) in the same PR
so the baseline stays honest.**

---

## 2. Target Architecture

```
┌───────────────────────────────────────────────────────────────────────────┐
│  CENTER (ESP-NOW master, LVGL UI)                                         │
│  ───────────────────────────────                                           │
│  • System Config screen → "Boost Control" box                              │
│      ├─ Target node:        [MOS-A | MOS-B]                                │
│      ├─ Output channel:     [CH0..CH3]                                     │
│      ├─ Active mode:        [OFF | LOW | MED | HIGH]   (radio)             │
│      ├─ Map editor:         per-mode × per-gear duty + setpoint curves     │
│      ├─ Safety limits:      overboost / EGT warn+crit / AFR lean / fuel    │
│      ├─ PID gains:          aKp/aKi/aKd, cKp/cKi/cKd + thresholds          │
│      ├─ [Sync to Node]      push full config over ESP-NOW                  │
│      ├─ [Pull from Node]    read back what slave actually stores           │
│      └─ [BLE OTA]           hand off node to opendash_bt_ota for firmware  │
│  • Live data forwarder (≥10 Hz):                                           │
│      RPM, MAP/boost, EGT, AFR, fuel_psi, throttle%, gear                   │
│      → OPENDASH_CMD_BOOST_LIVE_DATA → MOS-A                                │
└──────────────────────┬────────────────────────────────────────────────────┘
                       │ ESP-NOW (WiFi p2p)
                       ▼
┌───────────────────────────────────────────────────────────────────────────┐
│  MOS-4CH-A / MOS-4CH-B (relay node, headless)                              │
│  ───────────────────────────────────────────                                │
│  • opendash_boost_init() at boot                                           │
│  • dispatch_boost(msg)   ← new switch arm in main.c                        │
│      handles: GET/SET params, GET/SET duty row, GET/SET setpoint row,      │
│               GET/SET throttle curve, LIVE_DATA, MODE_SET, TELEMETRY_REQ   │
│  • boost_task @ 50 Hz:                                                     │
│      opendash_boost_compute(&telem) → opendash_relay_set_pwm(ch, duty)     │
│  • boost_telem_task @ 5 Hz:                                                │
│      opendash_boost_get_telemetry(&t) → OPENDASH_CMD_BOOST_TELEMETRY       │
│      (unicast to center, never broadcast — no storm risk)                  │
│  • NVS-backed (namespace "boost") — survives reboot                        │
│  • Existing relay commands keep working — boost only owns the configured   │
│    output_channel; other channels remain user-controlled                   │
└───────────────────────────────────────────────────────────────────────────┘
```

Three "boost levels" the user asked for (LOW / MED / HIGH) map onto the
staged 2-slot file (NORMAL=0, RACE=1) **plus** the `OPENDASH_BOOST_MODE_OFF`
state. Implement the third map slot as a small extension (see §3.0).

---

## 3. Wire Format — Define Once, Use Everywhere

### 3.0 Extend the staged surface from 2 → 3 map slots

The user requirement is **LOW / MED / HIGH**. The staged header defines
`OPENDASH_BOOST_MODES = 2` with slots `NORMAL`/`RACE`. Apply this exact
diff (and mirror it in [`boostcontrol-staging/boost_control.h`](./boostcontrol-staging/boost_control.h)
so the baseline still matches):

```c
/* opendash_boost.h */
#define OPENDASH_BOOST_GEARS        6
#define OPENDASH_BOOST_MAP_POINTS  16
#define OPENDASH_BOOST_MODES        3      /* was 2 */

typedef enum {
    OPENDASH_BOOST_MODE_OFF  = 0,
    OPENDASH_BOOST_MODE_LOW  = 1,
    OPENDASH_BOOST_MODE_MED  = 2,
    OPENDASH_BOOST_MODE_HIGH = 3,
} opendash_boost_mode_t;

#define OPENDASH_BOOST_SLOT_LOW   0
#define OPENDASH_BOOST_SLOT_MED   1
#define OPENDASH_BOOST_SLOT_HIGH  2
```

Update `opendash_boost_default_duty_row()` and
`opendash_boost_default_setpoint_row()` to produce three distinct curves
(LOW = current NORMAL, HIGH = current RACE, MED = arithmetic mean). Bump
`OPENDASH_BOOST_PARAMS_VERSION` from `1` → `2` so existing NVS blobs are
invalidated cleanly. The slave already discards mismatched versions in
`nvs_load_all()` — no migration code needed.

### 3.1 New ESP-NOW opcodes

Append to [`common/include/opendash_i2c_protocol.h`](./common/include/opendash_i2c_protocol.h)
**at the end of the existing Master→Slave / Slave→Master blocks** — do not
re-use any existing ID. Reserve a contiguous range:

```c
/* ── Boost Controller — Master → Slave ───────────────────────────────── */
#define OPENDASH_CMD_BOOST_SET_PARAMS        0x20  /* opendash_boost_params_t */
#define OPENDASH_CMD_BOOST_GET_PARAMS        0x21  /* (no payload) */
#define OPENDASH_CMD_BOOST_SET_DUTY_ROW      0x22  /* opendash_boost_duty_row_t */
#define OPENDASH_CMD_BOOST_GET_DUTY_ROW      0x23  /* [mode:1][gear:1] */
#define OPENDASH_CMD_BOOST_SET_SETP_ROW      0x24  /* opendash_boost_setpoint_row_t */
#define OPENDASH_CMD_BOOST_GET_SETP_ROW      0x25  /* [mode:1][gear:1] */
#define OPENDASH_CMD_BOOST_SET_THROTTLE      0x26  /* opendash_boost_throttle_curve_t */
#define OPENDASH_CMD_BOOST_GET_THROTTLE      0x27  /* (no payload) */
#define OPENDASH_CMD_BOOST_LIVE_DATA         0x28  /* opendash_boost_live_t */
#define OPENDASH_CMD_BOOST_MODE_SET          0x29  /* [mode:1] (opendash_boost_mode_t) */
#define OPENDASH_CMD_BOOST_TELEMETRY_REQ     0x2A  /* (no payload) */

/* ── Boost Controller — Slave → Master ───────────────────────────────── */
#define OPENDASH_CMD_BOOST_PARAMS_REPORT     0x90  /* opendash_boost_params_t */
#define OPENDASH_CMD_BOOST_DUTY_REPORT       0x91  /* opendash_boost_duty_row_t */
#define OPENDASH_CMD_BOOST_SETP_REPORT       0x92  /* opendash_boost_setpoint_row_t */
#define OPENDASH_CMD_BOOST_THROTTLE_REPORT   0x93  /* opendash_boost_throttle_curve_t */
#define OPENDASH_CMD_BOOST_TELEMETRY         0x94  /* opendash_boost_telemetry_t */
```

**All payloads are the exact `__attribute__((packed))` structs already
declared in `opendash_boost.h`.** Do not reinvent the wire layout — copy the
struct directly into `msg.payload`. Endianness is little-endian on both ends.

### 3.2 Sync sequencing (center → slave)

Full config sync is **one params frame + (3 modes × 6 gears) × 2 row types +
one throttle frame = 38 frames.** Send strictly sequentially with a ≥20 ms
inter-frame gap to avoid ESP-NOW queue starvation. After the last frame,
fire `OPENDASH_CMD_BOOST_TELEMETRY_REQ` and wait for
`OPENDASH_CMD_BOOST_TELEMETRY` (≤300 ms timeout) as a sync ACK.

For **incremental edits** (single row changed in the editor), send only the
changed `SET_DUTY_ROW` or `SET_SETP_ROW` frame. The slave persists per-row,
so this is cheap.

For **mode toggle from the dash**, send only `OPENDASH_CMD_BOOST_MODE_SET`
(2 bytes total) — do not push the whole params blob.

---

## 4. MOS-4CH-A Integration

All work in [`mos-4ch-a/main/main.c`](./mos-4ch-a/main/main.c). MOS-B is a
verbatim copy — apply the same changes there once MOS-A is signed off.

### 4.1 Add to includes & boot

```c
#include "opendash_boost.h"
/* ... existing includes ... */

void app_main(void)
{
    /* ... existing nvs_flash_init, relay init, espnow init ... */
    ESP_ERROR_CHECK(opendash_boost_init());        /* loads/installs maps */
    xTaskCreatePinnedToCore(boost_compute_task, "boost_pid",  4096, NULL, 6, NULL, 1);
    xTaskCreatePinnedToCore(boost_telem_task,   "boost_telem", 3072, NULL, 4, NULL, 0);
    /* ... existing tasks ... */
}
```

### 4.2 Compute task — 50 Hz hard cadence

```c
static void boost_compute_task(void *arg)
{
    const TickType_t period = pdMS_TO_TICKS(20);   /* 50 Hz */
    TickType_t last = xTaskGetTickCount();
    opendash_boost_params_t p;
    for (;;) {
        opendash_boost_telemetry_t t;
        uint8_t duty = opendash_boost_compute(&t);
        opendash_boost_get_params(&p);
        /* ONLY drive the channel the user assigned. Other channels remain
           under normal relay control — DO NOT touch them here. */
        opendash_relay_set_pwm(p.output_channel, duty);
        vTaskDelayUntil(&last, period);
    }
}
```

> **Critical:** `opendash_relay_set_pwm()` must already be configured for
> ≥1 kHz on the chosen channel (the existing `mos_config.pwm_freq_hz = 1000`
> in main.c is fine for N75 solenoids — they are happy at 30 Hz–2 kHz).
> If a user assigns the boost channel to a slot a regular relay command
> later targets, the boost task will overwrite it on the next tick. That is
> intentional — boost owns the channel exclusively while mode != OFF.

### 4.3 Telemetry task — 5 Hz unicast to center

```c
static void boost_telem_task(void *arg)
{
    const TickType_t period = pdMS_TO_TICKS(200);  /* 5 Hz */
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        if (s_center_mac_known) {
            opendash_boost_telemetry_t t;
            opendash_boost_get_telemetry(&t);
            opendash_i2c_msg_t m;
            opendash_i2c_build_msg(&m, OPENDASH_CMD_BOOST_TELEMETRY,
                                   (uint8_t *)&t, sizeof(t));
            uint8_t buf[OPENDASH_ESPNOW_MAX_DATA]; uint16_t len = 0;
            if (opendash_i2c_serialize(&m, buf, &len) == OPENDASH_OK) {
                opendash_espnow_send(s_center_mac, buf, len);
            }
        }
        vTaskDelayUntil(&last, period);
    }
}
```

### 4.4 Dispatch handlers — extend `dispatch_message()`

Add the boost arm **after** the existing `OPENDASH_CMD_SET_RELAY` /
`OPENDASH_CMD_REQUEST_RELAY_STATUS` / `OPENDASH_CMD_SYSTEM` cases:

```c
case OPENDASH_CMD_BOOST_SET_PARAMS:
    if (msg->length == sizeof(opendash_boost_params_t)) {
        opendash_boost_set_params((const opendash_boost_params_t *)msg->payload);
    } break;

case OPENDASH_CMD_BOOST_GET_PARAMS: {
    opendash_boost_params_t p; opendash_boost_get_params(&p);
    reply(evt, OPENDASH_CMD_BOOST_PARAMS_REPORT, &p, sizeof(p));
} break;

case OPENDASH_CMD_BOOST_SET_DUTY_ROW:
    if (msg->length == sizeof(opendash_boost_duty_row_t)) {
        const opendash_boost_duty_row_t *r = (const void *)msg->payload;
        opendash_boost_set_duty_row(r->mode, r->gear, r->duty);
    } break;

case OPENDASH_CMD_BOOST_GET_DUTY_ROW:
    if (msg->length == 2) {
        opendash_boost_duty_row_t r = { .mode = msg->payload[0], .gear = msg->payload[1] };
        if (opendash_boost_get_duty_row(r.mode, r.gear, r.duty) == ESP_OK)
            reply(evt, OPENDASH_CMD_BOOST_DUTY_REPORT, &r, sizeof(r));
    } break;

/* Same shape for SET/GET_SETP_ROW, SET/GET_THROTTLE. */

case OPENDASH_CMD_BOOST_LIVE_DATA:
    if (msg->length == sizeof(opendash_boost_live_t))
        opendash_boost_feed_live((const opendash_boost_live_t *)msg->payload);
    break;

case OPENDASH_CMD_BOOST_MODE_SET:
    if (msg->length == 1) {
        opendash_boost_params_t p; opendash_boost_get_params(&p);
        p.mode = msg->payload[0];
        opendash_boost_set_params(&p);   /* persists to NVS automatically */
    } break;

case OPENDASH_CMD_BOOST_TELEMETRY_REQ: {
    opendash_boost_telemetry_t t; opendash_boost_get_telemetry(&t);
    reply(evt, OPENDASH_CMD_BOOST_TELEMETRY, &t, sizeof(t));
} break;
```

Factor the existing unicast-to-center pattern (build → serialize → send) into
a static `static void reply(const opendash_espnow_event_t *evt, uint8_t cmd, const void *p, size_t n)`
helper so dispatch stays readable. **Do not regress the existing
`SET_RELAY` / `STATUS` flow.**

### 4.5 OTA on demand

OTA is already implemented in
[`common/include/opendash_bt_ota.h`](./common/include/opendash_bt_ota.h).
The existing `OPENDASH_SUBCMD_ENTER_BT_OTA` path stops ESP-NOW and calls
`opendash_bt_ota_start(OPENDASH_NODE_MOS_4CH_A)` — **reuse it as-is**. The
System Config UI just sends `OPENDASH_CMD_SYSTEM` + that subcmd; nothing
boost-specific needs to be added to the OTA layer.

---

## 5. Center Application Integration

### 5.1 Live-data forwarder

> **Data model reality check.** Center today has **no global value cache /
> getter** in `opendash_data_model.h` — values flow as one-shot pushes from
> `espnow_master.c` into `ui_manager_update_value()` and out to slaves via
> `push_data_point()` (see `center/main/espnow_master.c` ~lines 580–629).
> You must add a tiny snapshot cache before the forwarder can be written.

**Step A — add a snapshot cache in `espnow_master.c`** (a static struct, not a
new public API; keep the scope tight):

```c
/* Single-writer (ESP-NOW RX task) / multi-reader cache, plain volatile is
   fine for individual float reads on Xtensa; no mutex needed. */
static struct {
    volatile float rpm;          /* OPENDASH_DP_RPM         */
    volatile float boost_kpa;    /* OPENDASH_DP_BOOST_PRESSURE (gauge kPa) */
    volatile float egt_c;        /* OPENDASH_DP_EGT (already max-of-all)   */
    volatile float afr;          /* OPENDASH_DP_AFR         */
    volatile float fuel_kpa;     /* OPENDASH_DP_FUEL_PRESSURE              */
    volatile float throttle_pct; /* OPENDASH_DP_THROTTLE_POS               */
    volatile float gear;         /* see Step C — gear DP does not exist yet */
    volatile bool  any_received;
} s_dp_cache;
```

Hook the cache writes into the **existing** call sites that already update the
UI / slaves. Wherever you see `ui_manager_update_value(OPENDASH_DP_RPM, rpm)`
etc. in `espnow_master.c`, add the matching `s_dp_cache.rpm = rpm;`. Do **not**
create a parallel ingest path.

**Step B — the 10 Hz forwarder task** (uses the real DP names and the cache):

```c
static void boost_live_push_task(void *arg)
{
    const TickType_t period = pdMS_TO_TICKS(100);
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        /* Gauge boost in cBar = kPa gauge × 1 (1 BAR = 100 kPa = 100 cBar).
           OPENDASH_DP_BOOST_PRESSURE is gauge kPa — see espnow_master.c
           comment "0→200 kPa". If a future sensor publishes absolute kPa,
           subtract 100 here. */
        opendash_boost_live_t live = {
            .rpm            = (uint16_t) s_dp_cache.rpm,
            .boost_cbar     = (int16_t)  s_dp_cache.boost_kpa,
            .egt_c          = (int16_t)  s_dp_cache.egt_c,
            .afr_x10        = (uint16_t) (s_dp_cache.afr * 10.0f),
            .fuel_press_kpa = (uint16_t) s_dp_cache.fuel_kpa,
            .throttle_pct   = (uint8_t)  s_dp_cache.throttle_pct,
            .gear           = (uint8_t)  s_dp_cache.gear,
        };
        if (s_dp_cache.any_received && g_boost_target_node != OPENDASH_NODE_INVALID) {
            espnow_master_send_boost_live(g_boost_target_node, &live);
        }
        vTaskDelayUntil(&last, period);
    }
}
```

If a sensor is missing (`any_received == false` or the cached float is `0` and
never populated), do not forward — the slave's `OPENDASH_BOOST_DATA_TIMEOUT_MS`
will expire and trigger `SAFE_DATA_STALE` → duty 0. That is the desired
fail-safe.

**Step C — gear data point** (currently missing from
[`opendash_data_model.h`](./common/include/opendash_data_model.h) and
[`opendash_dp_catalog.h`](./common/include/opendash_dp_catalog.h)). Add it in
the same PR:

```c
/* opendash_data_model.h — Engine / OBD2 block (0x0100–0x01FF) */
#define OPENDASH_DP_GEAR             0x011C  /**< Current gear (1..N, 0 = unknown/neutral) */
```

Add a matching `opendash_dp_catalog[]` row (category `DRIVETRAIN`, units `"gear"`,
min 0 / max 8, decimals 0). Until a real gear source (CAN/calculation) exists,
the cache stays at `0`, the slave treats it as "use gear-row 0", and the
Boost box shows the yellow "Gear unknown" warning from §6.

**Step D — owner of `g_boost_target_node`.** Declare in `system_config.h`:

```c
extern opendash_node_t g_boost_target_node;  /* OPENDASH_NODE_MOS_4CH_A by default */
```

Persist its value in NVS namespace `"boost_ui"` key `"target"`. Set to
`OPENDASH_NODE_INVALID` to disable the forwarder entirely (e.g. car has no
boost solenoid wired).

Add a thin sender to `espnow_master.h`/`.c`:

```c
esp_err_t espnow_master_send_boost_live(opendash_node_t node,
                                        const opendash_boost_live_t *live);
esp_err_t espnow_master_boost_send_params(opendash_node_t node,
                                          const opendash_boost_params_t *p);
esp_err_t espnow_master_boost_send_duty_row(opendash_node_t node, uint8_t mode,
                                            uint8_t gear, const uint8_t *row);
esp_err_t espnow_master_boost_send_setp_row(opendash_node_t node, uint8_t mode,
                                            uint8_t gear, const uint16_t *row);
esp_err_t espnow_master_boost_set_mode(opendash_node_t node,
                                       opendash_boost_mode_t mode);
esp_err_t espnow_master_boost_request_pull(opendash_node_t node);  /* GET_PARAMS+rows */
```

Each is a 5-line wrapper around `opendash_i2c_build_msg` →
`opendash_i2c_serialize` → `opendash_espnow_send` using the IDs from §3.1.

### 5.2 Inbound telemetry — cache for UI

Inside the existing ESP-NOW RX handler in `espnow_master.c` (look for the
`switch (msg->cmd)` near line 334 that already handles `STATUS_REPORT` /
`DATA_RESPONSE` / `NAK`), add new cases:

```c
case OPENDASH_CMD_BOOST_TELEMETRY:
    if (msg.length == sizeof(opendash_boost_telemetry_t)) {
        xSemaphoreTake(g_boost_lock, portMAX_DELAY);
        memcpy(&g_boost_last_telem, msg.payload, sizeof(g_boost_last_telem));
        g_boost_last_telem_ms = pdTICKS_TO_MS(xTaskGetTickCount());
        xSemaphoreGive(g_boost_lock);
    }
    break;
case OPENDASH_CMD_BOOST_PARAMS_REPORT:
case OPENDASH_CMD_BOOST_DUTY_REPORT:
case OPENDASH_CMD_BOOST_SETP_REPORT:
case OPENDASH_CMD_BOOST_THROTTLE_REPORT:
    boost_ui_on_pull_frame(msg.cmd, msg.payload, msg.length);  /* defined in boost_config_ui.c */
    break;
```

`g_boost_last_telem` is a `static opendash_boost_telemetry_t` guarded by a
FreeRTOS mutex `g_boost_lock` (created once in `espnow_master_init`). The UI
reads it from the LVGL task via a thin `boost_ui_get_last_telem(&out)`
accessor. **Never call LVGL APIs from the ESP-NOW RX context** — the existing
code batches UI updates onto the LVGL task via the `s_pending_ui` queue
(`espnow_master.c` ~line 300); use the same pattern for any UI refresh you
need off telemetry.

### 5.3 NVS on center

The center stores its **own** copy of the maps under NVS namespace
`"boost_ui"`. The slave is the source of truth at runtime, but the UI cache
lets the user open the editor while the slave is offline and push later
with `[Sync to Node]`. Keys mirror the slave layout (`d_<mode>_<gear>`,
`s_<mode>_<gear>`, `params`, `throt`).

---

## 6. UI — System Config Screen with the Boost Box

The center has no System Config screen yet. Create one and wire it into the
existing screen-switch flow in
[`center/main/ui_manager.c`](./center/main/ui_manager.c). Long-press on the
touch screen (≥1 s) opens System Config — keep the gesture aligned with
whatever pattern the team adopts in the per-node config feature
(see [`PER_NODE_DISPLAY_CONFIG_SPEC.md`](./PER_NODE_DISPLAY_CONFIG_SPEC.md)).

### 6.1 New files

```
center/main/
  system_config.c / .h          ← top-level config router + screen
  boost_config_ui.c / .h        ← the "Boost Control" box + sub-screens
```

Hook them into `center/main/CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS "main.c" "display_init.c" "espnow_master.c" "ui_manager.c"
         "sd_logger.c" "layout_editor.c"
         "system_config.c" "boost_config_ui.c"      # NEW
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_event driver lvgl__lvgl
             opendash_common
)
```

### 6.2 Boost box layout (LVGL 9)

The box on the System Config screen is a tile with:

```
┌──────────────────────────────────────────┐
│  BOOST CONTROL                  [enable] │
│  Target node:  [ MOS-A ▾ ]               │
│  Output ch:    [ CH0   ▾ ]               │
│  Active mode:  ( ) OFF (●) LOW           │
│                ( ) MED  ( ) HIGH         │
│  Status:  PID=ON  Aggr=OFF  Safety=----  │
│  Boost:   1.18 BAR / Set 1.20 BAR        │
│  Duty:    142 / 255                      │
│                                          │
│  [ Map Editor ]  [ PID Tuning ]          │
│  [ Safety ]      [ Sync ▼ ]              │
│  [ BLE OTA → MOS-A ]                     │
└──────────────────────────────────────────┘
```

The **Sync** dropdown holds `Push to Node`, `Pull from Node`, `Restore
Defaults`. The status row is driven by `g_boost_last_telem` — refresh on a
200 ms LVGL timer.

### 6.3 Map Editor sub-screen

Three tabs: **LOW / MED / HIGH**. Inside each tab, a gear selector
(buttons 1–6) and **two stacked plots**:

- **Top:** Duty row — 16 points × Y 0..255, draggable.
- **Bottom:** Setpoint row — 16 points × Y 0..250 cBar, draggable.

Use `lv_chart` with `LV_CHART_TYPE_LINE`, `LV_CHART_UPDATE_MODE_SHIFT` off,
and add a custom press-and-drag handler that mutates the active point. RPM
axis labels are computed from `OPENDASH_BOOST_RPM_MAX` / 15:
`0, 533, 1066, 1600, ... 8000 RPM`.

A `[Save Row]` button on the sub-screen calls
`espnow_master_boost_send_duty_row(...)` / `..._setp_row(...)` for the
currently-edited row only. Do not auto-push on every drag — that floods
ESP-NOW.

### 6.4 PID & Safety sub-screens

Plain form: each field is a `lv_spinbox`. On save, the screen builds an
`opendash_boost_params_t` (read current params first, mutate only the
on-screen fields, push back) and calls
`espnow_master_boost_send_params(...)`. **Always read-modify-write** so
unrelated fields (e.g. `mode` from a quick-toggle) are preserved.

### 6.5 Mode toggle quick path

The four radio buttons in the Boost box bypass the full params push:
`espnow_master_boost_set_mode(g_boost_target_node, OPENDASH_BOOST_MODE_LOW)`.
Slave will persist via its own NVS write. UI flips the radio optimistically
and reconciles when the next telemetry frame arrives (`t.mode`).

### 6.6 OTA hand-off

The `[BLE OTA → MOS-A]` button:
1. Disables the boost mode (sends `MODE_SET = OFF`).
2. Sends `OPENDASH_CMD_SYSTEM` + `OPENDASH_SUBCMD_ENTER_BT_OTA` to the target.
3. Shows a modal with the Web Bluetooth URL/instructions
   (`OpenDash-MOS_4CH_A-OTA`, see
   [`common/include/opendash_bt_ota.h`](./common/include/opendash_bt_ota.h)).
4. After 30 s timeout or node reappearance, re-enables the previous mode.

---

## 7. Data Points the Boost System Consumes

Use the **exact** identifiers from
[`common/include/opendash_data_model.h`](./common/include/opendash_data_model.h).
Do not invent new prefixes:

| Identifier (existing) | Used as | Units in OpenDash | Conversion for live frame |
|---|---|---|---|
| `OPENDASH_DP_RPM` | engine RPM | rpm | none |
| `OPENDASH_DP_BOOST_PRESSURE` | feedback variable | **kPa gauge** | `cBar = kPa` (1:1) |
| `OPENDASH_DP_EGT` | safety pull (already max-of-all) | °C | none |
| `OPENDASH_DP_AFR` | lean-cut safety | ratio (e.g. 14.5) | `afr_x10 = afr × 10` |
| `OPENDASH_DP_FUEL_PRESSURE` | fuel-low safety | kPa | none |
| `OPENDASH_DP_THROTTLE_POS` | gating + throttle curve | 0..100 % | none |
| `OPENDASH_DP_GEAR` *(must be added — see §5.1 Step C)* | gear-row selection | 1..N (0 = unknown) | none |

There is no global getter for these in the current data model. The
forwarder reads from the small `s_dp_cache` introduced in §5.1.

For OBD2-only cars, gear is rarely published. The slave already treats
`gear == 0` as "use row 0" — acceptable, but the Boost box shows a yellow
"Gear unknown — using 1st-gear map" warning when `s_dp_cache.gear == 0`.

---

## 8. Build, Flash, Smoke-Test Order

1. **Header + opcode patch** — modify the staged header (mode count → 3),
   add opcodes to `opendash_i2c_protocol.h`, mirror to
   `boostcontrol-staging/boost_control.h`. Build `mos-4ch-a` and `center`.
   Expect zero call-site regressions because the new IDs are unused.
2. **MOS-A runtime** — add boost init, compute task, telem task, dispatch
   arms. Flash MOS-A. Confirm via serial monitor:
   - `boost: Loaded boost config from NVS` **OR** `installing defaults`
   - Periodic telemetry log (add a single `ESP_LOGD` print of `t.duty`).
   - With no live data fed, telemetry shows `safety_flags & SAFE_DATA_STALE`,
     `duty == 0`. **This is the critical fail-safe gate. Verify it on the
     bench before connecting any solenoid.**
3. **Center forwarder** — add `boost_live_push_task` and the
   `espnow_master_boost_*` helpers. Flash center. Confirm MOS-A telemetry
   now shows live RPM, no `SAFE_DATA_STALE` flag.
4. **System Config + Boost box** — UI in OFF mode only. Verify mode-set
   toggling persists across MOS-A reboot.
5. **Map editor** — author one duty row + one setpoint row, push, pull,
   verify they match.
6. **PID / Safety forms** — push, pull, verify.
7. **Bench dyno / pressurized rig** with a real N75 solenoid before any
   in-car testing. Verify overboost cut at the configured BAR limit by
   feeding a contrived `DP_BOOST_BAR` past the threshold (a debug DP
   injection helper is acceptable, gated behind `CONFIG_OPENDASH_DEBUG`).
8. **MOS-B** — copy the MOS-A changes verbatim (only the node ID differs).

---

## 9. Acceptance Criteria

A PR closes this task only when **every** item below holds:

- [ ] `boostcontrol-staging/` and `common/include/opendash_boost.h` agree on
      `OPENDASH_BOOST_MODES`, `_PARAMS_VERSION`, and the enum.
- [ ] All 5 node projects compile under ESP-IDF v6.1 with no new warnings.
- [ ] MOS-A boots with no boost config in NVS, installs defaults, persists,
      survives reboot.
- [ ] With center powered off, MOS-A reports `SAFE_DATA_STALE`, duty `0`.
- [ ] With center powered on and DPs populated, telemetry shows fresh
      `rpm`/`boost_cbar`/`gear` and a non-trivial duty when above gating.
- [ ] Map editor round-trips a row through Push → reboot MOS-A → Pull
      bit-exact.
- [ ] Mode quick-toggle from the Boost box reflects in the next telemetry
      frame (≤300 ms).
- [ ] Overboost, EGT-critical, AFR-lean, and fuel-low cuts all observable
      on the bench via debug DP injection.
- [ ] BLE-OTA flow from the Boost box correctly disables boost first, then
      hands off to `opendash_bt_ota_start()`.
- [ ] No existing `OPENDASH_CMD_*` opcode value changed.
- [ ] No existing relay control regressions on the non-boost channels.

---

## 10. Reference Material

- Staged baseline (do not edit):
  [`boostcontrol-staging/boost_control.h`](./boostcontrol-staging/boost_control.h),
  [`boostcontrol-staging/README.md`](./boostcontrol-staging/README.md).
- Shared boost API (the spec):
  [`common/include/opendash_boost.h`](./common/include/opendash_boost.h),
  [`common/src/opendash_boost.c`](./common/src/opendash_boost.c).
- ESP-NOW transport & frame format:
  [`common/include/opendash_espnow.h`](./common/include/opendash_espnow.h),
  [`common/include/opendash_i2c_protocol.h`](./common/include/opendash_i2c_protocol.h).
- Existing MOS node template:
  [`mos-4ch-a/main/main.c`](./mos-4ch-a/main/main.c).
- BLE OTA service (reuse, do not modify):
  [`common/include/opendash_bt_ota.h`](./common/include/opendash_bt_ota.h).
- Heritage — algorithm provenance (GPL-3.0, for reading only, do not
  copy code verbatim):
  `../../multidisplay-firmware/multidisplay/RPMBoostController.cpp`,
  `../../multidisplay-app/multidisplay-app/src/N75OptionsDialog.*`,
  `V2N75SetupDialog.*`.
- Project ground rules: [`readme.md`](./readme.md),
  [`PROJECT_INDEX.md`](./PROJECT_INDEX.md),
  [`TODO.md`](./TODO.md).

---

## 11. Review-Against-Code Notes (verified before merging this doc)

This section is the audit trail proving the spec above matches the live
codebase. Re-run these greps before you start implementing.

### 11.1 Verified APIs (names match exactly)

| Claim in this doc | File | Status |
|---|---|---|
| `opendash_espnow_send(mac, data, len)` / `_broadcast` / `_add_peer` | [common/include/opendash_espnow.h](./common/include/opendash_espnow.h) | OK exact |
| `OPENDASH_ESPNOW_MAX_DATA = 250`, channel 1 | same | OK exact |
| `opendash_espnow_event_t { src_mac[6], data[250], len, rssi }` | same | OK exact |
| `opendash_i2c_build_msg` / `_serialize` return `opendash_err_t` (success = `OPENDASH_OK`) | [common/include/opendash_i2c_protocol.h](./common/include/opendash_i2c_protocol.h) | OK exact — note: **not** `esp_err_t` |
| Existing CMDs 0x01–0x07 (M→S), 0x81–0x84 (S→M), 0xFF (NAK) | same | OK exact — new 0x20–0x2A / 0x90–0x94 ranges are free |
| `opendash_relay_set_pwm(channel, duty)` | [common/include/opendash_relay.h](./common/include/opendash_relay.h) | OK exact |
| `opendash_bt_ota_start(opendash_node_t)` triggered by `OPENDASH_SUBCMD_ENTER_BT_OTA` | [common/include/opendash_bt_ota.h](./common/include/opendash_bt_ota.h) | OK exact, reused as-is |
| `OPENDASH_NODE_MOS_4CH_A` / `_MOS_4CH_B` with `OTA` capability | [common/include/node_definitions.h](./common/include/node_definitions.h) | OK exact |
| `opendash_boost_*` runtime + per-row NVS persistence under namespace `"boost"` | [common/src/opendash_boost.c](./common/src/opendash_boost.c), [common/include/opendash_boost.h](./common/include/opendash_boost.h) | OK exact |
| MOS-A uses `s_center_mac` / `s_center_mac_known`, dispatches `OPENDASH_CMD_SET_RELAY` / `_REQUEST_RELAY_STATUS` / `_SYSTEM`, PWM on GPIO 16/17/26/27 @ 1 kHz | [mos-4ch-a/main/main.c](./mos-4ch-a/main/main.c) | OK exact |
| Center exposes `espnow_master_init/_start/_get_status/_send_data_point` and a `s_pending_ui[]` UI queue (lines ~300, ~580–629) | [center/main/espnow_master.h](./center/main/espnow_master.h), [center/main/espnow_master.c](./center/main/espnow_master.c) | OK exact |

### 11.2 Spec was wrong → fixed in this revision

1. **DP identifier prefix.** Earlier draft used `DP_*`. Real prefix is
   `OPENDASH_DP_*`. §5.1 and §7 now use the exact identifiers from
   [`opendash_data_model.h`](./common/include/opendash_data_model.h).
2. **Boost units.** `OPENDASH_DP_BOOST_PRESSURE` is **kPa gauge**, not BAR.
   Live frame conversion is `cBar = kPa` (1 BAR = 100 kPa = 100 cBar).
3. **No `data_model_get()` accessor exists.** Center pushes values directly
   into `ui_manager_update_value()` and `push_data_point()` at
   `center/main/espnow_master.c` lines ~580–629. §5.1 now adds a tiny
   `s_dp_cache` populated at those call sites — no parallel ingest.
4. **`OPENDASH_DP_GEAR` does not exist.** §5.1 Step C adds it to the data
   model and catalog in the same PR. Slave already handles `gear == 0`.
5. **`g_boost_target_node` ownership.** Declared in `system_config.h`, lives
   in NVS namespace `"boost_ui"` key `"target"`. §5.1 Step D.
6. **Concurrency contract.** §5.2 explicitly forbids LVGL calls from the
   ESP-NOW RX context and uses a mutex around `g_boost_last_telem`.
7. **`reply()` helper** is marked NEW (not pre-existing) — §4.4.

### 11.3 ESP-IDF version

[`readme.md`](./readme.md) prerequisites and the inline doxygen links inside
[`opendash_data_model.h`](./common/include/opendash_data_model.h) (~line 12)
both target **v5.3**. The opendash agent mode preamble says v6.1. Build
against whatever the repo's `idf.py --version` reports during local
verification. The acceptance checklist in §9 says "ESP-IDF v6.1" — adjust
to match the actual build host. The spec is version-neutral.

### 11.4 Still requires manual confirmation when implementing

These the implementer must verify on first read because they depend on
local state that can drift:

- `center/main/CMakeLists.txt` SRCS list — §6.1 adds two new files;
  confirm the current SRCS array shape (it may use
  `idf_component_register(SRCS ...)` or a wildcard) before patching.
- The 20 ms inter-frame gap in §3.2 is a conservative guess. Validate
  against the TX queue depth in
  [common/include/opendash_espnow.h](./common/include/opendash_espnow.h)
  and bump if you see TX-queue-full logs during the bulk push
  (params + 6×duty + 6×setp + throttle + activate ≈ 14 frames).
- Free opcode range — `grep -rn "0x[29][0-9A-F]" common/ center/ mos-4ch-a/`
  before merge to catch any newly-added collisions.
- `opendash_boost_params_t` packed size is 64 bytes (manually computed).
  Add a `_Static_assert(sizeof(opendash_boost_params_t) <= 248, ...)` next
  to the struct definition so the wire-format limit is compile-time
  enforced.

### 11.5 One-shot implementability — final answer

**Yes**, with this revision. The audit corrected every fabricated symbol,
nailed down the data-flow attach points, and added the missing
`OPENDASH_DP_GEAR` work item. The remaining items in §11.4 are 5–15 minute
confirmations, not unknowns. The plan is self-contained: slave runtime
already exists, BLE OTA already exists, ESP-NOW transport already exists,
dispatch pattern already exists. New code surface is roughly:

- `mos-4ch-a/main/main.c` — ~150 LOC (compute task + dispatch cases).
- `center/main/espnow_master.{h,c}` — ~250 LOC (cache, forwarder task,
  6 send helpers, 4 RX cases).
- `center/main/system_config.{h,c}` — new ~120 LOC.
- `center/main/boost_config_ui.{h,c}` — new ~600 LOC (System Config
  screen, Boost box, Map Editor, PID/Safety/OTA panes).
- `common/include/opendash_i2c_protocol.h` — 9 new `OPENDASH_CMD_BOOST_*`
  defines.
- `common/include/opendash_boost.h` — bump `_MODES` 2→3,
  `_PARAMS_VERSION` 1→2, add `_MODE_HIGH`.
- `common/include/opendash_data_model.h` + `opendash_dp_catalog.{h,c}` —
  add `OPENDASH_DP_GEAR`.

Total: ~1100 LOC of new code, zero refactors of existing modules.

---

## Appendix B — Safe Default Boost Values (firmware shipped)

These defaults are baked into [common/src/opendash_boost.c](common/src/opendash_boost.c)
and installed automatically on first boot (and any time NVS schema version
mismatches `OPENDASH_BOOST_PARAMS_VERSION`, currently **v2**).

| Mode    | Peak setpoint | Overboost cut | Notes                                       |
|---------|---------------|---------------|---------------------------------------------|
| NORMAL  | **14 psi** (0.97 BAR) | 2.80 BAR (~40 psi) | Daily-driving safe target           |
| RACE    | **36 psi** (2.48 BAR) | 2.80 BAR (~40 psi) | Aggressive; verify wastegate spring |
| HIGH    | shaped between NORMAL and RACE | 2.80 BAR | User-tunable per-gear                |

All three modes are seeded across the **full 32-point RPM range (0 → 16 000 RPM)**
so high-RPM engines have valid duty/setpoint curves out of the box.  Edit any
cell live in the Boost Controller config menu (Page 1 / Page 2 toggle).

The slave (MOS-A or MOS-B) regenerates these defaults on boot if NVS is empty
or `PARAMS_VERSION` was bumped.

---

## Appendix C — Flashing a MOS Boost Controller via BLE OTA

Both **MOS-4CH-A** (addr `0x10`) and **MOS-4CH-B** (addr `0x11`) ship with the
full OpenDash OTA stack: NimBLE GATT, dual-OTA partitions, and the
`ENTER_BT_OTA` (subcmd `0x06`) handler under `OPENDASH_CMD_SYSTEM`.

### 1. Build the slave binary

```bash
cd opendash/mos-4ch-a            # or mos-4ch-b
idf.py set-target esp32s3
idf.py build
# binary: build/mos-4ch-a.bin     (≈ 800 KB, fits in 1 MB OTA slot)
```

### 2. Put the target into OTA mode

From the Center display:

1. Open the **Config menu → Boost Controller → System**.
2. Tap **Enter BT-OTA** for the node you want to update.

This sends `OPENDASH_CMD_SYSTEM` / subcmd `OPENDASH_SUBCMD_ENTER_BT_OTA` over
ESP-NOW. The slave will:

- Drive PWM channel 3 (boost solenoid) to **0**.
- Open all relays via `opendash_relay_all_off()`.
- De-init ESP-NOW.
- Call `opendash_bt_ota_start(OPENDASH_NODE_MOS_4CH_A | _B)`.
- Reboot and come up advertising as **OpenDash-MOS-A** or **OpenDash-MOS-B**.

### 3. Push the binary

```bash
cd opendash
python3 ble_ota.py \
    --device-name OpenDash-MOS-A \
    --binary mos-4ch-a/build/mos-4ch-a.bin
```

(Use `OpenDash-MOS-B` + `mos-4ch-b/build/mos-4ch-b.bin` for the second node.)

`ble_ota.py` writes the new image into the inactive OTA slot, validates it,
flips `otadata`, and forces a reboot. ESP-NOW comes back automatically and the
node rejoins the bus.

### 4. Recovery — JTAG / FTDI fallback

The **factory partition** is never touched by OTA. If an OTA image bricks the
node:

```bash
cd opendash/mos-4ch-a
idf.py -p /dev/ttyUSB0 flash    # serial reflash via FTDI
```

This restores the factory image and clears `otadata`, allowing a clean OTA
retry.

### 5. Verifying OTA support is enabled

A node is OTA-capable iff **all** of the following are true:

- `partitions.csv` defines `ota_0`, `ota_1`, and `otadata` slots.
- `sdkconfig.defaults` enables `CONFIG_PARTITION_TABLE_CUSTOM=y`,
  `CONFIG_BT_ENABLED=y`, `CONFIG_BT_NIMBLE_ENABLED=y`, and
  `CONFIG_APP_PROJECT_VER_FROM_CONFIG=y`.
- `main/CMakeLists.txt` links `common/src/opendash_bt_ota.c` and `REQUIRES`
  `app_update nvs_flash bt`.
- `main/main.c` includes `opendash_bt_ota.h` and handles
  `OPENDASH_SUBCMD_ENTER_BT_OTA` in the `OPENDASH_CMD_SYSTEM` dispatch.

**Status as of this release:** center, left, right, gps, **mos-4ch-a**, and
**mos-4ch-b** all satisfy these requirements and can be flashed OTA from the
companion app or `ble_ota.py`.
