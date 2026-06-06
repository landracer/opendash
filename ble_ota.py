#!/usr/bin/env python3
# Licensed under Sovereign Individual License v1.0 — see LICENSE file
"""
OpenDash BLE OTA Client
=======================
Pushes a firmware binary to an OpenDash relay/MOS node in BLE OTA mode.

Usage:
    python3 ble_ota.py --node left left/build/opendash_left.bin
    python3 ble_ota.py --node right right/build/opendash_right.bin
  python3 ble_ota.py relay-8ch-a/build/opendash_relay_8ch_a.bin
  python3 ble_ota.py relay-8ch-b/build/opendash_relay_8ch_b.bin
  python3 ble_ota.py --device "OpenDash-RELAY_8CH_B-OTA" relay-8ch-b/build/opendash_relay_8ch_b.bin

The node must already be in BLE OTA mode (triggered via center Config screen OTA button,
or by holding GPIO0 at boot on boards that support it).

BLE service: 0x00FF
  CTRL  char: 0xFF01  (write: 0x01=start, 0x02=end)
  DATA  char: 0xFF02  (write-without-response: binary chunks)
  STATUS char: 0xFF03 (notify: [state, progress%, reserved])
"""

import asyncio
import argparse
import os
import socket
import sys
from bleak import BleakClient, BleakScanner
from bleak.exc import BleakGATTProtocolError
from bleak.exc import BleakError

# dbus_fast bits used for direct AcquireWrite — Bleak's write_gatt_char
# does a synchronous D-Bus WriteValue per chunk which caps throughput at
# ~3 KB/s on BlueZ. AcquireWrite returns a SOCK_SEQPACKET fd that bypasses
# D-Bus entirely.
try:
    from dbus_fast.message import Message as _DBusMessage
    from dbus_fast.signature import Variant as _DBusVariant
    _HAVE_DBUS_FAST = True
except Exception:
    _HAVE_DBUS_FAST = False

# UUIDs — match opendash_bt_ota.h
SVC_UUID    = "000000ff-0000-1000-8000-00805f9b34fb"  # 0x00FF
CTRL_UUID   = "0000ff01-0000-1000-8000-00805f9b34fb"  # 0xFF01
DATA_UUID   = "0000ff02-0000-1000-8000-00805f9b34fb"  # 0xFF02
STATUS_UUID = "0000ff03-0000-1000-8000-00805f9b34fb"  # 0xFF03
OFFSET_UUID = "0000ff04-0000-1000-8000-00805f9b34fb"  # 0xFF04 (resume offset)

CHUNK_SIZE  = 512   # bytes per DATA write — safe for BLE MTU
CTRL_START  = b'\x01'
CTRL_END    = b'\x02'
CTRL_RESUME = 0x05  # +4 LE bytes offset

OTA_STATE = {
    0: "IDLE",
    1: "READY",
    2: "CONNECTED",
    3: "RECEIVING",
    4: "VERIFYING",
    5: "COMPLETE",
    255: "ERROR",
}

NODE_TO_DEVICE_PREFIX = {
    "left": "OpenDash-LEFT-OTA",
    "right": "OpenDash-RIGHT-OTA",
}


async def acquire_write_socket(client: BleakClient, char_uuid: str, mtu_hint: int = 512):
    """Open a raw SOCK_SEQPACKET to BlueZ for write-without-response.

    Returns (socket, mtu) or (None, 0) on failure. Caller must close the
    socket when done. Throughput jumps from ~3 KB/s (D-Bus per write) to
    ~20+ KB/s (kernel socket) because each chunk no longer round-trips
    through dbus-daemon.
    """
    if not _HAVE_DBUS_FAST:
        return None, 0
    try:
        backend = client._backend  # type: ignore[attr-defined]
        bus = backend._bus         # type: ignore[attr-defined]
        char = client.services.get_characteristic(char_uuid)
        if char is None:
            return None, 0
        char_path = char.obj[0]
        msg = _DBusMessage(
            destination='org.bluez',
            path=char_path,
            interface='org.bluez.GattCharacteristic1',
            member='AcquireWrite',
            signature='a{sv}',
            body=[{'mtu': _DBusVariant('q', mtu_hint)}],
        )
        reply = await bus.call(msg)
        if reply is None or reply.message_type != reply.message_type.METHOD_RETURN:
            return None, 0
        # body = [Handle (UNIX_FD index), MTU]
        fd_idx, mtu = reply.body
        if not reply.unix_fds:
            return None, 0
        fd = reply.unix_fds[0]
        sock = socket.socket(fileno=fd)
        # socket.socket(fileno=fd) duplicates ownership semantics — sock now owns fd
        return sock, int(mtu)
    except Exception as ex:
        print(f"  AcquireWrite failed ({ex}); falling back to D-Bus WriteValue")
        return None, 0

