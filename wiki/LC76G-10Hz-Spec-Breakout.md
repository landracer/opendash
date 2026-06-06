<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# LC76G 10 Hz GPS Data Logging — Specification Breakout

> **Board:** Waveshare ESP32-S3-Touch-AMOLED-1.75  
> **Module:** Quectel LC76G (firmware LC76GABNR12A03S, 2024/04/14)  
> **Protocol:** CASIC I2C (0x50/0x54/0x58) at 100 kHz  
> **Production Code:** gps_handler.c v15L2  
> **Reference:** wiki/LC76G-I2C-GPS-Driver-Guide.md v2.0.0  
> **Date:** March 2025  

---

## 1. Can the LC76G Do 10 Hz?

**YES.** The LC76G firmware supports configurable update rates from 1 Hz to
10 Hz via the `$PAIR050` NMEA command. The Quectel PAIR protocol documentation
confirms 10 Hz (100 ms fix interval) as a supported rate.

### Command to Set 10 Hz

```
$PAIR050,100*CS\r\n
```

Where `100` = milliseconds between fixes. Valid values:

| Rate | Interval (ms) | `$PAIR050` Value |
|------|---------------|-----------------|
| 1 Hz | 1000 | `$PAIR050,1000` |
| 2 Hz | 500 | `$PAIR050,500` |
| 5 Hz | 200 | `$PAIR050,200` |
| **10 Hz** | **100** | **`$PAIR050,100`** |

### ACK Verification

After sending `$PAIR050,100`, the module responds with:

```
$PAIR001,050,0*CS
```

Where the third field `0` = success, `1` = unsupported, `2` = invalid param.
Parse this in `process_nmea_line()` to confirm the rate was accepted.

---

## 2. I2C Bandwidth Analysis at 10 Hz

### Theoretical Bandwidth

| Parameter | Value |
|-----------|-------|
| I2C clock | 100 kHz |
| Raw bit rate | 100,000 bits/s |
| Effective byte rate (8 data + 1 ACK) | ~11,111 bytes/s |
| Overhead per transaction | START + STOP + address byte ≈ 20 bits |
| Realistic throughput | ~10,000 bytes/s |

### NMEA Data Volume per Fix

At 10 Hz with all standard sentences enabled:

| Sentence | Typical Size | Per Second (10 Hz) |
|----------|-------------|-------------------|
| GGA | ~80 bytes | 800 bytes |
| RMC | ~70 bytes | 700 bytes |
| GSA | ~65 bytes | 650 bytes |
| GSV (1-3 messages) | ~200 bytes avg | 2000 bytes |
| VTG | ~40 bytes | 400 bytes |
| GLL | ~50 bytes | 500 bytes |
| **Total** | **~505 bytes/fix** | **~5,050 bytes/s** |

### Verdict

| Metric | Capacity | 10 Hz Demand | Margin |
|--------|----------|-------------|--------|
| I2C throughput | ~10,000 B/s | ~5,050 B/s | **~50%** |
| LC76G I2C buffer | ~4,096 bytes | 505 B per 100ms | **~8 fixes** buffered |

**Result:** 10 Hz is comfortably within I2C bandwidth at 100 kHz.
The ~50% margin accounts for I2C bus contention from other devices on
the shared bus (touch, IMU, display, audio, RTC).

### Optimization: Disable Unnecessary Sentences

At 10 Hz, you may want to disable GLL and VTG (data is redundant with
RMC and GGA) to reduce bandwidth:

```
$PAIR062,0,1,1,1,0,0,0,0,0,0,0,1,0,0,0,0,0*CS
```

This keeps: GGA, RMC, GSA, GSV (disabling GLL, VTG, and others).
Reduces per-fix data from ~505 to ~415 bytes (~4,150 B/s at 10 Hz).

---

## 3. Poll Loop Timing at 10 Hz

### Critical Constraint

At 10 Hz, NMEA bursts arrive every **100 ms**. The poll loop must complete
a full cycle (reset → query → read → parse → WAKE) within 100 ms to avoid
missing data.

### v15L2 Cycle Breakdown at 10 Hz

| Phase | Time | Notes |
|-------|------|-------|
| `i2c_master_bus_reset()` | ~1 ms | 9 SCL clocks |
| Post-reset delay | **50 ms** | v15L2 optimal (CANNOT reduce — shared bus) |
| TX avail query (8 B) | ~1 ms | Including START/STOP |
| TX→RX gap | 10 ms | Minimum stable |
| RX avail response (4 B) | ~0.5 ms | |
| TX data_req + RX data | ~5-50 ms | Depends on buffer fill |
| Per-read WAKE | ~15 ms | CW(8B) + dummy(1B) + delays |
| **Total per cycle** | **~82-127 ms** | |

### Will 50 ms Bus Reset Delay Work at 10 Hz?

