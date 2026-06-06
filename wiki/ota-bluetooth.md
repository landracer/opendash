<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OTA over Bluetooth Low Energy — End-User Guide

> **Updated June 2026** to cover all 11 OpenDash node families.
> Step-by-step procedure for updating any node wirelessly from a Linux laptop
> or Android phone (Android path: see [`ota-android-plan.md`](./ota-android-plan.md)).
>
> Typical time: **≤5 min** for LEFT/RIGHT (2.4 MB); **<2 min** for relay/MOS/GPS/POD nodes (<1 MB).
>
> If you want the deep technical root-cause and protocol reference, read
> [`../BLE_OTA.md`](../BLE_OTA.md) instead.

---

## 1. Prerequisites

### Hardware
- A Linux laptop with Bluetooth 4.0+ (virtually all machines made after 2012).
- The **CENTER** node alive and connected via USB **or** within touch range of
  the CONFIG screen (see §2 for both paths).
- The target pod **within ~1 metre** of the laptop during transfer. Proximity
  matters — RSSI below about −70 dBm causes mid-OTA supervision timeouts.

### One-time software setup

```bash
# Verify BlueZ is active
systemctl is-active bluetooth          # should print: active

# Create a venv (one time only)
cd ~/Documents/rAtTrax-Dash/opendash
python3 -m venv .venv-ota
source .venv-ota/bin/activate
pip install bleak pyserial

# Or install bleak system-wide if you prefer:
# python3 -m pip install --break-system-packages bleak
```

If you hit `Permission denied on hci0`:
```bash
sudo usermod -aG bluetooth $USER && newgrp bluetooth
```

### Build the firmware first
```bash
cd ~/Documents/rAtTrax-Dash/opendash
. ~/Documents/esp-ide/esp-idf/export.sh
idf.py -C right build      # replace 'right' with any node name
```

Binary output locations for all nodes:

| Node | Binary path |
|---|---|
| center | `center/build/opendash_center.bin` |
| left | `left/build/opendash_leftright.bin` |
| right | `right/build/opendash_leftright.bin` |
| gps | `gps/build/opendash_gps.bin` |
| pod1 | `pod1/build/opendash_pod1.bin` |
| pod2 | `pod2/build/opendash_pod2.bin` |
| relay-4ch-hd | `relay-4ch-hd/build/opendash_relay_4ch_hd.bin` |
| relay-8ch-a | `relay-8ch-a/build/opendash_relay_8ch_a.bin` |
| relay-8ch-b | `relay-8ch-b/build/opendash_relay_8ch_b.bin` |
| mos-4ch-a | `mos-4ch-a/build/opendash_mos_4ch_a.bin` |
| mos-4ch-b | `mos-4ch-b/build/opendash_mos_4ch_b.bin` |

> ⚠️ Flash the **application** image only. Never use `bootloader.bin` or a
> merged `*-full.bin` with BLE OTA — it will write to the next OTA slot and
> leave the bootloader intact.

---

## 2. Put the node into OTA mode

There are two ways. Use whichever is more convenient.

### Path A — CENTER touch UI (preferred, in-vehicle)

1. On the CENTER dash, tap through to **Config → OTA Flash**.
2. Tap the target node's button. The CENTER sends `ENTER_BT_OTA` over ESP-NOW.
3. The target pod displays **`WAITING FOR OTA — <NODE>`** and starts advertising.

> The CONFIG screen shows each node's online/offline status. If the target
> shows **OFFLINE**, check power and ESP-NOW range before proceeding.

### Path B — CENTER console (developer / fallback)

Open a terminal on the laptop:
```bash
. ~/Documents/esp-ide/esp-idf/export.sh
idf.py -C center -p /dev/ttyACM4 monitor
```
At the `>` prompt:
```
ota right
```
You should see:
```
opendash_center: Sent ENTER_BT_OTA x3 to right (attempts=3)
```

> Replace `/dev/ttyACM4` with your CENTER's actual port. Identify by serial:
> `udevadm info -q property /dev/ttyACM* | grep -A1 ID_SERIAL_SHORT`