# Shared between status callback and main flow
_done_event: asyncio.Event = None  # set in do_ota
_last_state: int = 0
_last_error: int = 0
_saw_done: bool = False
_saw_error: bool = False
_saw_verifying: bool = False
_saw_receiving: bool = False


async def scan_list(name_prefix: str, timeout: float = 8.0):
    """Print every BLE advertiser whose name starts with name_prefix."""
    print(f"Scanning for '{name_prefix}*' advertisers ({timeout:.0f}s)...\n")
    seen = {}
    devices = await BleakScanner.discover(timeout=timeout)
    for d in devices:
        if d.name and d.name.startswith(name_prefix):
            seen[d.address] = d
    if not seen:
        print("  (none found \u2014 is a node in BLE OTA mode?)")
        return
    print(f"  {'NAME':<32}  {'ADDRESS':<17}  RSSI")
    print(f"  {'-'*32}  {'-'*17}  -----")
    for d in seen.values():
        rssi = getattr(d, 'rssi', '?')
        print(f"  {d.name:<32}  {d.address:<17}  {rssi} dBm")


async def scan_for_device(name_prefix: str, timeout: float = 15.0):
    """Scan for a BLE device whose name starts with name_prefix."""
    print(f"Scanning for BLE device matching '{name_prefix}' (up to {timeout:.0f}s)...")
    found = None
    deadline = asyncio.get_event_loop().time() + timeout
    while asyncio.get_event_loop().time() < deadline:
        devices = await BleakScanner.discover(timeout=2.0)
        for d in devices:
            if d.name and d.name.startswith(name_prefix):
                rssi = getattr(d, 'rssi', '?')
                print(f"  Found: {d.name}  [{d.address}]  RSSI: {rssi} dBm")
                found = d
                break
        if found:
            break
        remaining = deadline - asyncio.get_event_loop().time()
        print(f"  Not found yet — {remaining:.0f}s remaining...")
    return found


async def is_advertising_name_prefix(name_prefix: str, timeout: float = 6.0) -> bool:
    """Return True if any advertiser matches name_prefix within timeout."""
    devices = await BleakScanner.discover(timeout=timeout)
    for d in devices:
        if d.name and d.name.startswith(name_prefix):
            return True
    return False


