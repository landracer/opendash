#!/usr/bin/env python3
# Licensed under Sovereign Individual License v1.0 — see LICENSE file
"""
OpenDash Node Serial Monitor — resets via DTR+RTS, captures boot + runtime output.
Usage: python3 monitor_node.py [port] [baud] [seconds]
  Defaults: /dev/ttyUSB0  115200  60
"""
import sys, serial, time, signal

port    = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB0"
baud    = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
timeout = int(sys.argv[3]) if len(sys.argv) > 3 else 60

print(f"[monitor] Opening {port} @ {baud} for {timeout}s")

s = serial.Serial(port, baud, timeout=0.5)

# Hard-reset the ESP32 via DTR/RTS toggle (FTDI convention)
s.setDTR(False)
s.setRTS(True)
time.sleep(0.1)
s.setRTS(False)
s.setDTR(True)
time.sleep(0.1)
s.setDTR(False)
print(f"[monitor] Reset pulse sent — capturing output...\n")

start = time.time()
try:
    while (time.time() - start) < timeout:
        data = s.read(1024)
        if data:
            text = data.decode("utf-8", errors="replace")
            print(text, end="", flush=True)
except KeyboardInterrupt:
    print("\n[monitor] Interrupted by user")
finally:
    s.close()
    elapsed = time.time() - start
    print(f"\n[monitor] Done — captured {elapsed:.1f}s of output from {port}")
