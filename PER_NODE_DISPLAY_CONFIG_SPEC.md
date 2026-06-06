<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# Per-Node Display Configuration — Implementation Spec

**Status:** Phase 1 integrated into `common/` + `center/` + all slaves; Phase 2 (UI editor + live apply) pending
**Owner of spec:** prepared for jr-dev hand-off
**Target version:** OpenDash v0.9.0
**Depends on:** v0.8.x batched ESP-NOW (already in main)

---

## 1. Goal

Let the end-user, from the CENTER touchscreen, pick which PIDs each node (LEFT, RIGHT, GPS, POD1, POD2, CENTER itself) shows on its display, **per display mode**, **per slot**, with point-and-click. Choices persist across reboots and survive OTA. CENTER pushes the chosen layout to the target node over ESP-NOW; the node updates its UI and saves its own copy to NVS.

---

## 2. Glossary

| Term | Meaning |
|---|---|
| **DP / data point** | A single sensor reading identified by a 16-bit `OPENDASH_DP_*` ID (e.g. `OPENDASH_DP_RPM`). |
| **Slot** | One on-screen widget position. The current center display has 6 text sections + 1 arc = **7 slots per mode**. Round gauge nodes (LEFT/RIGHT) have 1 arc + 4 corners = **5 slots per mode**. |
| **Mode** | A named screen layout the user can swipe between (`ENGINE`, `GPS`, `MD`, `OBD`, `BMS`, `RELAY`, `CONFIG`). |
| **Layout** | The full mapping `(node, mode) → slot[i] = dp_id` plus per-slot min/max for arcs. |
| **Catalog** | The static list of all assignable DPs with metadata (label, units, defaults, category). |

---

## 3. Current State (what already exists)

| Piece | Location | State |
|---|---|---|
| DP IDs | [common/include/opendash_i2c_protocol.h](common/include/opendash_i2c_protocol.h) | ✅ Defined and stable |
| `mode_dp_maps[]` | [center/main/ui_manager.c](center/main/ui_manager.c) ~L2460 | ⚠ `static const`, hard-coded, not editable at runtime |
| Slave layouts | each slave's `main.c` / `ui_manager.c` | ⚠ Hard-coded in code, identical pattern to CENTER |
| Wire opcode `OPENDASH_CMD_SET_SCREEN_LAYOUT = 0x02` | protocol header | ⚠ Reserved, payload format not yet defined |
| Device Mgmt screen | [center/main/ui_manager.c](center/main/ui_manager.c) `config_devmgmt_btn_cb` ~L1135 | ⚠ Read-only health grid; no editor |
| NVS partition | `partitions.csv` | ✅ Exists on every node |
| LVGL touch input | `display_init.c` | ✅ Working |

Nothing is destructive. Adding the feature replaces a `const` table with a RAM table loaded from NVS and adds one new opcode payload format.

---

## 4. Design

### 4.1 New shared catalog — `common/include/opendash_dp_catalog.h` *(new file)*

```c
typedef enum {
    OPENDASH_DP_CAT_ENGINE = 0,    // RPM, MAP, MAF, throttle…
    OPENDASH_DP_CAT_TEMP,          // coolant, oil, intake, EGT…
    OPENDASH_DP_CAT_PRESSURE,      // boost, oil, fuel…
    OPENDASH_DP_CAT_FUEL,          // AFR, lambda, level, consumption
    OPENDASH_DP_CAT_DRIVETRAIN,    // gear, vehicle speed, trans temp
    OPENDASH_DP_CAT_GPS,           // speed, lat, lon, alt, fix, hdop
    OPENDASH_DP_CAT_BMS,           // pack volts, current, SOC, cell mV
    OPENDASH_DP_CAT_VESC,          // motor temp, fet temp, duty, current
    OPENDASH_DP_CAT_OBD,           // OBD-II PIDs not covered above
    OPENDASH_DP_CAT_SYSTEM,        // battery V, ambient T, free heap
    OPENDASH_DP_CAT_COUNT
} opendash_dp_category_t;

typedef struct {
    uint16_t  dp_id;            // OPENDASH_DP_* constant
    const char *short_name;     // "RPM", "EGT 1", "Coolant"  (≤10 chars)
    const char *units;          // "rpm", "°C", "kPa"
    float     default_min;      // sensible arc minimum
    float     default_max;      // sensible arc maximum
    uint8_t   category;         // opendash_dp_category_t
    uint8_t   decimals;         // display decimals (0,1,2,3)
} opendash_dp_info_t;

extern const opendash_dp_info_t opendash_dp_catalog[];
extern const size_t             opendash_dp_catalog_count;

const opendash_dp_info_t *opendash_dp_lookup(uint16_t dp_id);   // O(log n)
```