async def do_ota(
    device_addr: str,
    firmware_path: str,
    expected_name_prefix: str,
    done_timeout: float,
    allow_timeout_success: bool,
    use_notifications: bool,
    data_write_response: bool,
    chunk_size: int,
    chunk_delay_s: float,
    periodic_pause_every: int,
    periodic_pause_s: float,
    flush_delay_s: float,
):
    fw_data = open(firmware_path, 'rb').read()
    fw_size = len(fw_data)
    print(f"\nFirmware: {firmware_path}  ({fw_size:,} bytes)")

    global _done_event, _last_state, _last_error
    global _saw_done, _saw_error, _saw_verifying, _saw_receiving
    _done_event = asyncio.Event()
    _last_state = 0
    _last_error = 0
    _saw_done = False
    _saw_error = False
    _saw_verifying = False
    _saw_receiving = False

    def on_status(sender, data: bytearray):
        global _last_state, _last_error
        global _saw_done, _saw_error, _saw_verifying, _saw_receiving
        state_id = data[0] if len(data) > 0 else 0
        progress = data[1] if len(data) > 1 else 0
        err = data[2] if len(data) > 2 else 0
        state_name = OTA_STATE.get(state_id, f"UNKNOWN({state_id})")
        _last_state = state_id
        _last_error = err
        if state_id == 3:
            _saw_receiving = True
        elif state_id == 4:
            _saw_verifying = True
        elif state_id == 5:
            _saw_done = True
        elif state_id == 255:
            _saw_error = True
        print(f"  [STATUS] {state_name}  {progress}%  err={err}")
        if state_id in (5, 255):  # COMPLETE or ERROR
            if _done_event and not _done_event.is_set():
                _done_event.set()

    print(f"Connecting to {device_addr}...")
    async with BleakClient(device_addr, timeout=15.0) as client:
        if not client.is_connected:
            print("ERROR: Failed to connect.")
            return False

        notify_active = False
        if use_notifications:
            print("Connected. Subscribing to status notifications...")
            await client.start_notify(STATUS_UUID, on_status)
            notify_active = True
        else:
            print("Connected. Notifications disabled; using status polling.")

        # Probe server-side offset. If > 0, server is mid-session (held across
        # a prior disconnect) and we RESUME instead of erasing & restarting.
        server_offset = 0
        try:
            raw = await client.read_gatt_char(OFFSET_UUID)
            if len(raw) >= 4:
                server_offset = int.from_bytes(raw[:4], 'little')
        except Exception as ex:
            print(f"  NOTE: OFFSET char unreadable ({ex}); assuming fresh session")
            server_offset = 0

        if server_offset > 0 and server_offset < fw_size:
            print(f"Server reports offset={server_offset:,} — sending RESUME...")
            payload = bytes([CTRL_RESUME]) + server_offset.to_bytes(4, 'little')
            await client.write_gatt_char(CTRL_UUID, payload, response=True)
            await asyncio.sleep(0.1)
            sent = server_offset
        else:
            # Send START
            print("Sending START command...")
            await client.write_gatt_char(CTRL_UUID, CTRL_START, response=True)
            await asyncio.sleep(0.1)
            sent = 0

        # Push firmware in chunks
        chunk_count = 0
        total_chunks = (fw_size + chunk_size - 1) // chunk_size
        if sent > 0:
            print(f"Resuming at {sent:,}/{fw_size:,} bytes ({sent*100//fw_size}%)...")
        else:
            print(f"Sending {fw_size:,} bytes in {total_chunks} chunks of {chunk_size}B...")

        # Flow-control window when using write-without-response. Server's
        # OFFSET char reports bytes actually committed to flash; if we run
        # too far ahead, the BLE controller's RX queue overflows and chunks
        # are silently dropped (we observed 2.4 MB "sent" but only 50 KB
        # received). Keep client no more than WINDOW bytes ahead of server.
        FC_WINDOW = 65536   # 64 KB outstanding cap
        FC_POLL_CHUNKS = 64 # check server offset every 64 chunks
        FC_MAX_WAIT_S = 8.0 # bail to outer reconnect loop if stuck

        # Try to open a raw L2CAP socket for write-without-response.
        write_sock = None
        write_sock_mtu = 0
        if not data_write_response:
            write_sock, write_sock_mtu = await acquire_write_socket(
                client, DATA_UUID, mtu_hint=chunk_size + 3
            )
            if write_sock is not None:
                print(f"  Fast path: AcquireWrite socket (mtu={write_sock_mtu})")
                # AcquireWrite negotiates an effective MTU. Outgoing packets
                # must be <= that mtu — drop chunk_size if necessary.
                if chunk_size > write_sock_mtu - 3 and write_sock_mtu > 23:
                    chunk_size = write_sock_mtu - 3
                    print(f"  Adjusted chunk_size to {chunk_size} for socket MTU")

        loop = asyncio.get_running_loop()

        while sent < fw_size:
            chunk = fw_data[sent:sent + chunk_size]
            if write_sock is not None:
                # Async write via run_in_executor to avoid blocking the loop
                # on rare socket back-pressure. SOCK_SEQPACKET preserves chunk
                # boundaries; kernel returns when packet is queued to controller.
                try:
                    await loop.run_in_executor(None, write_sock.send, chunk)
                except BlockingIOError:
                    await asyncio.sleep(0.005)
                    continue
                except OSError as ex:
                    print(f"  socket send failed ({ex}); reverting to D-Bus")
                    try:
                        write_sock.close()
                    except Exception:
                        pass
                    write_sock = None
                    continue
            else:
                await client.write_gatt_char(DATA_UUID, chunk, response=data_write_response)
            sent += len(chunk)
            chunk_count += 1

            # Progress bar every 10 chunks
            if chunk_count % 10 == 0 or sent >= fw_size:
                pct = sent * 100 // fw_size
                bar = '█' * (pct // 5) + '░' * (20 - pct // 5)
                print(f"\r  [{bar}] {pct:3d}%  {sent:,}/{fw_size:,} B", end='', flush=True)

            # Conservative pacing: some nodes drop chunks if host writes too fast.
            if chunk_delay_s > 0:
                await asyncio.sleep(chunk_delay_s)
            if periodic_pause_every > 0 and (chunk_count % periodic_pause_every) == 0:
                await asyncio.sleep(periodic_pause_s)

            # Server-paced flow control (active only when response=False).
            if (not data_write_response) and (chunk_count % FC_POLL_CHUNKS) == 0:
                waited = 0.0
                while True:
                    try:
                        raw = await client.read_gatt_char(OFFSET_UUID)
                    except Exception:
                        break  # disconnected — let outer loop retry
                    srv = int.from_bytes(raw[:4], 'little') if len(raw) >= 4 else 0
                    if sent - srv <= FC_WINDOW:
                        break
                    if waited >= FC_MAX_WAIT_S:
                        # Server fell behind; resync our notion of `sent` to
                        # what the server actually committed and continue.
                        print(f"\n  FC: server stuck at {srv:,} (we sent {sent:,}); resyncing.")
                        sent = srv
                        break
                    await asyncio.sleep(0.05)
                    waited += 0.05

        print()  # newline after progress bar

        # Close fast-path socket before END so BlueZ flushes pending writes
        if write_sock is not None:
            try:
                write_sock.close()
            except Exception:
                pass
            write_sock = None

        if flush_delay_s > 0:
            print(f"Waiting {flush_delay_s:.2f}s for peripheral write queue flush...")
            await asyncio.sleep(flush_delay_s)

        # Send END
        print("Sending END command...")
        try:
            await client.write_gatt_char(CTRL_UUID, CTRL_END, response=True)
        except BleakGATTProtocolError as ex:
            # Some nodes may flip to COMPLETE/reboot quickly and reject END.
            # Keep going; terminal status / timeout logic decides PASS/FAIL.
            print(f"  WARN: END write returned GATT error: {ex}")

        # Wait for STATUS_DONE/ERROR (notify-driven) or timeout
        print(f"Waiting for verification and reboot (timeout={done_timeout:.0f}s)...")
        if notify_active:
            try:
                await asyncio.wait_for(_done_event.wait(), timeout=done_timeout)
            except asyncio.TimeoutError:
                print("  WARN: No terminal status notification before timeout.")
                try:
                    raw = await client.read_gatt_char(STATUS_UUID)
                    if len(raw) >= 1:
                        _last_state = raw[0]
                        _last_error = raw[2] if len(raw) > 2 else 0
                        print(f"  Final status read: {OTA_STATE.get(_last_state, _last_state)} err={_last_error}")
                        if _last_state == 5:
                            _saw_done = True
                        elif _last_state == 255:
                            _saw_error = True
                except Exception as ex:
                    print(f"  Final status read unavailable: {ex}")

            try:
                await client.stop_notify(STATUS_UUID)
            except Exception:
                pass  # already disconnected after reboot
        else:
            deadline = asyncio.get_event_loop().time() + done_timeout
            while asyncio.get_event_loop().time() < deadline:
                try:
                    raw = await client.read_gatt_char(STATUS_UUID)
                    if len(raw) >= 1:
                        _last_state = raw[0]
                        _last_error = raw[2] if len(raw) > 2 else 0
                        if _last_state == 3:
                            _saw_receiving = True
                        elif _last_state == 4:
                            _saw_verifying = True
                        elif _last_state == 5:
                            _saw_done = True
                            break
                        elif _last_state == 255:
                            _saw_error = True
                            break
                except Exception:
                    # If device reboots and disconnects before final read, exit loop.
                    break
                await asyncio.sleep(0.5)

    if _saw_error or _last_state == 255:
        print(f"\nOTA FAILED \u2014 device reported ERROR (err={_last_error}).")
        return False

    if _saw_done:
        print("\nRESULT: PASS (DONE received)")
        return True

    if allow_timeout_success:
        still_advertising = await is_advertising_name_prefix(expected_name_prefix, timeout=8.0)
        if _saw_receiving and _saw_verifying and not still_advertising:
            print("\nRESULT: PASS (no DONE notify, but OTA advertiser disappeared after verify)")
            return True

    print("\nRESULT: FAIL (no DONE notification).")
    return False


async def main():
    parser = argparse.ArgumentParser(description="OpenDash BLE OTA Client")
    parser.add_argument("firmware", nargs='?', default=None,
                        help="Path to .bin firmware file (omit with --list)")
    parser.add_argument("--node", choices=sorted(NODE_TO_DEVICE_PREFIX.keys()), default=None,
                        help="Convenience target node (sets default --device prefix)")
    parser.add_argument("--device", "-d", default="OpenDash",
                        help="BLE device name prefix to scan for (default: 'OpenDash')")
    parser.add_argument("--address", "-a", default=None,
                        help="BLE MAC address (skip scan, connect directly)")
    parser.add_argument("--scan-timeout", type=float, default=20.0,
                        help="How long to scan for device (default: 20s)")
    parser.add_argument("--done-timeout", type=float, default=45.0,
                        help="Seconds to wait for DONE/ERROR after END (default: 45s)")
    parser.add_argument("--connect-retries", type=int, default=4,
                        help="Retries for BLE connect/service discovery before failing (default: 4)")
    parser.add_argument("--chunk-size", type=int, default=384,
                        help="BLE data chunk size in bytes (default: 384)")
    parser.add_argument("--chunk-delay-ms", type=float, default=1.2,
                        help="Delay between data chunks in milliseconds (default: 1.2)")
    parser.add_argument("--pause-every", type=int, default=24,
                        help="Insert a longer pause every N chunks (default: 24)")
    parser.add_argument("--pause-ms", type=float, default=10.0,
                        help="Pause duration in milliseconds at each pause interval (default: 10)")
    parser.add_argument("--flush-delay-ms", type=float, default=1600.0,
                        help="Delay before END command so peripheral drains queued writes (default: 1600)")
    parser.add_argument("--allow-timeout-success", action='store_true',
                        help="Fallback pass when DONE is missed but device exits OTA advertising")
    parser.add_argument("--no-notify", action='store_true',
                        help="Disable STATUS notify subscription and poll status instead")
    parser.add_argument("--no-response", action='store_true',
                        help="Use write-without-response for data chunks (faster, less reliable)")
    parser.add_argument("--list", "-l", action='store_true',
                        help="Scan and list visible OpenDash BLE OTA advertisers, then exit")
    args = parser.parse_args()

    if args.list:
        await scan_list(args.device, timeout=min(args.scan_timeout, 10.0))
        sys.exit(0)

    if args.node is not None and args.device == "OpenDash":
        args.device = NODE_TO_DEVICE_PREFIX[args.node]
        print(f"Using node target '{args.node}' -> device prefix '{args.device}'")

    if not args.firmware:
        parser.error("firmware path required unless --list is used")

    if not os.path.isfile(args.firmware):
        print(f"ERROR: Firmware file not found: {args.firmware}")
        sys.exit(1)

    if args.address:
        addr = args.address
        print(f"Using provided address: {addr}")
    else:
        device = await scan_for_device(args.device, timeout=args.scan_timeout)
        if not device:
            print(f"\nERROR: No device found matching '{args.device}'")
            print("Make sure the node is in BLE OTA mode (Config screen → OTA Flash).")
            sys.exit(1)
        addr = device.address

    success = False
    attempts = max(1, args.connect_retries)
    for attempt in range(1, attempts + 1):
        if attempt > 1:
            print(f"\nRetrying BLE session ({attempt}/{attempts})...")
            await asyncio.sleep(0.5)
        try:
            success = await do_ota(
                addr,
                args.firmware,
                expected_name_prefix=args.device,
                done_timeout=args.done_timeout,
                allow_timeout_success=args.allow_timeout_success,
                use_notifications=(not args.no_notify),
                data_write_response=(not args.no_response),
                chunk_size=max(64, min(512, int(args.chunk_size))),
                chunk_delay_s=max(0.0, float(args.chunk_delay_ms) / 1000.0),
                periodic_pause_every=max(0, int(args.pause_every)),
                periodic_pause_s=max(0.0, float(args.pause_ms) / 1000.0),
                flush_delay_s=max(0.0, float(args.flush_delay_ms) / 1000.0),
            )
            if success:
                break
        except BleakError as ex:
            print(f"WARN: BLE connect/discovery failed on attempt {attempt}/{attempts}: {ex}")
            continue
        except Exception as ex:
            print(f"WARN: OTA attempt {attempt}/{attempts} failed with exception: {ex}")
            continue

    sys.exit(0 if success else 1)


if __name__ == "__main__":
    asyncio.run(main())
