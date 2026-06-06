#!/usr/bin/env python3
# Licensed under Sovereign Individual License v1.0 — see LICENSE file
"""
OpenDash safe-flash gate.

Refuses to flash unless the target serial device matches the requested
node, verified by BOTH:

  1. /dev/serial/by-id/ symlink containing a known unique USB-serial
     substring for that physical board (CH340 serial# or ESP USB-JTAG MAC).

  2. Live tag probe of the running firmware (DTR low, RTS reset pulse,
     read until a known tag appears or ~13s elapses — long enough to catch
     the ~10s MOS/relay heartbeat;
     look for "opendash_<node>" log tag). Skipped if device is in
     bootloader / unresponsive (allowed for first-flash recovery, but
     a confirmation is required).

Usage:
    od-flash.py <node>                    # build + flash + monitor
    od-flash.py <node> --no-monitor       # build + flash
    od-flash.py <node> --probe-only       # just print which port it is
    od-flash.py --probe-all               # tag-sniff every ACM/USB port

Nodes: center | left | right | gps | pod1 | pod2
       mos-4ch-a | mos-4ch-b | relay-4ch-hd | relay-8ch-a | relay-8ch-b

SHARED FTDI FLASHER: every relay/MOS node is flashed individually through
the single FTDI FT232R on /dev/ttyUSB1 (swap the board, flash, repeat).
They therefore all resolve to the SAME port; the running-firmware TAG probe
is what tells you WHICH board is currently attached. A brand-new (never
flashed) board is silent and will need --force on its first flash.

Why: We have repeatedly flashed the wrong device (e.g. center FW to
LEFT pod) by trusting /dev/ttyACMn numbering, which is NOT stable
across reboots/replug. This wrapper makes that mistake impossible
without --force.
"""
import argparse
import glob
import os
import re
import subprocess
import sys
import time

# ---------------------------------------------------------------------------
# Per-node identity registry. Update when hardware is swapped.
# `serial_substr` matches against /dev/serial/by-id/* symlink names.
# `tag` is the ESP_LOGI tag the running firmware emits (defense in depth).
# `project_dir` is relative to repo root.
# ---------------------------------------------------------------------------
NODES = {
    "center": {
        "serial_substr": "1a86_USB_Single_Serial_58FA100070",  # CH340
        "tag":           "opendash_center",
        "project_dir":   "center",
    },
    "left": {
        "serial_substr": "1a86_USB_Single_Serial_5972050030",  # CH340
        "tag":           "opendash_left",
        "project_dir":   "left",
    },
    "right": {
        "serial_substr": "1a86_USB_Single_Serial_5AE7114735",  # CH340
        "tag":           "opendash_right",
        "project_dir":   "right",
    },
    "gps": {
        # Espressif USB-JTAG, MAC-based. Update if board swapped.
        "serial_substr": "10:20:BA:46:61:FC",
        "tag":           None,  # GPS node logs as display_init early
        "project_dir":   "gps",
    },
    "pod1": {
        "serial_substr": "1C:DB:D4:7B:5F:88",
        "tag":           "opendash_pod1",
        "project_dir":   "pod1",
    },
    "pod2": {
        "serial_substr": "1C:DB:D4:7B:61:68",
        "tag":           "opendash_pod2",
        "project_dir":   "pod2",
    },

    # ── FTDI-shared nodes (flashed one at a time through the FT232R on USB1) ──
    # These all resolve to the SAME port. The TAG probe disambiguates which
    # board is physically attached. `shared_ftdi` relaxes the resolver and
    # makes the messaging explicit.
    "mos-4ch-a": {
        "serial_substr": "FTDI_FT232R_USB_UART",
        "tag":           "mos_4ch_a",
        "project_dir":   "mos-4ch-a",
        "shared_ftdi":   True,
    },
    "mos-4ch-b": {
        "serial_substr": "FTDI_FT232R_USB_UART",
        "tag":           "mos_4ch_b",
        "project_dir":   "mos-4ch-b",
        "shared_ftdi":   True,
    },
    "relay-4ch-hd": {
        "serial_substr": "FTDI_FT232R_USB_UART",
        "tag":           "relay_4ch_hd",
        "project_dir":   "relay-4ch-hd",
        "shared_ftdi":   True,
    },
    "relay-8ch-a": {
        "serial_substr": "FTDI_FT232R_USB_UART",
        "tag":           "relay_8ch_a",
        "project_dir":   "relay-8ch-a",
        "shared_ftdi":   True,
    },
    "relay-8ch-b": {
        "serial_substr": "FTDI_FT232R_USB_UART",
        "tag":           "relay_8ch_b",
        "project_dir":   "relay-8ch-b",
        "shared_ftdi":   True,
    },
}

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Any known firmware log tag (opendash_*, mos_*, relay_*, BMS marker).
_KNOWN_TAGS = sorted({n["tag"] for n in NODES.values() if n["tag"]})
TAG_RE = re.compile(
    r"\b(" + "|".join(re.escape(t) for t in _KNOWN_TAGS)
    + r"|opendash_[a-z0-9]+)\b|(\[BMS\])"
)


def tags_in(text):
    """Return the set of known firmware tags seen in probe text."""
    found = set()
    for a, b in TAG_RE.findall(text):
        if a:
            found.add(a)
        if b:
            found.add(b)
    return sorted(found)