Implementation in `common/src/opendash_dp_catalog.c`. Sorted by `dp_id` so `opendash_dp_lookup` is binary-search.

**Size budget**: ~80 entries × ~32 B per row + strings ≈ 4 KB const flash. Trivial.

**Acceptance**: `opendash_dp_lookup(OPENDASH_DP_RPM)` returns non-NULL with `short_name="RPM"`, `units="rpm"`.

---

### 4.2 New wire payload — `OPENDASH_CMD_SET_SCREEN_LAYOUT = 0x02`

Already-reserved opcode. Define payload `screen_layout_v1_t`:

```
Byte  Field
────────────────────────────────────────────
 0    version          = 0x01
 1    mode             (display_mode_t value)
 2    slot_count       (1..16)
 3    arc_dp_id_hi
 4    arc_dp_id_lo     (0x0000 if no arc on this node)
 5..8 arc_min          (float, little-endian)
 9..12 arc_max         (float, little-endian)
13..  slot[0..slot_count-1]  each = dp_id_hi, dp_id_lo  (2B each)
```

Total: `13 + 2 × slot_count` bytes. For 16 slots = 45 B, well under the 248 B ESP-NOW payload limit. Single packet, no fragmentation.

**Direction**: CENTER → slave only. Slaves never send this opcode.

**Acceptance**: a hand-built buffer for `(mode=ENGINE, arc=RPM, slots=[COOLANT, OIL_T, BOOST, AFR, GEAR, SPEED])` round-trips through `opendash_i2c_serialize`/`deserialize` without checksum errors.

---

### 4.3 Per-node persistent storage

NVS namespace `od_layout` on every node. Key format: `m<mode>` (e.g. `m0`, `m1`, …). Value: raw `screen_layout_v1_t` payload bytes (same as wire format, minus the opcode wrapper).

API in `common/include/opendash_layout_store.h` *(new file)*:

```c
esp_err_t opendash_layout_load(uint8_t mode, screen_layout_v1_t *out);
esp_err_t opendash_layout_save(uint8_t mode, const screen_layout_v1_t *in);
esp_err_t opendash_layout_load_or_default(uint8_t mode,
                                          const screen_layout_v1_t *defaults,
                                          screen_layout_v1_t *out);
esp_err_t opendash_layout_factory_reset(void);   // erase namespace
```

`load_or_default` is the bootstrap helper: each node's `ui_manager_init()` calls it with its compiled-in defaults. First boot writes the defaults back so subsequent edits start from a known state.

**Acceptance**: save then load returns identical bytes; survives reboot; `factory_reset` clears all entries.

---

### 4.4 CENTER — runtime layout table

Replace `static const mode_dp_map_t mode_dp_maps[DISPLAY_MODE_COUNT]` with `static mode_dp_map_t mode_dp_maps[DISPLAY_MODE_COUNT]` (drop `const`).

In `ui_manager_init`, **after** `create_screen_layout()`:
```c
for (int m = 0; m < DISPLAY_MODE_COUNT; m++) {
    screen_layout_v1_t layout;
    if (opendash_layout_load_or_default(m, &compiled_defaults[m], &layout) == ESP_OK) {
        apply_layout_to_mode_dp_maps(m, &layout);
    }
}
```