> **Important:** type `ota right` as a single enter — do not tap the button or
> type the command a second time while the node is initialising BT. A duplicate
> trigger mid-init causes a race condition (tracked in TODO).

---

## 3. Scan to confirm the node is advertising

Before connecting, verify the node is actually in OTA mode:

```bash
source .venv-ota/bin/activate
python3 ble_ota.py --list
```

Sample output:
```
Scanning for 'OpenDash*' advertisers (20s)...

  NAME                              ADDRESS            RSSI
  --------------------------------  -----------------  -----
  OpenDash-RIGHT-OTA                3C:DC:75:6E:A0:A6  -52 dBm
  OpenDash-MOS_4CH_A-OTA            AA:BB:CC:11:22:44  -71 dBm
```

If nothing appears:
- The node is not in OTA mode (still running normal firmware — OTA service is
  not advertised in normal operation).
- BlueZ is rfkill'd: `rfkill unblock bluetooth`.
- BlueZ scanning is stale: `bluetoothctl power off; sleep 1; bluetoothctl power on`.

---

## 4. Flash

### By node alias (most common)
```bash
python3 -u ble_ota.py --node right --chunk-size 512 \
    right/build/opendash_leftright.bin
```

### By advertised name prefix
```bash
python3 ble_ota.py --device "OpenDash-RELAY_8CH_B-OTA" \
    relay-8ch-b/build/opendash_relay_8ch_b.bin
```

### By MAC address (fastest, skips scan)
```bash
python3 ble_ota.py --address 3C:DC:75:6E:A0:A6 \
    right/build/opendash_leftright.bin
```

### What you should see (typical RIGHT example)
```
Firmware: right/build/opendash_leftright.bin  (2,425,520 bytes)
Connecting to 3C:DC:75:6E:A0:A6...
Connected. Subscribing to status notifications...
Sending START command...
  [STATUS] RECEIVING  0%  err=0
Sending 2,425,520 bytes in 4738 chunks of 512B...
  [████░░░░░░░░░░░░░░░░]  21%  522,240/2,425,520 B
  [████████████████████] 100%  2,425,520/2,425,520 B
Waiting 1.60s for peripheral write queue flush...
Sending END command...
  [STATUS] VERIFYING  92%  err=0
  [STATUS] COMPLETE   92%  err=0

RESULT: PASS (DONE received)
===== WALL TIME: 271s (4m 31s) =====
```

The pod reboots automatically. In the CENTER monitor you will see it return to
ONLINE within ~5 seconds.

---

## 5. All `ble_ota.py` flags

| Flag | Short | Effect |
|---|---|---|
| `--node NAME` | | Node alias (`left`, `right`, `gps`, `pod1`, `pod2`). Implies the device name and is the friendliest option. |
| `--list` | `-l` | Scan for all `OpenDash-*-OTA` advertisers and exit. Use for discovery. |
| `--device PREFIX` | `-d` | Connect to first advertiser whose name matches this prefix. |
| `--address MAC` | `-a` | Skip scan; connect directly by BLE MAC. Fastest. |
| `--chunk-size N` | | Bytes per BLE GATT write (default 512; max negotiable). |
| `--scan-timeout SEC` | | Scan window in seconds (default 20). |

---

## 6. Recovering from interruptions

### "UNLIKELY_ERROR" on first attempt
The client retries automatically up to 4 times. This is normal — the node
needs ~2 s to settle after `ENTER_BT_OTA`. Just wait.

### Mid-OTA disconnect
The device persists the last verified write offset. Re-run the same command:
```
Server reports offset=917,504 — sending RESUME...
Resuming at 917,504/2,425,520 bytes (37%)...
```

### Pod stuck on `WAITING FOR OTA` screen
Press the **BOOT button** once to reboot into the previous image, or:
```
center> reset right
```

### BlueZ in a weird state / stale pairing cache
```bash
bluetoothctl power off; sleep 1; bluetoothctl power on
```
If the above doesn't work, remove the cached entry:
```bash
bluetoothctl
> remove 3C:DC:75:6E:A0:A6
> exit
```