def resolve_port(node):
    """Return /dev/ttyACMn for node, or None."""
    sub = NODES[node]["serial_substr"]
    by_id = "/dev/serial/by-id"
    if not os.path.isdir(by_id):
        return None
    for name in os.listdir(by_id):
        if sub in name:
            return os.path.realpath(os.path.join(by_id, name))
    return None


def probe_tag(port, timeout=13.0):
    """Reset the device and read serial; return concatenated decoded text.

    DTR is held low (IO0 high) for the whole probe so an FTDI board never
    drops into download mode (which would leave it silent). RTS is pulsed to
    attempt a reset, but several of the shared-FTDI relay/MOS boards do not
    wire RTS->EN in a way these pulses trigger, so we cannot rely on catching
    the boot banner. Those nodes emit a periodic status/uptime heartbeat
    (~10 s on MOS), so the read window must be long enough to catch one.
    The loop early-exits the moment any known firmware tag appears, keeping
    continuously-logging nodes (center/left/right/gps) fast.
    """
    try:
        import serial  # pyserial
    except ImportError:
        print("WARN: pyserial not installed; skipping tag probe", file=sys.stderr)
        return ""
    try:
        s = serial.Serial(port, 115200, timeout=0.3)
        # Keep IO0 high (normal boot, never download mode); pulse EN via RTS.
        s.dtr = False
        s.rts = False
        time.sleep(0.05)
        s.rts = True
        time.sleep(0.1)
        s.rts = False
        end = time.time() + timeout
        buf = b""
        while time.time() < end:
            buf += s.read(2048)
            # Early-exit as soon as a recognizable tag shows up.
            if tags_in(buf.decode("utf-8", "replace")):
                break
        s.close()
        return buf.decode("utf-8", "replace")
    except Exception as e:
        print(f"WARN: probe of {port} failed: {e}", file=sys.stderr)
        return ""


def confirm_or_die(node, port, force):
    """Hard gate before idf.py flash."""
    expected = NODES[node]
    expected_tag = expected["tag"]

    if not port:
        print(f"ERROR: no /dev/serial/by-id/* matched substring "
              f"'{expected['serial_substr']}' for node '{node}'.")
        print("       Is the device plugged in? Run with --probe-all.")
        sys.exit(2)

    print(f"[od-flash] node={node}  port={port}")
    print(f"[od-flash]   expected tag: {expected_tag}")

    text = probe_tag(port)
    if expected_tag and expected_tag in text:
        print(f"[od-flash]   tag probe: MATCH ({expected_tag} found)")
        return
    if not text.strip():
        print("[od-flash]   tag probe: device silent (bootloader or hung)")
        if expected.get("shared_ftdi"):
            print("[od-flash]   (shared FTDI: a fresh/unflashed board is silent — "
                  "use --force for its first flash)")
    else:
        # Find what tag IS running so the user knows what they would clobber.
        found = tags_in(text)
        print(f"[od-flash]   tag probe: did NOT see '{expected_tag}'. "
              f"Saw: {found if found else '(no known firmware tag)'}")

    if force:
        print("[od-flash]   --force given; proceeding anyway")
        return

    print()
    print("REFUSING TO FLASH — running firmware does not advertise the expected tag.")
    print("If you're sure (e.g. first flash, or recovering a bricked unit),")
    print("re-run with --force.")
    sys.exit(3)


def run_idf(node, action, force, monitor):
    project = os.path.join(REPO_ROOT, NODES[node]["project_dir"])
    if not os.path.isdir(project):
        print(f"ERROR: project dir not found: {project}")
        sys.exit(2)

    port = resolve_port(node)
    confirm_or_die(node, port, force)

    cmd = ["idf.py", "-p", port, action]
    if monitor:
        cmd.append("monitor")
    print(f"[od-flash] cd {project}")
    print(f"[od-flash] $ {' '.join(cmd)}")
    sys.exit(subprocess.call(cmd, cwd=project))


def probe_all():
    print("Probing all serial ports for opendash_* tags...")
    ports = sorted(glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*"))
    for p in ports:
        # Active boards early-exit fast; only genuinely silent ports wait the
        # full window (long enough to catch a ~10 s MOS/relay heartbeat even if
        # the RTS pulse restarted the board's uptime clock).
        text = probe_tag(p, timeout=13.0)
        tags = tags_in(text)
        print(f"  {p:18s} -> {tags if tags else '(silent / unknown)'}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("node", nargs="?", choices=list(NODES.keys()))
    ap.add_argument("--probe-only", action="store_true",
                    help="resolve port + probe tag, do not flash")
    ap.add_argument("--probe-all", action="store_true",
                    help="probe every serial port, identify each")
    ap.add_argument("--no-monitor", action="store_true")
    ap.add_argument("--build-only", action="store_true")
    ap.add_argument("--force", action="store_true",
                    help="bypass tag-mismatch refusal (dangerous)")
    args = ap.parse_args()

    if args.probe_all:
        probe_all()
        return
    if not args.node:
        ap.error("node is required (or use --probe-all)")

    if args.build_only:
        project = os.path.join(REPO_ROOT, NODES[args.node]["project_dir"])
        sys.exit(subprocess.call(["idf.py", "build"], cwd=project))

    if args.probe_only:
        port = resolve_port(args.node)
        confirm_or_die(args.node, port, force=False)
        return

    run_idf(args.node, "flash", args.force, monitor=not args.no_monitor)


if __name__ == "__main__":
    main()