Add helper `apply_layout_to_mode_dp_maps()` that copies `arc_dp/arc_min/arc_max/section_dp[]` from the layout struct into the runtime table, then triggers a redraw of the current mode if affected.

**Acceptance**: editing `m0` in NVS via `nvs_flash` test tool and rebooting changes the ENGINE-mode arc DP without recompiling.

---

### 4.5 Slaves — handler for incoming layout

Each slave (LEFT, RIGHT, GPS, POD1, POD2) gains in its `dispatch_message()`:

```c
case OPENDASH_CMD_SET_SCREEN_LAYOUT: {
    if (msg->length < sizeof(screen_layout_v1_t)) break;
    screen_layout_v1_t layout;
    memcpy(&layout, msg->payload, sizeof(layout));
    if (layout.version != 0x01) break;
    if (layout.mode >= DISPLAY_MODE_COUNT) break;
    apply_layout_to_mode_dp_maps(layout.mode, &layout);
    opendash_layout_save(layout.mode, &layout);          // persist
    if (layout.mode == current_mode) refresh_current_mode();
    break;
}
```

Same code on every slave because the helpers live in `common/`.

**Acceptance**: send a SET_SCREEN_LAYOUT to LEFT, monitor LEFT's serial — log "layout updated, mode=N", reboot LEFT, log "layout loaded from NVS, mode=N", values appear in correct slots.

---

### 4.6 CENTER UI — Layout Editor modal

Entry point: from existing **Device Mgmt** screen, each node-status box becomes clickable. Tap a node → opens **Layout Editor for `<NODE>`**. (Keep the read-only health view as the initial state; tap to edit.)

#### Editor screen (800 × 480)

```
┌───────────────────────────────────────────────────────────────────┐
│  ← BACK    Layout Editor — LEFT Gauge       [Reset] [Save & Push] │
├───────────────────────────────────────────────────────────────────┤
│  Mode: [ ENGINE ▼ ]                                               │
├───────────────────────────────────────────────────────────────────┤
│  ARC (center gauge)                                               │
│   [ RPM           ▶]   min [   0]   max [8000]                    │
│                                                                   │
│  SLOT 1 (top-left)        [ Coolant Temp °C   ▶]                  │
│  SLOT 2 (top-right)       [ Oil Pressure kPa  ▶]                  │
│  SLOT 3 (mid-left)        [ Boost kPa         ▶]                  │
│  SLOT 4 (mid-right)       [ AFR               ▶]                  │
│  SLOT 5 (bottom-left)     [ Gear              ▶]                  │
│  SLOT 6 (bottom-right)    [ Vehicle Speed     ▶]                  │
└───────────────────────────────────────────────────────────────────┘
```

* **Mode dropdown** — `lv_dropdown` listing every mode the target node supports. Switching mode reloads slot values from the in-RAM editor copy of that mode.
* **Slot rows** — each is a button labeled with current DP `short_name` + `units`. Tap → opens **PID Picker**.
* **Arc row** — same but with two numeric spinbox fields for min/max; defaults populate from the catalog when the DP is changed.
* **Save & Push** — packs `screen_layout_v1_t`, calls `espnow_master_send_screen_layout(node, mode, layout)`, and on success closes the editor with a toast `"Layout sent to LEFT"`.
* **Reset** — repopulates from `compiled_defaults[mode]`.

#### PID Picker modal

```
┌───────────────────────────────────────┐
│  Pick a PID            [Cancel] [None]│
├───────────────────────────────────────┤
│  Category: [ All ▼ ]                  │
│  ┌─────────────────────────────────┐  │
│  │ Engine                          │  │
│  │   ▶ RPM            rpm          │  │
│  │   ▶ Throttle Pos   %            │  │
│  │   ▶ MAF            g/s          │  │
│  │ Temperature                     │  │
│  │   ▶ Coolant        °C           │  │
│  │   ▶ Oil            °C           │  │
│  │   ▶ EGT 1          °C           │  │
│  │   …                             │  │
│  └─────────────────────────────────┘  │
└───────────────────────────────────────┘
```

