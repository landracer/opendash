#!/usr/bin/env python3
# Licensed under Sovereign Individual License v1.0 — see LICENSE file
"""
GPS Continuity Test — v16h
Resets the GPS ESP32, monitors serial for time-to-first-fix (TTFF),
then repeats N times. Reports pass/fail per run and summary.

Usage: python3 continuity_test.py [--runs 10] [--port /dev/ttyACM1] [--timeout 120]
"""

import serial
import time
import sys
import argparse
import re


def reset_device(port):
    """Hard reset ESP32 via RTS toggle. Retries if port disappears."""
    for attempt in range(5):
        try:
            s = serial.Serial(port, 115200)
            s.setDTR(False)
            s.setRTS(True)
            time.sleep(0.1)
            s.setRTS(False)
            time.sleep(0.1)
            s.close()
            time.sleep(0.5)  # Let it start booting
            return True
        except serial.SerialException:
            print(f"  Port {port} not available (attempt {attempt+1}/5), waiting 5s...")
            time.sleep(5)
    return False


def wait_for_fix(port, timeout_s):
    """
    Monitor serial output until we see a 3D FIX line.
    Returns (ttff_seconds, sats_used, hdop) or (None, 0, 0) on timeout.
    """
    try:
        s = serial.Serial(port, 115200, timeout=0.5)
    except serial.SerialException:
        return None, 0, 0.0
    start = time.time()
    first_data_time = None
    saw_recovery = False
    saw_10hz = False
    saw_watchdog = False

    while time.time() - start < timeout_s:
        try:
            line = s.readline().decode('utf-8', errors='replace').strip()
        except Exception:
            continue

        if not line:
            continue

        elapsed = time.time() - start

        # Track key events
        if 'FULL RECOVERY' in line and 'COMPLETE' not in line:
            saw_recovery = True
            print(f"  [{elapsed:5.1f}s] RECOVERY triggered")

        if 'First data' in line:
            first_data_time = elapsed
            print(f"  [{elapsed:5.1f}s] First data received")

        if '10 Hz' in line:
            saw_10hz = True
            print(f"  [{elapsed:5.1f}s] 10Hz configured")

        if 'WATCHDOG' in line or 'watchdog' in line:
            saw_watchdog = True
            print(f"  [{elapsed:5.1f}s] {line[-80:]}")

        # Check for fix status line
        # Pattern: GPS: fix=YES sats=X/Y speed=... hdop=...
        m = re.search(r'GPS: fix=(YES|no) sats=(\d+)/(\d+) speed=[\d.]+ hdop=([\d.]+)', line)
        if m:
            fix_status = m.group(1)
            sats_used = int(m.group(2))
            sats_visible = int(m.group(3))
            hdop = float(m.group(4))

            if fix_status == 'YES':
                ttff = elapsed
                s.close()
                print(f"  [{elapsed:5.1f}s] 3D FIX! sats={sats_used}/{sats_visible} HDOP={hdop}")
                return ttff, sats_used, hdop

        # Also check Uptime lines for fix status
        um = re.search(r'Uptime: (\d+)s \| GPS: (3D FIX|NO FIX)', line)
        if um:
            uptime = int(um.group(1))
            fix = um.group(2)
            if fix == '3D FIX' and first_data_time is None:
                # Fix from Uptime line (less precise but still counts)
                ttff = elapsed
                s.close()
                print(f"  [{elapsed:5.1f}s] 3D FIX (from Uptime: {uptime}s)")
                return ttff, 0, 0.0

    s.close()
    return None, 0, 0.0


def main():
    parser = argparse.ArgumentParser(description="GPS Continuity Test")
    parser.add_argument('--runs', type=int, default=10, help='Number of restart cycles')
    parser.add_argument('--port', default='/dev/ttyACM1', help='Serial port')
    parser.add_argument('--timeout', type=int, default=120, help='Max seconds per run')
    args = parser.parse_args()

    print(f"═══════════════════════════════════════════════════════════")
    print(f"  GPS Continuity Test — {args.runs} runs, {args.timeout}s timeout")
    print(f"  Port: {args.port}")
    print(f"═══════════════════════════════════════════════════════════")

    results = []

    for run in range(1, args.runs + 1):
        print(f"\n{'─' * 50}")
        print(f"  Run {run}/{args.runs} — Resetting device...")
        print(f"{'─' * 50}")

        reset_ok = reset_device(args.port)
        if not reset_ok:
            results.append({'run': run, 'ttff': None, 'sats': 0, 'hdop': 0, 'status': 'CRASH'})
            print(f"  >>> Run {run}: CRASH — Device disappeared from USB")
            continue

        ttff, sats, hdop = wait_for_fix(args.port, args.timeout)

        if ttff is not None:
            status = "PASS" if ttff < 90 else "SLOW"
            results.append({'run': run, 'ttff': ttff, 'sats': sats, 'hdop': hdop, 'status': status})
            print(f"  >>> Run {run}: {status} — TTFF={ttff:.1f}s sats={sats} HDOP={hdop}")
        else:
            results.append({'run': run, 'ttff': None, 'sats': 0, 'hdop': 0, 'status': 'FAIL'})
            print(f"  >>> Run {run}: FAIL — No fix in {args.timeout}s")

        # Brief pause between runs
        if run < args.runs:
            time.sleep(2)

    # Summary
    print(f"\n{'═' * 60}")
    print(f"  SUMMARY — {args.runs} runs")
    print(f"{'═' * 60}")
    print(f"  {'Run':>4} {'Status':>6} {'TTFF':>8} {'Sats':>5} {'HDOP':>6}")
    print(f"  {'─'*4} {'─'*6} {'─'*8} {'─'*5} {'─'*6}")

    passes = 0
    ttffs = []
    for r in results:
        ttff_str = f"{r['ttff']:.1f}s" if r['ttff'] is not None else "N/A"
        print(f"  {r['run']:>4} {r['status']:>6} {ttff_str:>8} {r['sats']:>5} {r['hdop']:>6.1f}")
        if r['status'] in ('PASS', 'SLOW'):
            passes += 1
            ttffs.append(r['ttff'])

    print(f"  {'─'*4} {'─'*6} {'─'*8} {'─'*5} {'─'*6}")
    if ttffs:
        avg_ttff = sum(ttffs) / len(ttffs)
        min_ttff = min(ttffs)
        max_ttff = max(ttffs)
        print(f"  Pass rate: {passes}/{args.runs} ({100*passes/args.runs:.0f}%)")
        print(f"  TTFF avg: {avg_ttff:.1f}s  min: {min_ttff:.1f}s  max: {max_ttff:.1f}s")
    else:
        print(f"  Pass rate: 0/{args.runs} (0%)")

    success = passes == args.runs and all(t < 90 for t in ttffs)
    print(f"\n  {'✅ ALL RUNS PASSED' if success else '❌ NOT ALL RUNS PASSED'}")
    print(f"{'═' * 60}")

    return 0 if success else 1


if __name__ == '__main__':
    sys.exit(main())