**Yes, but tight.** At ~82 ms minimum cycle, data arrives every 100 ms,
giving ~18 ms margin. If data is large (full GSV constellation report),
the cycle stretches to ~127 ms and may miss one beat — but the LC76G
buffers up to ~4 KB, so the next poll catches both.

### Can We Reduce the 50 ms Delay?

**Not recommended.** Testing showed:

| Bus Reset Delay | Data Flow Duration | Total Bytes |
|-----------------|-------------------|-------------|
| 20 ms | ~45 s | 57,710 |
| **50 ms** | **100-125 s** | **62-95K** |
| 100 ms | ~83 s | 67,196 |

The 50 ms delay is the sweet spot for shared bus stability. Reducing it
causes faster bus degradation and MORE total downtime from recovery cycles.

### Alternative: Reduce No-Data Backoff

Instead of reducing the bus reset delay, reduce the idle backoff when no
data is available:

```c
// In main loop, after avail=0:
uint32_t backoff = (rate_hz >= 10) ? 30 : 100;  // 30 ms at 10 Hz, 100 ms at 1 Hz
vTaskDelay(pdMS_TO_TICKS(backoff));
```

This keeps the bus reset delay at 50 ms (stable) but reduces the wait
between polls, catching 10 Hz bursts more reliably.

---

## 4. Data Logging Throughput at 10 Hz

### SD Card Write Performance

| Parameter | Value |
|-----------|-------|
| NMEA data rate (10 Hz) | ~5 KB/s |
| SD card write speed (SPI mode) | >1 MB/s |
| Write buffer (recommended) | 4096 bytes |
| Write frequency | Every ~0.8 s (buffer full) |
| SD card contention with I2C | **NONE** (SD uses SPI, not I2C) |

### Storage Requirements

| Duration | Raw NMEA Size | Notes |
|----------|-------------|-------|
| 1 minute | ~300 KB | |
| 1 hour | ~18 MB | |
| 1 race session (2h) | ~36 MB | |
| 16 GB card capacity | ~444 hours | |

### Logging Strategy

```c
// Recommended: buffer NMEA lines, flush on buffer full or timer
#define LOG_BUF_SIZE    4096
static char log_buf[LOG_BUF_SIZE];
static size_t log_pos = 0;

void log_nmea_line(const char *line, size_t len) {
    if (log_pos + len + 2 > LOG_BUF_SIZE) {
        // Flush to SD
        fwrite(log_buf, 1, log_pos, sd_file);
        log_pos = 0;
    }
    memcpy(log_buf + log_pos, line, len);
    log_pos += len;
    log_buf[log_pos++] = '\n';
}
```

---

## 5. Constellation Configuration for Best 10 Hz Performance

### `$PAIR066` — Enable/Disable Constellations

```
$PAIR066,<GPS>,<GLONASS>,<Galileo>,<BeiDou>,<0>,<QZSS>*CS
```

| Config | Command | Sats Available | Fix Quality |
|--------|---------|---------------|-------------|
| **All** (default) | `$PAIR066,1,1,1,1,0,1` | 30-50 | Best accuracy |
| GPS+GLONASS+Galileo | `$PAIR066,1,1,1,0,0,0` | 20-35 | Good accuracy, less CPU |
| GPS only | `$PAIR066,1,0,0,0,0,0` | 8-14 | Fastest fix, less accuracy |

**Recommendation for racing:** Keep all constellations enabled. The LC76G
handles multi-constellation well, and more satellites = better HDOP = better
position accuracy at speed.

### Observed Performance (v15L2, All Constellations)

| Metric | Observed Value |
|--------|---------------|
| Satellites tracked | 9-16 |
| HDOP | 0.7-1.1 |
| Fix type | 3D |
| Position accuracy | ~1.5 m (estimated from HDOP) |
| Fix quality | Consistent across all test runs |

---

## 6. 10 Hz Implementation Checklist

### Prerequisites

- [ ] v15L2 code running stable at 1 Hz (baseline verified)
- [ ] SD card logging framework implemented
- [ ] `lc76g_send_command()` function available for PAIR commands

### Implementation Steps

1. **Set rate AFTER first valid NMEA data** (not at init):
   ```c
   // In main loop, after first GGA received:
   if (first_fix && !rate_set) {
       lc76g_send_command("$PAIR050,100");  // 10 Hz
       rate_set = true;
   }
   ```

2. **Verify ACK**:
   ```c
   // In process_nmea_line():
   if (strncmp(line, "$PAIR001,050,", 13) == 0) {
       char result = line[13];  // '0'=success, '1'=unsupported
       if (result == '0') rate_hz = 10;
   }
   ```

3. **Reduce idle backoff**:
   ```c
   uint32_t backoff = (rate_hz >= 10) ? 30 : 100;
   vTaskDelay(pdMS_TO_TICKS(backoff));
   ```

4. **Optionally disable GLL/VTG** to save bandwidth:
   ```c
   lc76g_send_command("$PAIR062,0,1,1,1,0,0,0,0,0,0,0,1,0,0,0,0,0");
   ```