### `STATUS ERROR err=1` (bad image header)
Wrong binary selected. Verify you are passing the application `.bin` for the
correct node (e.g. do not flash `left/build/...bin` to RIGHT).

### `STATUS ERROR err=2` (flash write failure)
Partition table corruption. Recover via USB:
```bash
idf.py -C right -p /dev/ttyACM1 flash
```

---

## 7. Node-specific notes

### LEFT and RIGHT (round 480×480 RGB pods)
These pods have a 18 MHz RGB DMA driving the LCD from PSRAM. The DMA, LVGL
render task, and MD UART parser together saturate the PSRAM bus and starve the
BT controller's link-layer ISR unless explicitly mitigated. The mitigation is
already in production firmware:

1. `ui_manager_suspend()` — stops LVGL drawing
2. `opendash_uart_suspend()` *(LEFT only)* — stops MD UART task
3. `display_pause_for_ota()` — calls `esp_lcd_panel_del(s_panel)` to tear down
   the bounce-buffer DMA entirely (blanking the backlight is not enough)
4. BT stack pinned to CPU1 (`CONFIG_BT_CTRL_PINNED_TO_CORE_1=y`)
5. 2M PHY + correct PPCP keys in `sdkconfig.defaults`

You don't need to do anything special — this all happens automatically when
the node receives `ENTER_BT_OTA`. Just trigger and flash.

### GPS / POD1 / POD2 (AMOLED, no RGB DMA)
These nodes do not have a bounce-buffer DMA, so BT contention is not a
problem. OTA works reliably without special mitigation. Typical transfer time
is well under 2 minutes for ~1 MB images.

Full sdkconfig hardening (2M PHY, PPCP, suspend sequence) is tracked in
TODO §1.2 but is low urgency — field failures have not been observed on these
nodes.

### Relay and MOS nodes (headless)
No display, no DMA. OTA works without any special mitigation. Typical image
size <1 MB, transfer <90 s. Trigger via CENTER console: `ota mos_4ch_a`,
`ota relay_8ch_b`, etc. Or use `--device "OpenDash-MOS"` to connect by prefix.

---

## 8. Reference

- Companion script: [`../ble_ota.py`](../ble_ota.py)
- Device-side OTA source: [`../common/src/opendash_bt_ota.c`](../common/src/opendash_bt_ota.c)
- Full GATT protocol + troubleshooting: [`../BLE_OTA.md`](../BLE_OTA.md)
- Android OTA options: [`./ota-android-plan.md`](./ota-android-plan.md)
- CHANGELOG entry for this work: [`../CHANGELOG.md`](../CHANGELOG.md) v0.4.0-beta + v0.4.1-beta
- sdkconfig reference: `left/sdkconfig.defaults` or `right/sdkconfig.defaults`, BLE OTA section

---

## 1. Prerequisites

### Hardware
- A Linux laptop (Bluetooth 4.0+; nearly any modern machine).
- The CENTER node alive and connected via USB (you trigger OTA via its console).
- The target pod powered and within **~1 metre** of the laptop. Yes, really.
  RSSI under about −70 dBm causes mid-OTA disconnects.

### Software
- ESP-IDF installed: `~/Documents/esp-ide/esp-idf`
- Python virtualenv with `bleak`:
  ```bash
  cd ~/Documents/rAtTrax-Dash/opendash
  python3 -m venv .venv-ota
  source .venv-ota/bin/activate
  pip install bleak pyserial
  ```
- BlueZ running and powered on:
  ```bash
  bluetoothctl power on
  ```

### Build the firmware
```bash
cd ~/Documents/rAtTrax-Dash/opendash
. ~/Documents/esp-ide/esp-idf/export.sh
idf.py -C right build       # or -C left, -C gps, -C pod1, -C pod2
```
Output: `right/build/opendash_leftright.bin` (~2.4 MB for LEFT/RIGHT).

---

## 2. The procedure (RIGHT example)

### Step 1 — open the CENTER console