* `lv_list` populated by walking `opendash_dp_catalog[]`, grouped by category header.
* Filter dropdown narrows by category.
* Tapping a row resolves to a `dp_id` and writes it back into the editor's slot.
* "None" sets `dp_id = 0` → slot rendered blank on target.

**Files added**:
* `center/main/layout_editor.c` / `.h` — owns editor + picker LVGL objects, lifecycle tied to entering/leaving the editor mode.
* `center/main/espnow_master.c` — new public API:
  ```c
  esp_err_t espnow_master_send_screen_layout(opendash_node_t node,
                                              const screen_layout_v1_t *layout);
  ```
  Internally builds `OPENDASH_CMD_SET_SCREEN_LAYOUT` and sends on **CH3 CONTROL** (configuration is control-plane, not telemetry).

**Acceptance**: User opens editor → picks RPM for slot 1 of ENGINE on LEFT → taps Save → LEFT's screen updates within 200 ms → reboots LEFT → setting persists.

---

## 5. Defaults

`compiled_defaults[DISPLAY_MODE_COUNT]` per node is the existing hard-coded mapping, captured as a `screen_layout_v1_t` literal. This guarantees zero behavior change for any user who never touches the editor.

---

## 6. Versioning & migration

* Layout struct starts at `version = 0x01`.
* On load, if `version != known`, log warning and fall back to defaults; do not crash.
* Future changes (new fields) bump version; loader contains explicit per-version reader functions.

---

## 7. Tasks for the jr dev (in order)

| # | Task | Est. effort | Files touched | Done when |
|---|------|------|---|---|
| 1 | Add catalog header + impl | 2 h | `common/include/opendash_dp_catalog.h`, `common/src/opendash_dp_catalog.c` | Unit test passes for 5 lookups |
| 2 | Add layout struct + serializer | 1 h | `common/include/opendash_layout.h` | Round-trip serialize/deserialize ok |
| 3 | Add NVS store helpers | 2 h | `common/include/opendash_layout_store.h`, `common/src/opendash_layout_store.c` | Save → reboot → load returns same bytes |
| 4 | Wire `SET_SCREEN_LAYOUT` payload | 30 min | protocol header comment | Doc only |
| 5 | CENTER: load defaults at boot, keep mode_dp_maps mutable | 1 h | `center/main/ui_manager.c` | First boot writes defaults to NVS, second boot reads them |
| 6 | CENTER: add `espnow_master_send_screen_layout` | 1 h | `center/main/espnow_master.{c,h}` | Sends on CH3, slave logs receipt |
| 7 | Slave: add SET_SCREEN_LAYOUT handler | 1 h × 5 nodes (mostly copy/paste) | `left/main/main.c`, `right/main/main.c`, `gps/main/main.c`, pods | Slave applies and persists; survives reboot |
| 8 | CENTER UI: Editor screen scaffolding | 3 h | `center/main/layout_editor.{c,h}` | Visual layout matches §4.6, no logic yet |
| 9 | CENTER UI: PID Picker modal | 2 h | same | Tapping a row updates the slot label |
| 10 | CENTER UI: hook to Device Mgmt | 30 min | `center/main/ui_manager.c` (`config_devmgmt_btn_cb`) | Tapping a node box opens editor for that node |
| 11 | CENTER UI: Save & Push wiring | 1 h | `layout_editor.c` | LEFT updates within 200 ms after Save |
| 12 | Bench test all 5 slaves | 1 h | — | Edit → push → reboot for each node OK |
| 13 | Update [DATAFLOW.md](DATAFLOW.md) §10 to mark item 4 done | 15 min | `DATAFLOW.md` | Doc reflects new state |