5. **Enable SD logging** (10 Hz generates ~5 KB/s)

6. **Monitor buffer overruns** — if `avail` consistently returns > 2048 bytes,
   the poll loop is falling behind. Increase task priority or reduce sentences.

### What NOT To Do

- **DON'T** set 10 Hz before first fix — the module may reject the command
  during cold start
- **DON'T** reduce bus reset delay below 50 ms — causes bus degradation
- **DON'T** set 10 Hz via I2C register write — use NMEA `$PAIR050` command
- **DON'T** expect 100% continuous 10 Hz — shared bus degradation after
  100-125s will cause recovery gaps (~10-15s each)

---

## 7. Expected 10 Hz Performance Budget

### Optimistic Scenario

| Phase | Duration | Data Rate | Bytes |
|-------|----------|-----------|-------|
| Cold start → first fix | 30 s | 0 | 0 |
| Continuous data flow | 100-125 s | ~5 KB/s | 500-625 KB |
| Recovery (power cycle) | ~15 s | 0 | 0 |
| Next continuous flow | 100-125 s | ~5 KB/s | 500-625 KB |

### Over a 5-Minute Window

| Metric | Estimate |
|--------|----------|
| Active data time | ~250 s (of 300 s) |
| Recovery gaps | ~2-3 gaps × 15 s = 30-45 s |
| Total data | ~1.25 MB |
| Fix count | ~2,500 fixes |
| Effective rate | ~8.3 Hz average |

### Over a 30-Minute Race Session

| Metric | Estimate |
|--------|----------|
| Active data time | ~1,500 s (of 1,800 s) |
| Recovery gaps | ~12-15 gaps × 15 s |
| Total data | ~7.5 MB |
| Fix count | ~15,000 fixes |
| Effective rate | ~8.3 Hz average |

> **Note:** The ~8.3 Hz effective rate (vs 10 Hz nominal) is due to shared
> bus degradation recovery cycles. This is a hardware limitation of the shared
> I2C bus, not a GPS or software limitation. A dedicated I2C bus would likely
> achieve >99% 10 Hz coverage.

---

## 8. Future: Dedicated I2C Bus for GPS

If 10 Hz continuous coverage is critical (no recovery gaps), the solution is
a **dedicated I2C bus** for the LC76G:

| Option | Pins | Feasibility |
|--------|------|-------------|
| Bit-bang I2C on free GPIOs | Any 2 GPIOs | Works but CPU-intensive |
| Hardware I2C bus 1 | Requires free GPIO pair | Best option if available |
| External I2C multiplexer | TCA9548A on existing bus | Isolates GPS from contention |

An I2C multiplexer (TCA9548A) on the existing bus would let the GPS have
exclusive bus access during reads, preventing touch/IMU contention.

---

## 9. PAIR Command Reference (LC76G Firmware)

| Command | Purpose | Example |
|---------|---------|---------|
| `$PAIR050,<ms>` | Set fix interval | `$PAIR050,100` (10 Hz) |
| `$PAIR062,...` | Configure NMEA sentences | See §5 above |
| `$PAIR066,g,l,e,b,0,q` | Enable/disable constellations | `$PAIR066,1,1,1,1,0,1` |
| `$PAIR001,<cmd>,<result>` | ACK response | `$PAIR001,050,0` (success) |
| `$PAIR513` | Cold start | `$PAIR513` |
| `$PAIR514` | Warm start | `$PAIR514` |
| `$PAIR515` | Hot start | `$PAIR515` |
| `$PAIR020` | Query fix interval | `$PAIR020` |
| `$PAIR021,<ms>` | Response to `$PAIR020` | `$PAIR021,100` |

All commands sent via `lc76g_send_command()` which wraps them in CASIC I2C
write protocol (TX to 0x50 with proper CASIC framing).

---

## 10. Version History Relevant to 10 Hz

| Version | Bus Reset | Flow Duration | Effective 10Hz Rate |
|---------|-----------|-------------- |-------------------- |
| v15 | 20 ms | 1 burst | Not viable |
| v15k-fix2 | 20 ms | ~45 s | ~7.5 Hz avg |
| **v15L2** | **50 ms** | **100-125 s** | **~8.3 Hz avg** |
| v15L3 | 100 ms | ~83 s | ~7.7 Hz avg |

**v15L2 is optimal for 10 Hz logging.** The 50 ms bus reset delay maximizes
continuous flow duration, giving the best effective data rate.

---

> **Bottom line:** 10 Hz GPS logging is achievable with the current v15L2
> firmware and LC76G hardware. The I2C bandwidth supports it with ~50% margin.
> The main limitation is shared bus degradation (~15s recovery every ~2 minutes),
> resulting in ~8.3 Hz effective average rate. For racing telemetry, this
> provides ~15,000 position fixes per 30-minute session — more than adequate
> for lap timing, racing line analysis, and speed logging.
