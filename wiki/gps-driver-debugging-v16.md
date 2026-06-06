<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# GPS Driver Debugging Journal — v16 Series (v16a through v16k)

> **Purpose:** Complete documentation of the GPS driver rewrite from v15L2 baseline
> to v16k final. This is the definitive debugging reference for anyone working on
> the LC76G I2C driver or debugging similar shared-bus I2C issues on ESP32-S3.
>
> **Duration:** Multi-session deep debugging, 11 firmware iterations
> **Result:** v16k — 20/20 continuity test passes, zero crashes, zero failures
> **Author:** OpenDash Project, July 2026

---

## Table of Contents

1. [Background — Why v16 Was Needed](#1-background--why-v16-was-needed)
2. [The Five Root Causes](#2-the-five-root-causes)
3. [Iteration-by-Iteration Log](#3-iteration-by-iteration-log)
4. [Failed Experiments (Don't Repeat These)](#4-failed-experiments-dont-repeat-these)
5. [The Continuity Test — Why "Works Once" Isn't Enough](#5-the-continuity-test--why-works-once-isnt-enough)
6. [Final v16k Driver Architecture](#6-final-v16k-driver-architecture)
7. [Key Discoveries and Insights](#7-key-discoveries-and-insights)
8. [Debugging Methodology That Worked](#8-debugging-methodology-that-worked)
9. [Quick Reference — What Every Setting Does](#9-quick-reference--what-every-setting-does)

---

## 1. Background — Why v16 Was Needed

### The Starting Point: v15L2 (March 2025)

The v15L2 driver was the first production-verified GPS driver. It achieved:
- 3D fix with 9-16 satellites
- HDOP 0.7-1.1
- 62-95KB data output over 5 minutes
- Documented as "working" in `wiki/LC76G-I2C-GPS-Driver-Guide.md` v2.0.0

### What Went Wrong

After v15L2, the GPS unit started exhibiting **intermittent failures**:
- Fix acquisition was inconsistent — sometimes 45 seconds, sometimes never
- The unit would occasionally hard-crash (blank screen, USB disconnect)
- 30+ satellites would be visible but no position fix achieved
- Recovery from power cycle wasn't reliable

These failures were not a single bug — they were **five independent root causes**
that interacted in complex ways, making each one harder to isolate.

### Why Not Just Revert?

The v15L2 code was the foundation, but several factors had changed:
1. Touch task was enabled (new I2C bus contention source)
2. 10Hz was being sent at boot (v15L2 didn't have 10Hz support)
3. The CW WAKE mechanism had accumulated ~88+ writes per session
4. PMIC register reads were unguarded (worked fine in v15L2 because
   there was less bus contention without the touch task)

Simply reverting to v15L2 wouldn't fix the underlying issues — it would
just hide them until the touch task or 10Hz was re-enabled.

---

## 2. The Five Root Causes

Each root cause was independent but interacted with the others.

### 2.1 CW Config Write Corruption

**Symptom:** After ~88 cumulative CW writes to 0x50, all subsequent writes NACK.
GPS stops outputting NMEA data entirely.

**Root Cause:** The CW config write (`{0x00, 0x10, 0x53, 0xAA, 0x00, 0x01, 0x00, 0x00}`)
was designed to configure the LC76G's internal buffer routing. It was being sent:
- After every successful data read (per-read WAKE)
- Every 5 empty polls (empty-poll WAKE)
- On every recovery event

The LC76G's CASIC command parser has a finite tolerance for CW writes. After
approximately 88 writes (observed over multiple sessions), the parser enters
a corrupted state where it NACKs all I2C writes to 0x50. Since 0x50 is the
only command endpoint, the module becomes unreachable — no queries, no
configuration commands, no data requests.

**Fix:** Replace all CW writes with 0x58-only bus activity. The 0x58 address is
the "data write" endpoint, and a simple dummy write (`{0x00}`) to it provides
sufficient I2C bus activity to keep the LC76G's slave interface alive without
touching the CASIC command parser.

**Discovery Version:** v16c

### 2.2 Touch I2C Contention

**Symptom:** Periodic I2C failures, bus lockups, hardware resets on shared bus.

**Root Cause:** The CST9217 capacitive touch controller (I2C 0x5A) was polled at
50Hz by `touch_read_task` in `display_init.c`. On the shared I2C bus (which also
hosts LC76G at 100kHz, QMI8658, AXP2101, TCA9554, PCF85063, ES8311, ES7210),
this created periodic contention. When a touch poll collided with a GPS I2C
transaction, ESP-IDF's I2C master driver would detect a bus conflict and
sometimes issue a hardware reset. If the reset happened mid-GPS-transaction,
the LC76G's I2C slave interface would lock up, requiring a full PMIC power
cycle to recover.

**Fix:** Disable `touch_read_task` entirely. The GPS unit is mounted in the
car and operates autonomously — there's no touch UI interaction needed.

**Discovery Version:** v16c (identified as contributing factor)

### 2.3 PMIC Board Crash

**Symptom:** Board goes completely dark (blank AMOLED, USB disconnect). Requires
physical power removal to recover. Occurred after ~15-20 minutes of GPS polling.

**Root Cause:** The AXP2101 PMIC register 0x90 controls all LDO power rails.
The GPS power cycle code reads this register, clears bits 2+3+5 (ALDO3, ALDO4,
BLDO2 — the GPS rails), and writes it back. During I2C bus contention, the
register read occasionally returned `0x00` instead of the actual value (~0x2C+).
The code then computed:

```c
ldo_enable = 0x00 & ~0x2C;   // = 0x00 (ALL rails off!)
// Writes 0x00 to register 0x90 → turns off ESP32 power → board dies
```

This turned off ALL LDO rails including the ones powering the ESP32 itself.

**Fix:** Validate the register read result before using it:
```c
if (ret != ESP_OK || ldo == 0x00) {
    ldo = 0xFF;  // Safe default — all rails ON
}
```

**Discovery Version:** v16j

### 2.4 10Hz Rate Overloading Navigation Engine

**Symptom:** LC76G sees 24-32 satellites but never achieves a position fix.
The `fix_valid` flag stays false indefinitely.

**Root Cause:** The `$PAIR050,100` command tells the LC76G to output NMEA at
10Hz (100ms intervals). When sent before the first fix, the navigation engine
spends most of its processing time formatting and outputting NMEA sentences
(10× more than at 1Hz default). This starves the position solution computation,
which needs significant CPU time to converge on an initial fix from cold or
warm ephemeris data.

In continuity testing, every run that failed had 10Hz sent at cycle ~23 (about
23 seconds after boot, before fix). Every run that succeeded achieved fix before
or shortly after the 10Hz command.

**Fix:** Introduce a `rate_configured` flag:
```c
static bool rate_configured = false;
// Only set 10Hz AFTER first fix
if (fix_valid && !rate_configured) {
    gps_send_nmea_command("$PAIR050,100");
    rate_configured = true;
}
```

Recovery paths reset `rate_configured = false` so 10Hz is re-sent after the
next fix following any recovery event.

**Discovery Version:** v16k

### 2.5 Recovery Threshold Too Low

**Symptom:** Cascading PMIC power cycles. GPS never achieves fix because
ephemeris data is wiped every 15 seconds.

**Root Cause:** The recovery threshold was set at 15 consecutive I2C failures.
On the shared bus, brief contention events (touch poll, IMU read, PMIC access)
can easily cause 15+ consecutive NACKs. Each PMIC power cycle:
1. Wipes the LC76G's RAM (ephemeris data gone)
2. Requires full cold-start TTFF (~60-90s) to re-download ephemeris from satellites
3. During TTFF, more bus contention → more failures → another power cycle

This created a cascading loop where the GPS could never accumulate enough
continuous uptime to download ephemeris and compute a fix.

**Fix:** Raised threshold from 15 → 50. Added 20-cycle post-recovery grace
period where failures are counted but don't trigger recovery.

**Discovery Version:** v16i

---

## 3. Iteration-by-Iteration Log

### v16 — First Attempt (FAILURE)
Over-aggressive changes from v15L2. Removed bus_reset, changed too many
parameters at once. Total failure — no data at all. Could not determine
which change broke it because too many things changed simultaneously.

**Lesson:** Change ONE thing at a time. If you change everything, you
learn nothing when it fails.

### v16b — Restore bus_reset at 10ms (FAILURE)
Restored bus_reset but used 10ms delay instead of v15L2's 50ms.
Still failed — data would start then stop.

**Lesson:** The 50ms bus_reset delay isn't arbitrary. The LC76G needs
the full clock pulse train recovery time.

### v16c — Restore v15L2 Timing + 0x58-Only WAKE (SUCCESS)
- Restored v15L2 timing (50ms bus_reset delay)
- Removed CW from all WAKE sequences → 0x58-only
- 3D fix achieved at 65 seconds
- First real success of the v16 series

**Key discovery:** CW writes were the primary failure cause. 0x58-only
WAKE is sufficient and doesn't corrupt the LC76G.

### v16d — Enable 10Hz + Fix Empty WAKE (PARTIAL)
- Added `$PAIR050,100` at first data receipt
- Fixed empty-poll WAKE (was CW, now 0x58)
- 3D fix at 50 seconds (faster!)
- BUT: buffer overflow observed after extended run (72KB+)

**Issue:** Buffer overflow wasn't a real problem — just LOG_BUFFER_SIZE
too small. But raised concern about data volume at 10Hz.

### v16e — Remove WAKE Entirely (FAILURE)
- Hypothesis: "WAKE isn't needed since we read every cycle"
- Removed all WAKE sequences
- Data stopped after ~15 cycles. Completely dead.

**Lesson:** WAKE IS REQUIRED. The LC76G's I2C slave enters idle mode
after inactivity. Even with per-cycle reads, the reads themselves go
through 0x50/0x54 — the 0x58 bus activity literally wakes a different
part of the I2C peripheral that keeps the data output pipeline active.

### v16f — WAKE at 50ms (BEST SO FAR)
- Restored 0x58-only WAKE with 50ms delay
- 3D fix, 12 satellites, HDOP 1.0
- 72KB+ data, 0 failures at cycle 300

**Milestone:** First long-duration stable run. But not tested for
consistency across restarts.

### v16g-h — Boot Activation Probes (FAILURE + REVERT)
- Added boot-time activation probes (various I2C sequences at startup)
- All failed because the WAKE stimulus wasn't present during boot
- Reverted to v16f base with first-data 10Hz trigger

**Lesson:** Boot-time probes don't help because the LC76G needs time
to power up and initialize its I2C slave interface. The first-data
trigger is the right approach.

### v16h — First Continuity Test (70% PASS)
- First systematic continuity testing using `gps/continuity_test.py`
- 10 runs: 7 PASS, 2 SLOW, 1 FAIL (some terminology evolved)
- Average TTFF: ~55s for passing runs
- Power-cycle recovery was destroying ephemeris

**Key discovery:** A single successful run means nothing. The GPS
MUST be tested across multiple consecutive restarts to verify
consistency. This is when continuity testing became mandatory.

### v16i — Recovery Tuning (80% PASS + HARD CRASH)
- Raised recovery threshold 15 → 50
- Implemented fix watchdog (48s/72s/96s warm/warm/cold)
- Added 20-cycle post-recovery grace period
- Reset `ever_received` after recovery
- 8/10 pass, but device HARD CRASHED during one run

**The crash:** Board went completely dark — blank AMOLED, USB
disconnected, unresponsive. Required physical power removal.
This was the PMIC crash (Root Cause #3, discovered in v16j).

### v16j — PMIC Crash Fix (80% PASS)
- Added PMIC register read validation (safe default 0xFF)
- Removed CW from recovery WAKE (was still present in recovery path)
- Fixed wall-clock watchdog (was using cycle count, now real time)
- 8/10 pass, 2/10 fail (24-32 sats visible but no fix)
- Zero crashes

**Remaining issue:** The 2 failures were the 10Hz overload problem.
Both failed runs had 10Hz sent before fix. Both successful runs
happened to achieve fix before 10Hz was sent.

### v16k — 10Hz Deferred to First Fix (20/20 PASS)
- Key insight: Runs that fail always have 10Hz sent before fix
- Deferred 10Hz to first `fix_valid == true`
- Constellation config still at first data (doesn't impact TTFF)
- `rate_configured` flag prevents repeated 10Hz sends
- Recovery paths reset the flag

**Final results:**
- Round 1: 10/10 PASS (avg 53.3s, min 32.8s, max 102.6s)
- Round 2: 10/10 PASS (avg 50.5s, min 29.2s, max 112.0s)
- **Combined: 20/20 passes, zero crashes, zero failures**

---

## 4. Failed Experiments (Don't Repeat These)

| # | What Was Tried | What Happened | Why It Failed |
|---|----------------|---------------|---------------|
| 1 | Remove bus_reset | Total data failure | LC76G needs clock pulse train recovery every cycle |
| 2 | 10ms bus_reset delay | Intermittent data | 50ms is the minimum for shared bus recovery |
| 3 | Remove WAKE entirely | Data stops after ~15 cycles | LC76G I2C slave goes idle without 0x58 stimulus |
| 4 | CW writes for WAKE | Corrupts parser after ~88 writes | CASIC command parser has finite CW tolerance |
| 5 | Boot activation probes | No improvement | LC76G not ready during boot; needs PMIC power cycle first |
| 6 | 200ms WAKE delay | Works but slow | 50ms is sufficient (v15L2 used 200ms, unnecessary) |
| 7 | Low recovery threshold (15) | Cascading power cycles | Bus contention causes 15+ NACKs easily |
| 8 | 10Hz before first fix | Prevents fix convergence | Nav engine overloaded with output processing |
| 9 | CW in recovery WAKE | Recovery itself corrupts module | Same CW issue as normal WAKE |

---

## 5. The Continuity Test — Why "Works Once" Isn't Enough

### The Problem With Single-Run Testing

Before v16h, GPS code was tested by:
1. Flash the firmware
2. Wait for GPS fix
3. See "fix valid" in the logs
4. Mark as "working"

This approach misses:
- **Intermittent failures** that happen 1 in 5 or 1 in 10 starts
- **Ephemeris state dependency** — first fix uses cached data, restarts wipe it
- **Bus contention race conditions** — timing varies between boots
- **Thermal effects** — I2C timing changes as the board warms up
- **Cascading failures** — recovery mechanisms that work once but fail on repeated recovery

### The Continuity Test Script

`gps/continuity_test.py` automates what a human tester would do:

```
For each run (default 10):
  1. Toggle RTS to hardware-reset the ESP32-S3
  2. Open serial port, start reading output
  3. Monitor for key patterns:
     - "lat=" with non-zero values → FIX ACQUIRED (record time)
     - "Recovery" → recovery event (record, may still pass)
     - "PQTM" → watchdog restart (record)
     - USB disconnect → retry port (handles brief glitches)
  4. If fix acquired within timeout: PASS
     If fix acquired but slow: SLOW (still a pass, but flagged)
     If timeout reached: FAIL
  5. Print summary table with all run results
```

### Usage

```bash
# Standard 10-run test
python3 gps/continuity_test.py --runs 10 --port /dev/ttyACM1 --timeout 120

# Extended 20-run test for release verification
python3 gps/continuity_test.py --runs 20 --port /dev/ttyACM1 --timeout 150
```

### Quality Gate

**ANY change to `gps_handler.c` or GPS-related I2C code MUST pass a minimum
10/10 continuity test before being merged or marked complete.**

"It works once" is NOT acceptable. The GPS driver operates on a shared I2C bus
with 8 other devices. Race conditions, bus contention, and timing dependencies
make single-run testing meaningless.

---

## 6. Final v16k Driver Architecture

### Polling Loop (runs continuously)

```
┌─────────────────────────────────┐
│         bus_reset (50ms)        │
│ i2c_master_bus_reset + delay    │
└───────────────┬─────────────────┘
                │
┌───────────────▼─────────────────┐
│      WAKE: 0x58 write (50ms)   │
│ Dummy byte to 0x58, wait 50ms  │
└───────────────┬─────────────────┘
                │
┌───────────────▼─────────────────┐
│     Query Available Bytes       │
│ TX 0x50: avail_cmd              │
│ Wait 10ms                       │
│ RX 0x54: read avail response    │
└───────────────┬─────────────────┘
                │
        ┌───────▼───────┐
        │  avail > 0?   │
        └───┬───────┬───┘
          Yes       No
            │         │
┌───────────▼──┐  ┌──▼──────────────────┐
│  Read Data   │  │  Empty poll counter  │
│  TX 0x50:    │  │  Every 5: 0x58 WAKE  │
│    data_req  │  └──────────────────────┘
│  Wait 10ms   │
│  RX 0x54:    │
│    NMEA data │
└───────┬──────┘
        │
┌───────▼──────────────────────────┐
│         Parse NMEA               │
│  GGA → lat, lon, alt, sats, hdop│
│  RMC → speed, heading, date     │
│  GSV → sats in view             │
│  GSA → fix type, pdop           │
└───────┬──────────────────────────┘
        │
┌───────▼──────────────────────────┐
│    First data? → Send $PAIR066   │
│    (constellation config)        │
│                                  │
│    fix_valid && !rate_configured?│
│    → Send $PAIR050,100 (10Hz)   │
│    → Set rate_configured = true  │
└──────────────────────────────────┘
```

### Recovery Chain

```
TX Failure Recovery (every 5 TX fails):
  → transmit_receive(0x50) — re-registers 0x54 slave address
  
Hard Recovery (50 consecutive hard fails):
  1. PMIC power cycle:
     a. Read reg 0x90 (validate: if 0x00 or error → use 0xFF)
     b. Clear GPS bits (& ~0x2C): ALDO3 + ALDO4 + BLDO2 off
     c. TCA9554: FORCE_ON LOW, NRESET LOW
     d. Wait 5 seconds
     e. Restore voltage registers (0x94=3000mV, 0x95=1800mV, 0x97=2800mV)
     f. Read reg 0x90 again (same validation)
     g. Set GPS bits (| 0x2C): ALDO3 + ALDO4 + BLDO2 on
     h. TCA9554: FORCE_ON HIGH, NRESET HIGH
     i. Wait 500ms on, then 5s boot wait
  2. Re-prime: TxRx(0x50) + data_req + drain read
  3. Send constellation config ($PAIR066,1,1,1,1,0,0)
  4. Reset rate_configured = false
  5. Start 20-cycle grace period

Fix Watchdog (wall-clock time since last valid fix):
  48s + sats > 3  → $PQTMWARM  (warm restart, preserves ephemeris)
  72s              → $PQTMWARM  (warm restart catch-all)
  96s              → $PQTMCOLD  (cold restart, re-downloads ephemeris)
  On fix: reset watchdog timer
  After recovery: apply grace period to watchdog
```

---

## 7. Key Discoveries and Insights

### 7.1 The LC76G Has Three Separate I2C Personalities

- **0x50 (Command):** Accepts CASIC commands. CW writes are dangerous.
  The command parser has limited corruption tolerance. Use sparingly.
- **0x54 (Data):** NEVER write to this address. Read-only. Will NACK writes.
- **0x58 (Data Write):** Safe for bus activity. Can be written to freely
  without corrupting anything. This is the key to the WAKE mechanism.

### 7.2 Bus Activity ≠ Command Execution

The WAKE mechanism doesn't execute a command — it just generates I2C bus
activity (START, address, data, STOP). The LC76G's I2C slave interface
uses this activity as a "keep alive" signal. Without it, the interface
enters a low-power idle mode within ~15 read cycles.

### 7.3 Navigation Engine Priority Inversion

At 10Hz output, the LC76G spends most of its CPU time formatting and DMA'ing
NMEA sentences. The navigation engine (which computes the actual position
from satellite signals) is lower priority than the output pipeline. Before
first fix, the nav engine needs large blocks of uninterrupted CPU time to:
1. Decode satellite ephemeris (orbital parameters)
2. Compute pseudoranges from signal timing
3. Solve the 4-unknown position equation (x, y, z, clock bias)
4. Refine via iterative least-squares fitting

At 1Hz, the nav engine gets 900ms between output bursts. At 10Hz, it only
gets 50ms — not enough for initial position solution convergence.

### 7.4 Warm vs Cold Restart — A World of Difference

- **Warm restart ($PQTMWARM):** Preserves ephemeris in RAM. TTFF 20-45s.
  Use this when the module is producing data but not fixing. Ephemeris
  is still valid if the module has been running < 2 hours.
- **Cold restart ($PQTMCOLD):** Wipes everything. Downloads ephemeris
  from scratch via satellite signals. TTFF 45-120s. Use only as
  last resort — every cold restart extends the total recovery time.

### 7.5 The Shared I2C Bus Is a Minefield

Eight devices on one I2C bus at varying speeds:
| Device | Address | Speed | Poll Rate | Notes |
|--------|---------|-------|-----------|-------|
| LC76G GPS | 0x50/54/58 | 100kHz | Continuous | Requires bus_reset + WAKE |
| CST9217 Touch | 0x5A | 400kHz | 50Hz | **DISABLED** (contention source) |
| QMI8658 IMU | 0x6B | 400kHz | 100Hz | Works fine at 400kHz |
| AXP2101 PMIC | 0x34 | 400kHz | On-demand | Register read can return 0x00 |
| TCA9554 GPIO | 0x20 | 400kHz | On-demand | Used for GPS power control |
| PCF85063 RTC | 0x51 | 400kHz | On-demand | Rare conflicts |
| ES8311 Codec | 0x18 | 400kHz | Unused | Not polled in GPS firmware |
| ES7210 ADC | 0x40 | 400kHz | Unused | Not polled in GPS firmware |

The LC76G requires 100kHz. When ESP-IDF's I2C master driver switches between
400kHz devices and the 100kHz LC76G, the clock domain transition can cause
brief bus glitches. The bus_reset before each GPS cycle ensures a clean state.

### 7.6 PMIC Register Read Is Never Safe to Trust

Any I2C register read on a contended bus can return stale or garbage data.
For power-critical operations (turning LDOs on/off), always validate:
- Check the return code (`ESP_OK` or error)
- Check the value for implausible states (0x00 for a register that should
  have at least some bits set)
- Use a safe default that can't cause worse damage than the current state

---

## 8. Debugging Methodology That Worked

### 8.1 Change One Thing at a Time

Every v16 iteration changed exactly one variable from the previous iteration.
When an iteration failed, the root cause was always the one thing that changed.
When v16 (the first attempt) changed everything at once, the failure was
undiagnosable.

### 8.2 Verify, Don't Assume

"The code looks correct" is NOT verification. Verification means:
1. Build succeeds
2. Flash succeeds
3. Monitor shows expected behavior for at least 60 seconds
4. Continuity test passes 10/10

### 8.3 Continuity Testing Is Non-Negotiable

A single successful boot means nothing for a system with:
- Shared bus contention (random timing)
- Ephemeris state (cold vs warm vs hot start)
- Temperature drift (I2C timing changes)
- Recovery cascade potential

### 8.4 Monitor Output Is Your Best Friend

Every v16 iteration was diagnosed by reading the monitor output carefully:
- CW corruption: monitor showed NACK count correlating with CW write count
- PMIC crash: monitor showed USB disconnect at specific timing
- 10Hz overload: monitor showed "sats=30, fix=0" with 10Hz sent before fix
- Recovery cascade: monitor showed repeated "Recovery triggered" every 15s

### 8.5 Record Everything

Every test run was documented:
- Firmware version
- Time to fix (or time to failure)
- Satellite count
- Recovery events
- Crash events

This data was essential for spotting patterns across runs.

### 8.6 The 20/20 Standard

Don't stop at 10/10. Run it again. If it passes 20/20, you have real
confidence. If it fails 1 in 20, you have a 5% failure rate that needs
investigation.

---

## 9. Quick Reference — What Every Setting Does

| Parameter | Value | Why This Value |
|-----------|-------|----------------|
| bus_reset delay | 50ms | LC76G needs full SCL recovery time on shared bus |
| WAKE type | 0x58 dummy write | Safe bus activity — doesn't corrupt command parser |
| WAKE delay | 50ms | Sufficient for LC76G slave interface activation |
| Per-read WAKE | Yes, every read | Keeps I2C slave alive during continuous polling |
| Empty-poll WAKE | Every 5 polls | Prevents idle mode during periods of no GPS data |
| TX fail recovery | Every 5 fails | TxRx on 0x50 re-registers 0x54 slave address |
| Hard recovery threshold | 50 fails | High enough to survive bus contention, low enough to eventually recover |
| Post-recovery grace | 20 cycles | Prevents immediate re-trigger from residual bus noise |
| PMIC read validation | 0xFF safe default | Prevents board power-off from corrupted register read |
| 10Hz activation | After first fix only | Prevents navigation engine priority inversion |
| Constellation config | At first data | No TTFF impact — safe to send early |
| Watchdog 48s (sats>3) | Warm restart | Ephemeris likely valid, just need position solution nudge |
| Watchdog 72s | Warm restart | Catch-all for stuck nav engine |
| Watchdog 96s | Cold restart | Last resort — full ephemeris re-download |
| Touch task | DISABLED | Eliminates #1 source of I2C bus contention |
| GPS task priority | 8 | Higher than default (5) to ensure polling isn't starved |
| I2C clock | 100kHz | LC76G maximum supported rate |
| TX-RX delay | 10ms | Module needs time to populate response buffer |

---

## Appendix A: Continuity Test Results

### v16k Round 1 (10 runs)
| Run | Time to Fix | Sats | Status |
|-----|-------------|------|--------|
| 1 | 45.2s | 14 | PASS |
| 2 | 32.8s | 12 | PASS |
| 3 | 55.1s | 15 | PASS |
| 4 | 48.7s | 13 | PASS |
| 5 | 102.6s | 11 | PASS |
| 6 | 42.3s | 14 | PASS |
| 7 | 56.8s | 12 | PASS |
| 8 | 39.4s | 16 | PASS |
| 9 | 67.2s | 13 | PASS |
| 10 | 43.1s | 14 | PASS |
**Average: 53.3s | All PASS**

### v16k Round 2 (10 runs)
| Run | Time to Fix | Sats | Status |
|-----|-------------|------|--------|
| 1 | 38.9s | 15 | PASS |
| 2 | 112.0s | 10 | PASS |
| 3 | 44.2s | 14 | PASS |
| 4 | 29.2s | 16 | PASS |
| 5 | 51.7s | 13 | PASS |
| 6 | 47.3s | 12 | PASS |
| 7 | 55.8s | 14 | PASS |
| 8 | 41.6s | 15 | PASS |
| 9 | 39.1s | 14 | PASS |
| 10 | 45.4s | 13 | PASS |
**Average: 50.5s | All PASS**

---

## Appendix B: File References

| File | Purpose |
|------|---------|
| `gps/main/gps_handler.c` | GPS driver (v16k) — all mechanisms implemented here |
| `gps/main/display_init.c` | Touch task disabled here |
| `gps/continuity_test.py` | Automated continuity test script |
| `wiki/LC76G-I2C-GPS-Driver-Guide.md` | Authoritative protocol reference (v3.0.0) |
| `wiki/LC76G-10Hz-Spec-Breakout.md` | 10Hz feasibility analysis |
| `common/include/opendash_identity.h` | Device identity system |
| `common/src/opendash_identity.c` | Identity NVS logic |

---

*This document represents months of cumulative debugging. If you change the GPS
driver and it breaks, come back here first. The answer is almost certainly one
of the five root causes documented above.*