**Total**: ~16–20 h focused work.

---

## 8. Out of scope (for v0.9.0)

* Drag-to-reorder slots — slots are positional; users edit the DP that sits in slot N.
* Per-slot color theming — uses each node's existing palette.
* Multi-page modes (more than 7) — current modes are the universe.
* Companion-app sync — local touchscreen only. App support comes later via the same wire opcode.
* Custom user-named modes — fixed mode list for now.

---

## 9. Risk register

| Risk | Mitigation |
|---|---|
| Slave receives a layout referencing a DP it doesn't know how to render. | Slaves silently no-op unknown DPs (label = `"---"`). Catalog guarantees ID validity at edit time. |
| User saves a layout, then OTAs to firmware that removes a DP. | `load_or_default` validates each `dp_id` against the catalog on boot; unknown IDs fall back to default. |
| NVS write fails (full / corrupt). | `save` returns error, UI shows toast `"Save failed: <code>"`. Layout still applied in RAM for the session. |
| Layout push reaches slave but reboot happens before NVS flush. | NVS commit happens **before** ack; loss window ≤ NVS latency (~10 ms). Acceptable. |
| Two phones / two CENTERs try to push different layouts. | Last-writer-wins by design. CENTER is the only authoritative editor today. |

---

## 10. Definition of Done

- [ ] Catalog covers every `OPENDASH_DP_*` ID with non-empty short_name and units.
- [ ] Editing any slot on any node from the CENTER touchscreen updates that node's display within 200 ms.
- [ ] Power-cycling the node restores the user's last-saved layout.
- [ ] Factory reset returns every node to compiled defaults.
- [ ] No regression in `dp/s` throughput or `qHW` on CH1 (validated by 30-second monitor capture).
- [ ] DATAFLOW.md updated, CHANGELOG.md gets a v0.9.0 entry.

---

## 11. Integration status (live)

### ✅ Phase 1 — Transport + persistence (DONE)

| Item | Location |
|---|---|
| Catalog header / source | [common/include/opendash_dp_catalog.h](common/include/opendash_dp_catalog.h), [common/src/opendash_dp_catalog.c](common/src/opendash_dp_catalog.c) |
| Layout struct + (de)serializer | [common/include/opendash_layout.h](common/include/opendash_layout.h), [common/src/opendash_layout.c](common/src/opendash_layout.c) |
| NVS store | [common/include/opendash_layout_store.h](common/include/opendash_layout_store.h), [common/src/opendash_layout_store.c](common/src/opendash_layout_store.c) |
| Build wiring | [common/CMakeLists.txt](common/CMakeLists.txt) — added 3 SRCS |
| `espnow_master_send_screen_layout()` | [center/main/espnow_master.c](center/main/espnow_master.c), [center/main/espnow_master.h](center/main/espnow_master.h) |
| `OPENDASH_CMD_SET_SCREEN_LAYOUT` slave handler | left/right/gps/pod1/pod2 `main.c` — deserializes + persists to NVS, logs result |

End-to-end: CENTER can call `espnow_master_send_screen_layout(node, &layout)` and the receiving slave will save the new layout to NVS. The handler does **not yet** rebind the live UI — that's Phase 2 because each slave's UI binding is node-specific.

### ⏳ Phase 2 — Live apply + editor UI (TODO)

| # | Task |
|---|---|
| 5 | CENTER: drop `const` from `mode_dp_maps`; call `opendash_layout_load_or_default()` for each mode in `ui_manager_init` and copy into the runtime table |
| 6 | Per-slave `apply_layout_to_mode_dp_maps()` impl + `refresh_current_mode()` — each node rebinds its widgets to the new DPs |
| 8–11 | LVGL Layout Editor + PID Picker in `center/main/layout_editor.{c,h}`, hooked from Device Mgmt screen |
| 12 | Bench test all 5 slaves end-to-end |
| 13 | Update DATAFLOW.md / CHANGELOG.md |
