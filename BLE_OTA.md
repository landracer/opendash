<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OpenDash BLE OTA — Wireless Firmware Flashing

Flash any OpenDash node from this laptop over Bluetooth Low Energy — no USB
cable, no opening the dash.

The companion tool is [`ble_ota.py`](ble_ota.py). It uses
[`bleak`](https://bleak.readthedocs.io/) on top of the laptop's built-in
Bluetooth adapter (BlueZ on Linux).

---

## 1. One-time host setup

Linux (Fedora/Ubuntu/Arch — anything with BlueZ + DBus):

```bash
# Verify BlueZ is up
systemctl is-active bluetooth            # should print: active

# Install bleak into the system Python (or a venv)
python3 -m pip install --break-system-packages bleak
# or, in a venv:
#   python3 -m venv .venv && . .venv/bin/activate && pip install bleak
```

No pairing is required — OpenDash OTA uses an unauthenticated GATT service
(see the protocol details below). You also don't need to be `root`; the
`bluetooth` group is enough on most distros. If you hit "permission denied"
on `hci0`, add yourself:

```bash
sudo usermod -aG bluetooth $USER && newgrp bluetooth
```

---

## 2. Put a node into BLE OTA mode

A node will not advertise the OTA service until you ask it to. There are three
ways:

| Method | When to use |
| --- | --- |
| **Center dash → Config → OTA Flash → pick node** | Normal in-vehicle workflow. The center sends `MSG_ENTER_BT_OTA` over ESP-NOW; the slave reboots into OTA mode. |
| **Hold `BOOT` (GPIO0) during power-on** (boards that support it) | Useful if ESP-NOW is broken on the target node. |
| **Direct UART command** | `idf.py -p /dev/ttyACM<n> monitor` and send `ota` if the node exposes a console command. |

Once in OTA mode the node:

1. Lights its BLE-OTA banner / LED if it has one.
2. Advertises as `OpenDash-<NODE_NAME>-OTA`, e.g. `OpenDash-RELAY_8CH_B-OTA`.
3. The center dash shows a flashing **BLE OTA** badge for that node (driven
   by the `STATUS_REPORT.flags.BLE_OTA` bit — see
   [common/include/opendash_i2c_protocol.h](common/include/opendash_i2c_protocol.h)).

---

## 3. Discover devices

```bash
python3 ble_ota.py --list
```

Sample output:

```
Scanning for 'OpenDash*' advertisers (10s)...

  NAME                              ADDRESS            RSSI
  --------------------------------  -----------------  -----
  OpenDash-RELAY_8CH_B-OTA          AA:BB:CC:11:22:33  -54 dBm
  OpenDash-MOS_4CH_A-OTA            AA:BB:CC:11:22:44  -71 dBm
```

If nothing shows up:

- The node isn't actually in OTA mode (still running normal firmware → BLE
  service is not advertised).
- Bluetooth adapter is rfkill'd: `rfkill list` then `rfkill unblock bluetooth`.
- BlueZ scanning is paused: `bluetoothctl power on; bluetoothctl scan on`
  in another terminal for a few seconds, then retry.

---

## 4. Flash

By name prefix (most common — the script picks the first match):

```bash
python3 ble_ota.py --device "OpenDash-RELAY_8CH_B-OTA" \
    relay-8ch-b/build/opendash_relay_8ch_b.bin
```

Or by MAC address (skips the scan, fastest):

```bash
python3 ble_ota.py --address AA:BB:CC:11:22:33 \
    relay-8ch-b/build/opendash_relay_8ch_b.bin
```

The full flow you'll see:

```
Firmware: relay-8ch-b/build/opendash_relay_8ch_b.bin  (945,312 bytes)
Connecting to AA:BB:CC:11:22:33...
Connected. Subscribing to status notifications...
Sending START command...
Sending 945,312 bytes in 1847 chunks of 512B...
  [████████████████████]  100%  945,312/945,312 B
Sending END command...
Waiting for verification and reboot...
  [STATUS] VERIFYING  100%  err=0
  [STATUS] DONE       100%  err=0

OTA transfer complete — device should be rebooting with new firmware.
```

Approximate transfer time: **~30 s/MB** at default 512-byte chunks. For a
typical 800 KB relay/pod image this is well under a minute.

### Useful options

| Flag | Effect |
| --- | --- |
| `--list, -l` | Scan only, print all `OpenDash-*-OTA` advertisers, exit. |
| `--device PREFIX, -d` | Override the default `OpenDash` name filter (e.g. `-d "OpenDash-MOS"`). |
| `--address MAC, -a` | Skip scan; connect directly to a known MAC. |
| `--scan-timeout SEC` | How long to scan for the device (default 20 s). |

---

## 5. Per-node firmware paths

After a successful `idf.py -C <node> build`, the image is at
`<node>/build/opendash_<node>.bin`. Common ones:

| Node | Binary path |
| --- | --- |
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

> ⚠️ Flash the **application** image only (`opendash_*.bin`), not
> `bootloader.bin` or the merged `*-full.bin`. BLE OTA writes to the next OTA
> slot only.

---

## 6. Protocol reference

GATT service exposed by every node in OTA mode:

| Item | UUID | Properties | Notes |
| --- | --- | --- | --- |
| Service | `0x00FF` (full: `000000ff-0000-1000-8000-00805f9b34fb`) | — | Primary |
| `CTRL` | `0xFF01` | write w/ response | `0x01` = START, `0x02` = END |
| `DATA` | `0xFF02` | write **without** response | 512-byte chunks of raw firmware |
| `STATUS` | `0xFF03` | notify | 3-byte payload: `[state, progress%, err]` |

`STATUS` state codes:

| Value | Meaning |
| --- | --- |
| 0 | IDLE |
| 1 | RECEIVING |
| 2 | VERIFYING |
| 3 | DONE (device will reboot shortly) |
| 4 | ERROR (see `err` byte) |

The Python client treats `DONE` and `ERROR` as terminal — it stops waiting as
soon as either arrives, instead of a blind sleep.

---

## 7. Troubleshooting

| Symptom | Likely cause / fix |
| --- | --- |
| `(none found …)` on `--list` | Node isn't in OTA mode yet. Use Center → Config → OTA Flash. |
| Connect succeeds, then drops after START | MTU too small / interference. Move closer; rerun. |
| Stalls partway through chunks | Other Bluetooth traffic on the same host — disable other BLE clients (e.g. `bluetoothctl`'s `agent on`). |
| `STATUS ERROR  err=1` (image header bad) | Wrong binary — flashed `bootloader.bin` or wrong node's image. |
| `STATUS ERROR  err=2` (write fail) | Flash partition issue. Reflash over USB to recover. |
| Adapter not present | `rfkill unblock bluetooth`, or USB-detach/attach the BT dongle. |

---

## 8. Related docs

- [BLUETOOTH_PAIRING.md](BLUETOOTH_PAIRING.md) — companion-app pairing (not OTA).
- [common/include/opendash_i2c_protocol.h](common/include/opendash_i2c_protocol.h) — `STATUS_REPORT.flags.BLE_OTA` bit definition.
- [center/main/ui_manager.c](center/main/ui_manager.c) — dash-side BLE OTA banner + OTA Flash menu.
