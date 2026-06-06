<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OpenDash Device Identity & Serial Numbers

## Overview

OpenDash uses a **device identity system** to prevent flashing the wrong firmware to the wrong ESP32 unit. With 5+ physically similar devices in the car (Center, Left, Right, GPS, BMS), it's easy to accidentally flash GPS firmware onto the Left gauge pod — this system catches that mistake at boot.

**How it works:**
1. Each ESP32 chip has a **unique MAC address** burned in at the factory — this is the **serial number**.
2. Each firmware build is compiled for a specific **node type** (CENTER, LEFT, RIGHT, GPS, etc.).
3. On **first boot**, the node type is stored in NVS (Non-Volatile Storage) alongside the MAC.
4. On **subsequent boots**, the compiled node type is compared against the NVS-stored value.
5. A **mismatch** produces a prominent warning in the serial log — the firmware will still run, but the warning is unmistakable.

---

## Boot Log Output

### Normal boot (match):
```
I (od_identity) ┌──────────────────────────────────────────────┐
I (od_identity) │  DEVICE IDENTITY                             │
I (od_identity) │  Serial: AA:BB:CC:DD:EE:FF              │
I (od_identity) │  Node:   GPS        (compiled)               │
I (od_identity) │  Status: MATCH OK                            │
I (od_identity) └──────────────────────────────────────────────┘
```

### Mismatch (wrong firmware on wrong unit):
```
I (od_identity) ┌──────────────────────────────────────────────┐
I (od_identity) │  DEVICE IDENTITY                             │
I (od_identity) │  Serial: AA:BB:CC:DD:EE:FF              │
I (od_identity) │  Node:   LEFT       (compiled)               │
E (od_identity) │  ╔════════════════════════════════════════╗   │
E (od_identity) │  ║  !! FIRMWARE / HARDWARE MISMATCH !!   ║   │
E (od_identity) │  ║  NVS says: GPS                        ║   │
E (od_identity) │  ║  Firmware:  LEFT                       ║   │
E (od_identity) │  ║  Wrong firmware on this unit?         ║   │
E (od_identity) │  ╚════════════════════════════════════════╝   │
I (od_identity) └──────────────────────────────────────────────┘
```

---

## Supported Node Types