```bash
. ~/Documents/esp-ide/esp-idf/export.sh
idf.py -C center -p /dev/ttyACM4 monitor
```
Wait for the status lines to settle and confirm the target node is
`ONLINE`. Don't proceed if the target is `OFFLINE`.

### Step 2 — trigger OTA mode

At the console `>` prompt, type:
```
ota right
```
You should see:
```
opendash_center: Sent ENTER_BT_OTA x3 to right (attempts=3)
```
The pod's LCD will go black and show **`WAITING FOR OTA — RIGHT`**.
The pod is now advertising as `OpenDash-RIGHT-OTA` and the LCD bounce-DMA
has been torn down so BT has clean access to the bus.

### Step 3 — run the OTA client

In a **separate terminal** (don't close the CENTER monitor):
```bash
cd ~/Documents/rAtTrax-Dash/opendash
source .venv-ota/bin/activate
python3 -u ble_ota.py --node right --chunk-size 512 \
    right/build/opendash_leftright.bin
```

What you should see (typical):
```
Connecting to 3C:DC:75:6E:A0:A6...
Connected. Subscribing to status notifications...
Sending START command...
  [STATUS] RECEIVING  0%  err=0
Sending 2,425,520 bytes in 4738 chunks of 512B...
  [█████████████████░░░] 87%  ...
  [████████████████████] 100%
Waiting 1.60s for peripheral write queue flush...
Sending END command...
  [STATUS] COMPLETE  92%  err=0

RESULT: PASS (DONE received)
```

The pod reboots automatically into the new image.

### Step 4 — verify

In the CENTER monitor you should see RIGHT come back online, then
return to its normal data rate within a few seconds. Done.

---

## 3. Recovering from interruptions

### "Connect failed (UNLIKELY_ERROR)" on first attempt
The client retries up to 4 times. The pod sometimes needs a brief
moment to settle after `ENTER_BT_OTA`. Wait for the retry — it almost
always succeeds.

### Mid-OTA disconnect
The protocol persists the last verified write offset on the device side.
Just re-run the exact same client command. You'll see:
```
Server reports offset=917,504 — sending RESUME...
Resuming at 917,504/2,425,520 bytes (37%)...
```

### Pod stuck on `WAITING FOR OTA` after a failure
Press the **BOOT button** to reboot the pod into its previous image, or
run `reset right` from the CENTER console.

### BlueZ in a weird state
```bash
bluetoothctl power off
sleep 1
bluetoothctl power on
```
If that fails, remove the cached device:
```bash
bluetoothctl
> remove 3C:DC:75:6E:A0:A6
> exit
```

---

## 4. Why so many moving parts?

The round LEFT/RIGHT pods drive a 480×480 RGB LCD via a continuous
bounce-buffer DMA out of PSRAM. That DMA, the LVGL render task, and
the MD UART parser all contend for CPU0. The BT controller's
link-layer ISR runs at high priority but if it can't get a slice of
CPU0 within the connection-event window, the **HCI link drops** before
NimBLE even sees the connection. POD1/POD2/GPS don't have a
bounce-DMA display so they don't hit this problem.

The fix is a layered defence:
1. **Suspend the noisy tasks** before BLE starts.
2. **Pin the BT stack to CPU1** so it never contends with whatever is
   left running.
3. **Negotiate fast 2M-PHY** so the transfer ends quickly even when
   the BlueZ central refuses to grant us a fast connection interval.

You don't need to remember any of this. You just need:

```
center>  ota right
laptop$  python3 ble_ota.py --node right --chunk-size 512 right/build/opendash_leftright.bin
```

---

## 5. Reference

- Source: [`common/src/opendash_bt_ota.c`](../common/src/opendash_bt_ota.c)
- Client: [`ble_ota.py`](../ble_ota.py)
- Sdkconfig keys: see `right/sdkconfig.defaults` BLE OTA section
- Root-cause writeup: [`BLE_OTA.md`](../BLE_OTA.md)
- Changelog entry: [`CHANGELOG.md`](../CHANGELOG.md) v0.4.0-beta