| Enum Value | Name | Description |
|------------|------|-------------|
| 0 | CENTER | Center display (4.3" LCD, ESP-NOW master) |
| 1 | LEFT | Left gauge pod (2.8" round LCD) |
| 2 | RIGHT | Right gauge pod (2.8" round LCD) |
| 3 | GPS | GPS/Telemetry unit (1.75" AMOLED) |
| 4 | BMS | External BMS node (rAtTrax) |
| 5–12 | POD1–POD8 | Expansion gauge pods |
| 13 | RELAY-4CH | 4-channel HD relay module |
| 14–15 | RELAY-8CH-A/B | 8-channel relay modules |
| 16–17 | MOS-4CH-A/B | 4-channel MOSFET modules |

---

## Adding Identity to a New Node

### Step 1: Include the header

In your new node's `main.c`:

```c
#include "opendash_identity.h"
```

### Step 2: Call init after NVS

In `app_main()`, after `nvs_flash_init()`:

```c
void app_main(void)
{
    // NVS must be initialized first
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // Initialize identity — use the correct node type for this unit
    opendash_identity_init(OPENDASH_NODE_GPS);  // or CENTER, LEFT, RIGHT, etc.

    // ... rest of initialization
}
```

### Step 3: Choose the correct node type

Use the `opendash_node_t` enum value that matches the hardware this firmware is built for. The available values are defined in `common/include/opendash_common.h`.

### Step 4: Build and flash

```bash
cd <node-directory>
. ~/Documents/esp-ide/esp-idf/export.sh > /dev/null 2>&1
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

On first flash, the serial log will say "First boot — identity stored: GPS" (or whichever type). All subsequent boots will compare.

---

## Re-purposing a Unit (Changing Identity)

If you intentionally want to re-assign a unit (e.g., turn a spare Left into a Right), you have two options:

### Option A: Call `opendash_identity_reset()` in code

Add a temporary call before the identity init:

```c
opendash_identity_reset();  // Wipe stored identity
opendash_identity_init(OPENDASH_NODE_RIGHT);  // Store new identity
```

Build, flash, verify the new identity is stored, then **remove** the `opendash_identity_reset()` line and flash again. This prevents the identity from being wiped on every boot.

### Option B: Erase NVS via idf.py

```bash
idf.py -p /dev/ttyACM0 erase-flash
```

This erases the entire flash including NVS. Then flash the new firmware — it will treat the next boot as a "first boot" and store the new identity.

> **Warning:** Erasing flash also wipes any other NVS data (odometer, configuration, calibration). Use Option A if you want to preserve other NVS data.

---

## Querying Identity at Runtime

To read the identity from application code:

```c
opendash_identity_t id;
if (opendash_identity_get(&id) == ESP_OK) {
    ESP_LOGI(TAG, "Serial: %s", id.serial_str);
    ESP_LOGI(TAG, "Node: %s", opendash_node_name(id.compiled_node));
    ESP_LOGI(TAG, "Match: %s", id.identity_match ? "YES" : "NO");
    ESP_LOGI(TAG, "First boot: %s", id.first_boot ? "YES" : "NO");
}
```

The `opendash_identity_t` structure contains:

| Field | Type | Description |
|-------|------|-------------|
| `mac[6]` | `uint8_t[6]` | Raw WiFi MAC address bytes |
| `serial_str` | `char[18]` | MAC formatted as `"AA:BB:CC:DD:EE:FF"` |
| `compiled_node` | `opendash_node_t` | Node type this firmware was built for |
| `stored_node` | `opendash_node_t` | Node type previously stored in NVS |
| `identity_match` | `bool` | `true` if compiled == stored |
| `first_boot` | `bool` | `true` if NVS had no prior identity |

---

## Source Files

| File | Description |
|------|-------------|
| `common/include/opendash_identity.h` | Public API and data types |
| `common/src/opendash_identity.c` | NVS storage and MAC reading logic |
| `common/include/opendash_common.h` | `opendash_node_t` enum definition |

---

## NVS Details

- **Namespace**: `od_identity`
- **Key**: `node_type` (uint8_t — the enum value)
- NVS must be initialized before calling `opendash_identity_init()`
- The identity system never blocks or halts the firmware — mismatches are warnings only

---

## Headless Devices (No Serial Console)

Some OpenDash peripheral nodes may not have USB serial console access during
normal operation:

| Device Type | Serial Access | Identity Visibility |
|-------------|---------------|---------------------|
| Center (4.3" LCD) | USB-CDC via ACM0 | Full boot banner in serial monitor |
| Left (2.8" Round) | USB-CDC via ACM2 | Full boot banner in serial monitor |
| Right (2.8" Round) | USB-CDC (when connected) | Full boot banner in serial monitor |
| GPS (1.75" AMOLED) | USB-CDC via ACM1 | Full boot banner in serial monitor |
| BMS (rAtTrax) | USB-UART via USB0 | Full boot banner (Arduino Serial) |
| Relay 4CH HD | **No USB in car** | Identity stored in NVS but mismatch warning not visible without monitor |
| MOS 4CH A/B | **No USB in car** | Identity stored in NVS but mismatch warning not visible without monitor |
| Pod1-Pod8 | Depends on mounting | May or may not have accessible USB |

### Implications for Headless Nodes

- The identity system still **works** on headless devices — the node type is
  stored in NVS and compared at every boot.
- The mismatch warning is **only visible** if a serial monitor is attached.
- **Recommendation:** When flashing headless nodes, always attach a serial
  monitor ONCE to verify the identity system initialized correctly.
- **Future improvement:** Consider adding LED blink patterns (e.g., on GPIO)
  to signal identity mismatch on devices without serial console. A slow blink
  could indicate "OK" while fast blink indicates "mismatch".
- **ESP-NOW broadcast:** Consider broadcasting identity status via ESP-NOW
  so the Center display can show which nodes are online and whether any have
  identity mismatches. This would make headless node debugging visible from
  the dashboard.

### Verifying a Headless Node's Identity

If you suspect a headless node has the wrong firmware:

1. **Temporarily connect a USB cable** and run `idf.py -p <port> monitor`
2. Press the reset button (or toggle RTS) to restart the board
3. Look for the identity banner in the boot log
4. If there's a mismatch, re-flash with the correct firmware

Alternatively, if ESP-NOW identity broadcast is implemented (see Future
Improvements above), the Center display will show the mismatch without
requiring physical USB access.
