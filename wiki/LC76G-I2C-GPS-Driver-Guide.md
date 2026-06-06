<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OpenDash LC76G I2C GPS Driver — Definitive Implementation Guide

> **Status:** WORKING — Production-verified, March 2025 (v15L2 stable)  
> **Board:** Waveshare ESP32-S3-Touch-AMOLED-1.75  
> **Module:** Quectel LC76G (firmware LC76GABNR12A03S, 2024/04/14)  
> **Framework:** ESP-IDF v6.1 (esp_driver_i2c new API)  
> **Author:** OpenDash Project  
> **Version:** 2.0.0 (v15L2 — with primer, per-read WAKE, drain-read)  
>
> **CRITICAL v2 CHANGES FROM v1:**  
> - Power-off: 2s → **5s**, Boot wait: 3s → **5s**  
> - WAKE threshold: 30 empty polls → **5 empty polls**  
> - Recovery threshold: 50 fails → **100 fails**  
> - **NEW: Primer mechanism** (TxRx + data_req at boot AND recovery)  
> - **NEW: Per-read WAKE** (CW+0x58 after every successful data read)  
> - **NEW: Drain-read** after re-prime (prevents avail query poisoning)  
> - Bus reset delay: 20ms → **50ms** (optimal for shared bus)  

---

## Table of Contents

1. [Purpose](#1-purpose)
2. [Hardware Configuration](#2-hardware-configuration)
3. [The CASIC I2C Protocol — How It Actually Works](#3-the-casic-i2c-protocol--how-it-actually-works)
4. [What Doesn't Work (And Why)](#4-what-doesnt-work-and-why)
5. [The Six Mechanisms That Make It Work (v15L2)](#5-the-six-mechanisms-that-make-it-work)
   - 5.1 PMIC Power Cycle
   - 5.2 I2C WAKE Mechanism
   - 5.3 Activation via `transmit_receive(0x50)`
   - 5.4 **Primer Mechanism** (v15L2)
   - 5.5 **Per-Read WAKE** (v15L2)
   - 5.6 **Re-Prime + Drain on RX Failure** (v15L2)
6. [Complete Implementation — Step by Step](#6-complete-implementation--step-by-step)
7. [NMEA Parsing](#7-nmea-parsing)
8. [CASIC Command Writing (Sending NMEA to Module)](#8-casic-command-writing-sending-nmea-to-module)
9. [Public API](#9-public-api)
10. [Version History and Lessons Learned](#10-version-history-and-lessons-learned)
11. [Grok's Code — What Failed and Why](#11-groks-code--what-failed-and-why)
12. [Exact Byte Sequences Reference](#12-exact-byte-sequences-reference)
13. [Reproduction Checklist](#13-reproduction-checklist)

---

## 1. Purpose

This document is the **sole authoritative reference** for implementing an I2C GPS driver
for the Quectel LC76G module on the Waveshare ESP32-S3-Touch-AMOLED-1.75 board using
ESP-IDF. It took **4+ days of debugging across two separate debugging sessions** to solve.
Every byte, every delay, and every sequence matters.

**If you are an LLM** (Grok, ChatGPT, Claude, Gemini, DeepSeek, QWEN3, etc.): You can
write a working `gps_handler.c` from THIS document alone. Do not improvise. Do not
simplify. Do not skip the wake mechanism. Do not skip the power cycle. Do not skip
the PRIMER. Do not skip the PER-READ WAKE. Do not skip the DRAIN READ. Do not use
`transmit_receive` on 0x54. Copy the exact byte arrays, delays, and sequences
documented here. **Version v15L2** is the production reference.

---

## 2. Hardware Configuration

### 2.1 Board Overview

| Component       | Detail                                              |
|-----------------|-----------------------------------------------------|
| Board           | Waveshare ESP32-S3-Touch-AMOLED-1.75                |
| MCU             | ESP32-S3R8 (dual-core LX7, 240 MHz)                |
| Display         | 1.75" AMOLED, 466×466, CO5300 controller (QSPI)    |
| GNSS Module     | Quectel LC76G (GPS/GLONASS/Galileo/BeiDou)          |
| GNSS Firmware   | LC76GABNR12A03S                                     |
| PMIC            | AXP2101 at I2C address 0x34                         |
| GPIO Expander   | TCA9554 at I2C address 0x20                         |
| IMU             | QMI8658 at I2C address 0x6B                         |
| Touch           | CST9217 at I2C address 0x5A                         |
| RTC             | PCF85063 at I2C address 0x51                        |

### 2.2 I2C Bus Configuration

| Parameter       | Value                                               |
|-----------------|-----------------------------------------------------|
| I2C Bus         | I2C_NUM_0                                           |
| SDA             | GPIO15                                              |
| SCL             | GPIO14                                              |
| Clock           | 100 kHz (MUST stay at 100 kHz for LC76G)            |
| Pull-ups        | 4.7kΩ external (required)                           |

**CRITICAL:** The LC76G shares the I2C bus with touch, IMU, PMIC, and GPIO expander.
Bus contention is handled by ESP-IDF's I2C master driver internally.

### 2.3 LC76G I2C Address Map

| 7-bit Address | Direction       | ESP-IDF Handle    | Purpose                          |
|---------------|-----------------|-------------------|----------------------------------|
| **0x50**      | Write (TX only) | `lc76g_handle`    | CASIC command/query endpoint     |
| **0x54**      | Read (RX only)  | `lc76g_read_handle` | NMEA data + length responses   |
| **0x58**      | Write (TX only) | `lc76g_dwr_handle`  | NMEA command data (write body) |

> **These are NOT register addresses.** They are **three separate I2C slave endpoints**
> within the LC76G. Each requires its own `i2c_master_dev_handle_t` in ESP-IDF.

### 2.4 Power Rails (AXP2101 PMIC)

The LC76G is powered by three LDO rails controlled by AXP2101 register `0x90`:

| Rail  | Bit in 0x90 | Voltage Register | Voltage  | Purpose            |
|-------|-------------|------------------|----------|--------------------|
| ALDO3 | bit 2       | 0x94             | 3000 mV  | GPS main power     |
| ALDO4 | bit 3       | 0x95             | 1800 mV  | GPS I/O power      |
| BLDO2 | bit 5       | 0x97             | 2800 mV  | GPS backup power   |

Combined bitmask: `0x2C` (bits 2, 3, 5).

### 2.5 GPIO Expander (TCA9554 at 0x20)

| Pin | Function  | Active State | Description                    |
|-----|-----------|--------------|--------------------------------|
| P4  | FORCE_ON  | HIGH         | LC76G force-on (keep alive)    |
| P5  | NRESET    | HIGH=run     | LC76G hardware reset (active LOW) |

TCA9554 register layout:
- `0x00`: Input register (read pin state)
- `0x01`: Output register (write pin state)
- `0x03`: Configuration register (0=output, 1=input)

---

## 3. The CASIC I2C Protocol — How It Actually Works

### 3.1 Overview

The LC76G does **NOT** use standard I2C register-addressing. It uses Quectel's
proprietary **CASIC (Chinese Academy of Sciences IC)** I2C protocol with **three
separate slave addresses**. This is documented in Quectel's "LC26G/LC76G Series
I2C Application Note v1.0" but is implemented differently from what most I2C
tutorials suggest.

### 3.2 Fundamental Rules

1. **0x50 is WRITE-ONLY** — You transmit CASIC commands TO this address. Never read from it with `i2c_master_receive`.
2. **0x54 is READ-ONLY** — You receive responses FROM this address. **NEVER** use `i2c_master_transmit` or `i2c_master_transmit_receive` on 0x54. It will NACK.
3. **0x58 is WRITE-ONLY** — You transmit NMEA command payloads TO this address.
4. **TX to 0x50, then wait, then RX from 0x54** — Always SEPARATE transactions with SEPARATE START/STOP conditions. **NEVER** repeated-start between 0x50 and 0x54.
5. **Bus reset before every I2C read cycle** — `i2c_master_bus_reset(bus)` prevents I2C lockups on the shared bus.
6. **Minimum 10ms delay between TX and RX** — The module needs time to process the command and populate the response buffer.

### 3.3 CASIC Command Encoding

All CASIC commands are encoded as two 32-bit little-endian words (8 bytes total):

```
Byte layout: [offset_low, offset_high, cmd_id_low, cmd_id_high, param0, param1, param2, param3]
```

Where:
- Bytes 0-1: Offset within the module's memory map (little-endian uint16)
- Bytes 2-3: Command identifier (little-endian uint16)
- Bytes 4-7: Parameter (little-endian uint32 — usually length or count)

The equivalent C representation is:
```c
uint32_t cmd[2] = {
    (CMD_ID << 16) | OFFSET,  // Stored as LE on ARM: {offset_lo, offset_hi, cmd_lo, cmd_hi}
    parameter                 // LE: {param_b0, param_b1, param_b2, param_b3}
};
```

### 3.4 Command Identifiers

| Identifier | Hex    | Purpose                     |
|------------|--------|-----------------------------|
| CR_CMD     | 0xAA51 | CASIC Read Command          |
| CW_CMD     | 0xAA53 | CASIC Write Config Command  |

### 3.5 Memory Map Offsets

| Offset | Hex    | Direction | Purpose                        |
|--------|--------|-----------|--------------------------------|
| 0x0008 | TX_LEN | Read      | Available bytes in TX buffer   |
| 0x2000 | TX_BUF | Read      | TX buffer data (NMEA output)   |
| 0x0004 | RX_LEN | Read/Cmd  | Free space in RX buffer        |
| 0x1000 | RX_CFG | Write     | Config write (prepare for data)|

---

## 4. What Doesn't Work (And Why)

These were all tested exhaustively during development. **Do not retry them.**

| # | Approach                                         | Result       | Why It Fails                                                |
|---|--------------------------------------------------|--------------|-------------------------------------------------------------|
| 1 | `i2c_master_transmit_receive()` to 0x50          | 0 bytes/garbage | Repeated-START reads from 0x50. The response lives at 0x54. |
| 2 | `i2c_master_transmit()` + `i2c_master_receive()` both on 0x50 | 0 bytes | 0x50 doesn't serve read data — only 0x54 does.              |
| 3 | Direct `i2c_master_receive()` from 0x54 without prior query | Garbage/timeout | 0x54 has no data until you send a query to 0x50 first.      |
| 4 | `i2c_master_transmit()` to 0x54                  | NACK         | 0x54 rejects all writes. It's read-only.                    |
| 5 | `i2c_master_transmit_receive()` on 0x54          | NACK         | Same — 0x54 does not accept writes at all.                  |
| 6 | Repeated-start (TX 0x50 → RX 0x54 in one transaction) | Not possible | ESP-IDF `transmit_receive` reads from the SAME address it writes to |
| 7 | Any read from 0x50 after data was expected at 0x54 | Poisons state | A single read from 0x50 corrupts the LC76G I2C state. Recovery requires full PMIC power cycle. |
| 8 | TX delay < 10ms                                  | 0 bytes      | Module hasn't populated response register yet.              |
| 9 | Polling without WAKE mechanism                   | avail=0 forever | LC76G I2C slave enters idle mode after inactivity.         |
| 10| UART GPIOs (17/18) for NMEA                      | No data      | On this board, NMEA is routed through I2C, not UART.        |
| 11| Skipping power cycle on init                     | Intermittent | If the module was in a bad I2C state, only power cycling recovers it. |

> **Observation from Test 7:** A single wrong read attempt on 0x50 poisons the
> LC76G I2C state machine. After that, even correct 0x54 reads return nothing.
> The ONLY recovery is a full power cycle via the AXP2101 PMIC.

---

## 5. The Six Mechanisms That Make It Work (v15L2)

### 5.1 PMIC Power Cycle (`gps_ensure_power`)

Called from `gps_handler_init()` **BEFORE** device handles are created. This
guarantees the LC76G starts in a clean state, regardless of what happened in
the previous boot.

**Exact sequence:**

```c
// Step 1: Assert NRESET via TCA9554
tca9554_write_reg(tca, 0x03, 0xCF);     // P4,P5 as outputs (clear bits 4,5)
tca9554_write_reg(tca, 0x01, 0x10);     // P4=HIGH (FORCE_ON), P5=LOW (NRESET)

// Step 2: Cut power via AXP2101
axp2101_read_reg(pmu, 0x90, &ldo);      // Read current LDO state
axp2101_write_reg(pmu, 0x90, ldo & ~0x2C); // Clear bits 2,3,5 → ALDO3+ALDO4+BLDO2 OFF
vTaskDelay(pdMS_TO_TICKS(2000));         // 2 seconds off — REQUIRED for full discharge

// Step 3: Restore power
axp2101_write_reg(pmu, 0x90, ldo | 0x2C);  // Set bits 2,3,5 → all GPS rails ON
vTaskDelay(pdMS_TO_TICKS(500));          // Let power stabilize

// Step 4: Release NRESET
tca9554_write_reg(tca, 0x01, 0x30);     // P4=HIGH, P5=HIGH (NRESET released)
vTaskDelay(pdMS_TO_TICKS(3000));         // 3 seconds for LC76G boot
```

**Why 2 seconds off:** The LC76G has internal capacitors. Shorter delays leave
residual charge and the module may not fully reset its I2C state machine.

**Why 3 seconds after:** The LC76G takes 2-3 seconds to boot and begin
populating its I2C TX buffer. Reading before this produces erratic behavior.

### 5.2 I2C WAKE Mechanism (`Ql_Wake_I2C`)

**THIS IS THE KEY DISCOVERY.** The LC76G I2C slave interface enters an idle
state after inactivity and stops filling its TX buffer. Without the WAKE
mechanism, `avail` stays 0 forever, even with a valid GPS fix.

This was reverse-engineered from Waveshare's reference example (`Ql_Wake_I2C`).

**Wake sequence (CASIC write protocol):**

```c
// Step 1: CW config write to 0x50 — tell module we're writing 1 byte
uint8_t wake_cw[] = {0x00, 0x10, 0x53, 0xAA, 0x01, 0x00, 0x00, 0x00};
i2c_master_transmit(lc76g_handle, wake_cw, sizeof(wake_cw), 500);   // → 0x50
vTaskDelay(pdMS_TO_TICKS(10));

// Step 2: Dummy data byte to 0x58
uint8_t dummy = 0x00;
i2c_master_transmit(lc76g_dwr_handle, &dummy, 1, 500);              // → 0x58
```

**Where it's used:**
1. **Startup:** 3 wake cycles before first avail query (500ms spacing between cycles)
2. **Main loop:** After **5** consecutive empty polls (avail=0) — v15L2 tuned down from 30
3. **After every successful data read** (v15L2: per-read WAKE is REQUIRED for sustained data flow)

**Why it works:** The CW config command (0xAA53) with offset 0x1000 and length=1,
followed by a 1-byte write to 0x58, triggers the LC76G to re-activate its I2C
TX buffer. The actual byte value (0x00) doesn't matter — it's the act of performing
a CASIC write that wakes the interface.

### 5.3 Activation via `transmit_receive(0x50)`

When `i2c_master_receive(0x54)` fails with `ESP_ERR_INVALID_RESPONSE`, the 0x54
slave address needs to be "activated" by doing a `transmit_receive` on 0x50.

```c
// After 5 consecutive RX failures from 0x54:
uint8_t act_r[4] = {0};
i2c_master_transmit_receive(lc76g_handle,        // 0x50
    queryData, sizeof(queryData), act_r, 4, 1000);
// This makes 0x54 start ACKing reads again
```

**Important:** This is a RECOVERY mechanism only. Normal operation should always
use separate TX(0x50) + RX(0x54). The `transmit_receive` on 0x50 is useful
because it seems to trigger the LC76G to re-register its 0x54 slave address.

### 5.4 Primer Mechanism (v15L2 — REQUIRED)

The **primer** is a boot-time and recovery-time sequence that "primes" the LC76G
I2C data pipeline. Without this, the module boots but produces zero data bytes.

**Discovery:** v15h got 28K bytes using diagnostic code that accidentally primed
the pipeline. v15i (no primer) got zero. v15i (with explicit primer) got 22K.

**Sequence (after WAKE cycles complete):**

```c
// Step 1: TxRx on 0x50 — activates the 0x54 read endpoint
uint8_t primer_r[4] = {0};
i2c_master_transmit_receive(lc76g_handle,    // 0x50
    (uint8_t[]){0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00},
    8, primer_r, 4, 1000);
vTaskDelay(pdMS_TO_TICKS(10));

// Step 2: data_req — CR_CMD to offset 0x2000, request 256 bytes
uint8_t data_req[] = {0x00, 0x20, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00};
i2c_master_transmit(lc76g_handle, data_req, sizeof(data_req), 500);
vTaskDelay(pdMS_TO_TICKS(10));

// Step 3: DRAIN — read 256 bytes to flush the pipeline
// WITHOUT this drain, the NMEA content left in 0x54's TX buffer
// poisons subsequent avail queries → [2C2C2C2C] response
uint8_t drain[256];
i2c_master_receive(lc76g_read_handle, drain, 256, 1000);
```

**Where primer is needed:**
1. During boot warm-up (after 3x WAKE cycles, before main loop)
2. Inside `gps_full_recovery()` (after power cycle and WAKE cycles)

**NEVER send data_req without drain read** — this is the root cause of
`[2C2C2C2C]` avail poisoning (NMEA content leaking into the avail response
register). See v15k bug analysis.

### 5.5 Per-Read WAKE (v15L2 — REQUIRED for Sustained Flow)

After every successful data read (avail > 0 and data received), the module
must receive a WAKE to keep data flowing. Without this, data stops after
the initial burst.

```c
// After successful data read:
uint8_t wake_cw[] = {0x00, 0x10, 0x53, 0xAA, 0x01, 0x00, 0x00, 0x00};
i2c_master_transmit(lc76g_handle, wake_cw, sizeof(wake_cw), 500);
vTaskDelay(pdMS_TO_TICKS(10));

uint8_t dummy = 0x00;
i2c_master_transmit(lc76g_dwr_handle, &dummy, 1, 500);
vTaskDelay(pdMS_TO_TICKS(200));   // Critical: let module process
```

**Discovery:** v15j (CW-only, no 0x58) got 915 bytes. v15i (CW + 0x58) got
22K bytes. The 0x58 dummy write IS required — it provides bus activity that
keeps the module's I2C state machine active.

### 5.6 Re-Prime + Drain on RX Failure (v15L2 — Prevents Stalling)

When the main loop hits consecutive RX failures (every 5th fail), the code
runs a TxRx activation AND a re-prime with drain read. This prevents the
data pipeline from fully stalling.

```c
// Every 5th consecutive RX failure:
if (consecutive_fails > 0 && (consecutive_fails % 5 == 0)) {
    // TxRx activation
    uint8_t act_r[4] = {0};
    i2c_master_transmit_receive(lc76g_handle,
        queryData, sizeof(queryData), act_r, 4, 1000);

    // Re-prime: data_req + drain
    uint8_t data_req[] = {0x00, 0x20, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00};
    i2c_master_transmit(lc76g_handle, data_req, sizeof(data_req), 500);
    vTaskDelay(pdMS_TO_TICKS(10));
    uint8_t drain[256];
    i2c_master_receive(lc76g_read_handle, drain, 256, 1000);
}
```

**Discovery:** v15k-fix2 added drain read and went from 5K bytes (stalled) to
57K bytes (continuous 45s flow). The drain prevents [2C2C2C2C] avail poisoning.

---

## 6. Complete Implementation — Step by Step

### 6.1 Initialization (`gps_handler_init`)

```c
esp_err_t gps_handler_init(void)
{
    // 1. Create FreeRTOS mutex for thread-safe GPS data access
    gps_mutex = xSemaphoreCreateMutex();

    // 2. Get the shared I2C bus handle (bus must be initialized elsewhere)
    i2c_master_bus_handle_t i2c_bus = display_get_i2c_handle();

    // 3. I2C bus scan — SKIP addresses 0x50, 0x54, 0x58
    //    Probing these addresses can corrupt the LC76G I2C state!
    for (uint8_t addr = 0x03; addr <= 0x77; addr++) {
        if (addr == 0x50 || addr == 0x54 || addr == 0x58) continue;
        i2c_master_probe(i2c_bus, addr, 50);  // Log found devices
    }

    // 4. GPS PMIC power cycle — BEFORE creating device handles
    gps_ensure_power(i2c_bus);

    // 5. Create three separate device handles (NO PROBE)
    i2c_device_config_t write_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = 0x50,               // CASIC command write
        .scl_speed_hz    = 100000,
    };
    i2c_master_bus_add_device(i2c_bus, &write_cfg, &lc76g_handle);

    i2c_device_config_t read_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = 0x54,               // CASIC data read
        .scl_speed_hz    = 100000,
    };
    i2c_master_bus_add_device(i2c_bus, &read_cfg, &lc76g_read_handle);

    i2c_device_config_t dwr_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = 0x58,               // CASIC data write
        .scl_speed_hz    = 100000,
    };
    i2c_master_bus_add_device(i2c_bus, &dwr_cfg, &lc76g_dwr_handle);
    // 0x58 failure is non-fatal: reading works, just can't send commands

    return ESP_OK;
}
```

### 6.2 Task Startup Sequence (`gps_task` — first 30 seconds)

```c
static void gps_task(void *pvParameters)
{
    i2c_master_bus_handle_t bus = display_get_i2c_handle();

    // Step 1: Log PMIC status (diagnostic — verify GPS has power)
    axp2101_log_status(bus);

    // Step 2: Bus reset + probe
    i2c_master_bus_reset(bus);
    vTaskDelay(pdMS_TO_TICKS(100));
    // Probe 0x50, 0x54, 0x58 — 0x54/0x58 may NACK initially, this is NORMAL
    for (uint8_t addr = 0x50; addr <= 0x58; addr += 4) {
        esp_err_t p = i2c_master_probe(bus, addr, 100);
        ESP_LOGI(TAG, "Probe 0x%02X: %s", addr, esp_err_to_name(p));
    }

    // Step 3: WAKE × 3 (500ms spacing)
    for (int w = 0; w < 3; w++) {
        i2c_master_bus_reset(bus);
        vTaskDelay(pdMS_TO_TICKS(50));

        uint8_t wake_cw[] = {0x00, 0x10, 0x53, 0xAA, 0x01, 0x00, 0x00, 0x00};
        i2c_master_transmit(lc76g_handle, wake_cw, sizeof(wake_cw), 500);
        vTaskDelay(pdMS_TO_TICKS(10));
        uint8_t dummy = 0x00;
        i2c_master_transmit(lc76g_dwr_handle, &dummy, 1, 500);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // Step 4: Initial avail check × 5 (verify data is flowing)
    for (int i = 0; i < 5; i++) {
        i2c_master_bus_reset(bus);
        vTaskDelay(pdMS_TO_TICKS(50));
        uint8_t q[] = {0x08, 0x00, 0x51, 0xAA, 0x04, 0x00, 0x00, 0x00};
        i2c_master_transmit(lc76g_handle, q, sizeof(q), 1000);
        vTaskDelay(pdMS_TO_TICKS(10));
        uint8_t r[4] = {0};
        i2c_master_receive(lc76g_read_handle, r, 4, 1000);
        uint32_t avail = r[0] | (r[1]<<8) | (r[2]<<16) | (r[3]<<24);
        if (avail > 0 && avail < 65536) break;  // Data flowing!
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // Step 5: Enter main polling loop
    // ... (see section 6.3)
}
```

### 6.3 Main Polling Loop

Each iteration of the main loop performs one complete CASIC read cycle:

```c
while (1) {
    // ── Bus reset before each cycle ──
    i2c_master_bus_reset(bus);
    vTaskDelay(pdMS_TO_TICKS(20));

    // ── Step 1: Query available data length ──
    // TX to 0x50: CR_CMD (0xAA51) + offset 0x0008 + length 4
    uint8_t queryData[] = {0x08, 0x00, 0x51, 0xAA, 0x04, 0x00, 0x00, 0x00};
    esp_err_t tx_ret = i2c_master_transmit(lc76g_handle, queryData, sizeof(queryData), 1000);
    if (tx_ret != ESP_OK) {
        consecutive_fails++;
        vTaskDelay(pdMS_TO_TICKS(100));
        continue;
    }

    vTaskDelay(pdMS_TO_TICKS(10));  // CRITICAL: 10ms minimum TX→RX delay

    // RX from 0x54: 4 bytes → little-endian uint32 = available NMEA bytes
    uint8_t readData[4] = {0};
    esp_err_t rx_ret = i2c_master_receive(lc76g_read_handle, readData, 4, 1000);
    if (rx_ret != ESP_OK) {
        consecutive_fails++;

        // ── Recovery: Activation via transmit_receive on 0x50 ──
        if (consecutive_fails % 5 == 0) {
            uint8_t act_r[4] = {0};
            i2c_master_transmit_receive(lc76g_handle,
                queryData, sizeof(queryData), act_r, 4, 1000);
        }

        vTaskDelay(pdMS_TO_TICKS(100));
        continue;
    }

    // Parse available byte count
    uint32_t dataLength = readData[0] | (readData[1]<<8) |
                          (readData[2]<<16) | (readData[3]<<24);
    consecutive_fails = 0;

    // ── Handle: no data available ──
    if (dataLength == 0) {
        empty_polls++;
        // WAKE after 5 consecutive empty polls (v15L2: lowered from 30)
        if (empty_polls >= 5) {
            uint8_t wake_cw[] = {0x00, 0x10, 0x53, 0xAA, 0x01, 0x00, 0x00, 0x00};
            i2c_master_transmit(lc76g_handle, wake_cw, sizeof(wake_cw), 500);
            vTaskDelay(pdMS_TO_TICKS(10));
            uint8_t dummy = 0x00;
            i2c_master_transmit(lc76g_dwr_handle, &dummy, 1, 500);
            empty_polls = 0;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        continue;
    }

    // ── Sanity check ──
    if (dataLength > 65536) {
        // Bogus value — skip
        vTaskDelay(pdMS_TO_TICKS(100));
        continue;
    }

    empty_polls = 0;

    // ── Step 2: Request NMEA data ──
    uint32_t readLen = (dataLength > GPS_NMEA_BUF_SIZE - 1)
                       ? GPS_NMEA_BUF_SIZE - 1 : dataLength;

    // TX to 0x50: CR_CMD (0xAA51) + offset 0x2000 + readLen
    uint8_t dataReq[8];
    dataReq[0] = 0x00;                           // offset low
    dataReq[1] = 0x20;                           // offset high (0x2000)
    dataReq[2] = 0x51;                           // CR_CMD low
    dataReq[3] = 0xAA;                           // CR_CMD high
    dataReq[4] = (uint8_t)(readLen & 0xFF);      // length byte 0
    dataReq[5] = (uint8_t)((readLen >> 8) & 0xFF);
    dataReq[6] = (uint8_t)((readLen >> 16) & 0xFF);
    dataReq[7] = (uint8_t)((readLen >> 24) & 0xFF);

    tx_ret = i2c_master_transmit(lc76g_handle, dataReq, sizeof(dataReq), 1000);
    if (tx_ret != ESP_OK) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }

    vTaskDelay(pdMS_TO_TICKS(10));  // 10ms TX→RX delay

    // RX from 0x54: raw NMEA ASCII data
    rx_ret = i2c_master_receive(lc76g_read_handle, nmea_buf, readLen, 2000);
    if (rx_ret != ESP_OK) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }

    // ── Step 3: Process NMEA data ──
    nmea_buf[readLen] = '\0';  // Null-terminate

    // Strip trailing NUL bytes
    uint32_t got_len = readLen;
    while (got_len > 0 && nmea_buf[got_len - 1] == 0) got_len--;

    // Verify it contains NMEA data (look for '$')
    bool has_nmea = false;
    for (uint32_t i = 0; i < got_len && i < 256; i++) {
        if (nmea_buf[i] == '$') { has_nmea = true; break; }
    }
    if (!has_nmea) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }

    // Feed into line-by-line NMEA parser
    for (uint32_t i = 0; i < got_len; i++) {
        char c = (char)nmea_buf[i];
        if (c == '\n' || c == '\r') {
            if (nmea_line_pos > 0) {
                nmea_line[nmea_line_pos] = '\0';
                process_nmea_line(nmea_line, &local_data);
                nmea_line_pos = 0;
            }
        } else if (nmea_line_pos < sizeof(nmea_line) - 1) {
            nmea_line[nmea_line_pos++] = c;
        }
    }

    // Thread-safe update of shared GPS data
    if (xSemaphoreTake(gps_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        memcpy(&current_gps_data, &local_data, sizeof(gps_data_t));
        xSemaphoreGive(gps_mutex);
    }
}
```

### 6.4 Timing Summary

| Operation                       | Delay After    | Notes                           |
|---------------------------------|----------------|---------------------------------|
| Power rails OFF                 | **5000 ms**    | Full capacitor discharge (v15L2: 2s insufficient after CW writes) |
| Power rails ON                  | 500 ms         | Voltage stabilization           |
| NRESET release                  | **5000 ms**    | LC76G boot time (v15L2: 3s insufficient for clean reset) |
| Bus reset                       | **50 ms**      | I2C recovery (v15L2: tuned 20/50/100ms — 50ms optimal) |
| TX(0x50) → RX(0x54)            | 10 ms minimum  | Command processing time         |
| WAKE cycle spacing              | 500 ms         | Between consecutive wakes       |
| Main loop iteration (no data)   | 100 ms         | Polling rate                    |
| Command write timing            | 100 ms         | Between config and data write   |

---

## 7. NMEA Parsing

### 7.1 Supported Sentence Types

| Sentence          | Talkers         | Parsed Fields                                |
|-------------------|-----------------|----------------------------------------------|
| `$xxRMC`          | GP, GN          | Time, status, lat, lon, speed, heading       |
| `$xxGGA`          | GP, GN          | Fix quality, satellites, HDOP, altitude      |
| `$xxGSV`          | GP, GN, GL, GA, GB | Visible satellite count per constellation |
| `$xxGSA`          | GP, GN          | Counted only                                 |
| `$xxGLL`          | GP, GN          | Counted only                                 |
| `$xxVTG`          | GP, GN          | Counted only                                 |
| `$PQTM*`         | —               | Proprietary Quectel responses (logged)       |
| `$xxTXT`          | GP, GN          | Module status messages (logged)              |

### 7.2 Coordinate Conversion

NMEA coordinates are in `DDmm.mmmm` format. Convert to decimal degrees:

```c
static double nmea_to_degrees(const char *val, const char *dir)
{
    if (val == NULL || val[0] == '\0') return 0.0;
    double raw = atof(val);
    int degrees = (int)(raw / 100.0);
    double minutes = raw - (degrees * 100.0);
    double result = degrees + (minutes / 60.0);
    if (dir && (dir[0] == 'S' || dir[0] == 'W')) result = -result;
    return result;
}
```

### 7.3 Speed Conversion

NMEA speed is in knots. Convert to km/h: `speed_kmh = speed_knots * 1.852`

---

## 8. CASIC Command Writing (Sending NMEA to Module)

To send an NMEA command (e.g., cold start `$PQTMCOLD*cs\r\n`) to the LC76G:

### 8.1 Build the NMEA Command String

```c
// Compute XOR checksum of body (between $ and *)
uint8_t cksum = 0;
for (const char *p = body; *p; p++) cksum ^= (uint8_t)*p;
snprintf(cmd_buf, sizeof(cmd_buf), "$%s*%02X\r\n", body, cksum);
```

### 8.2 Three-Step CASIC Write Protocol

```c
// Step 1: Query RX buffer free space
// TX to 0x50: CR_CMD + offset 0x0004 + length 4
uint8_t query[] = {0x04, 0x00, 0x51, 0xAA, 0x04, 0x00, 0x00, 0x00};
i2c_master_transmit(lc76g_handle, query, sizeof(query), 500);       // → 0x50
vTaskDelay(pdMS_TO_TICKS(100));
uint8_t free_buf[4] = {0};
i2c_master_receive(lc76g_read_handle, free_buf, sizeof(free_buf), 500); // ← 0x54
uint32_t free_space = free_buf[0] | (free_buf[1]<<8) | (free_buf[2]<<16) | (free_buf[3]<<24);

// Step 2: Config write — tell LC76G we're about to write cmd_len bytes
// TX to 0x50: CW_CMD + offset 0x1000 + cmd_len
uint8_t config_cmd[8] = {
    0x00, 0x10, 0x53, 0xAA,       // CW_CMD (0xAA53) + offset 0x1000
    (uint8_t)(cmd_len & 0xFF),    // length byte 0
    (uint8_t)((cmd_len >> 8) & 0xFF),
    (uint8_t)((cmd_len >> 16) & 0xFF),
    (uint8_t)((cmd_len >> 24) & 0xFF)
};
i2c_master_transmit(lc76g_handle, config_cmd, sizeof(config_cmd), 1000); // → 0x50
vTaskDelay(pdMS_TO_TICKS(100));

// Step 3: Write actual NMEA command data to 0x58
i2c_master_transmit(lc76g_dwr_handle, (uint8_t*)cmd_buf, cmd_len, 1000); // → 0x58
```

---

## 9. Public API

### 9.1 Data Structures

```c
typedef struct {
    double latitude;        // Decimal degrees (positive = N)
    double longitude;       // Decimal degrees (positive = E)
    float altitude;         // Meters above MSL
    float speed;            // km/h
    float heading;          // Degrees (0-360, true north)
    uint8_t satellites;     // Satellites used in fix
    uint16_t visible_sats;  // Total visible satellites
    float hdop;             // Horizontal dilution of precision
    float accuracy;         // Estimated accuracy in meters (hdop * 5)
    bool fix_valid;         // True if GPS has valid fix
    uint8_t fix_quality;    // 0=invalid, 1=GPS, 2=DGPS, 6=estimated
    uint8_t hour, minute, second;  // UTC time
} gps_data_t;

typedef struct {
    uint32_t total_bytes;       // Total NMEA bytes received
    uint32_t total_sentences;   // Total sentences parsed
    uint32_t cycle;             // Current polling cycle
    uint32_t consecutive_fails; // Consecutive I2C failures
    uint32_t warm_ups;          // WAKE sequences performed
    uint32_t cmds_sent;         // Commands sent
    uint32_t cmds_ok;           // Commands acknowledged
    uint32_t cnt_gga, cnt_rmc, cnt_gsv, cnt_gsa;
    uint32_t cnt_gll, cnt_vtg, cnt_txt, cnt_pqtm, cnt_other;
    char last_gga[128];         // Last raw GGA (debug)
    char last_rmc[128];         // Last raw RMC (debug)
} gps_debug_t;
```

### 9.2 Functions

| Function                   | Description                                          |
|----------------------------|------------------------------------------------------|
| `gps_handler_init()`       | Power cycle, create device handles, create mutex     |
| `gps_handler_start()`      | Launch gps_task on core 0, priority 5, 8192 stack    |
| `gps_handler_get_data()`   | Thread-safe copy of latest GPS data                  |
| `gps_handler_get_debug()`  | Thread-safe copy of debug/diagnostic counters        |
| `gps_handler_send_cold_start()` | Send `$PQTMCOLD` to force satellite re-acquisition |

---

## 10. Version History and Lessons Learned

### 10.1 GPS-H1.c — The Discovery Phase

**Key characteristics:**
- Multi-method auto-detection (tried 5 different I2C read methods)
- Extensive diagnostic test suite (Tests A through H)
- Used `transmit_receive` on 0x50 as primary method (method 2)
- No WAKE mechanism
- No PMIC power cycle

**What it proved:**
- `transmit_receive` on 0x50 does return data (repeated-START reads from 0x50)
- But this method is fragile and periodically corrupts the LC76G state
- The module can enter an unrecoverable state requiring power cycling

### 10.2 GPS-H2.c — The UART Detour

**Key characteristics:**
- Added UART support (GPIO17/18) hoping NMEA was routed there
- UART auto-detection (tried 9600 and 115200 baud)
- Full `gps_ensure_power()` with PMIC + TCA9554 control
- More robust TCA9554 FORCE_ON/NRESET handling

**What it proved:**
- On this board, LC76G NMEA data is NOT available on UART — I2C only
- The PMIC power cycle sequence is essential and correct
- TCA9554 P4/P5 control works and is required

### 10.3 gps_handler.c — The Working Version

**Key characteristics (what changed from H1/H2):**
- Pure TX(0x50) + RX(0x54) — no `transmit_receive` as primary method
- WAKE mechanism discovered and implemented (from Waveshare `Ql_Wake_I2C`)
- Activation recovery via `transmit_receive(0x50)` after 5 consecutive RX failures
- Bus reset before every cycle
- 10ms TX→RX delay
- Empty poll wake trigger (30 consecutive avail=0)
- Full PMIC power cycle on init

**Why it works:**
- Correct split-address protocol (TX 0x50, RX 0x54)
- WAKE mechanism keeps I2C slave active
- Power cycle guarantees clean state on boot
- Recovery mechanism handles transient failures

### 10.4 grok-GPS-i2c.c — Why External AI-Generated Code Failed

See Section 11 below.

---

## 11. Grok's Code — What Failed and Why

Grok (xAI) was given the `I2C_GPS-FIX.md` and `first-GPS_I2C_RESOLUTION.md`
documentation notes and asked to write a `gps_handler.c`. The code compiled
but **never produced NMEA data**. Here is exactly why:

### 11.1 Missing: WAKE Mechanism

Grok's code has zero wake-up logic. It goes straight to polling `casic_read_available()`.
Without the WAKE mechanism, the LC76G I2C slave stays idle and reports avail=0 forever.

**Working code has:** 3 wake cycles at startup + wake-on-empty-polls (30 cycle threshold).

### 11.2 Missing: Full PMIC Power Cycle

Grok's `gps_ensure_power()` is a stub:
```c
// Grok's version:
ESP_LOGI(TAG, "gps_ensure_power: waiting for GNSS stabilization (stub)");
vTaskDelay(pdMS_TO_TICKS(1800));
```

**Working code has:** Full AXP2101 register manipulation (0x90 bitmask 0x2C),
TCA9554 NRESET control, 2-second power-off, 3-second boot wait.

### 11.3 Missing: Bus Reset Per Cycle

Grok's main loop never calls `i2c_master_bus_reset()`. On a shared I2C bus with
touch and IMU constantly generating traffic, the bus can get stuck. Without
per-cycle bus reset, consecutive failures accumulate and never recover.

### 11.4 Missing: Recovery Mechanism

Grok's code logs "Too many consecutive failures — consider full power cycle"
but never actually does anything about it. No `transmit_receive(0x50)` activation,
no re-WAKE, no power cycle recovery.

### 11.5 Inadequate Timing

Grok used `TX_DELAY_MS = 12` (12ms after TX before RX). While the theoretical
minimum is 10ms, the Waveshare board with its shared bus needs reliable timing.
The working code uses exactly 10ms for data reads but 100ms for command writes.

### 11.6 Missing: I2C Bus Scan Protection

Grok's init doesn't skip LC76G addresses during bus scan. Probing 0x50/0x54/0x58
during scan can corrupt the module's state before the first real read attempt.

### 11.7 Incorrect Parsing

Grok's `parse_nmea_line()` checks `nmea_line_pos < 6` and `line[1] != 'G'` which
incorrectly filters `$PQTM` proprietary sentences. The working code handles all
valid `$`-prefixed sentences.

### 11.8 Summary: What the Notes Failed to Convey

The documentation focused on the CASIC byte protocol but didn't adequately
emphasize that three **separate, additional mechanisms** (power cycle, wake, and
activation recovery) are required beyond just "TX 0x50, RX 0x54". The CASIC
protocol alone is necessary but NOT sufficient.

---

## 12. Exact Byte Sequences Reference

### 12.1 Length Query (TX → 0x50)
```
{0x08, 0x00, 0x51, 0xAA, 0x04, 0x00, 0x00, 0x00}
 ├─ offset 0x0008 ─┤ ├─ CR_CMD 0xAA51 ┤ ├── length = 4 ──────┤
```

### 12.2 Data Read Request (TX → 0x50)
```
{0x00, 0x20, 0x51, 0xAA, <len_b0>, <len_b1>, <len_b2>, <len_b3>}
 ├─ offset 0x2000 ─┤ ├─ CR_CMD 0xAA51 ┤ ├── readLen (LE) ────┤
```

### 12.3 RX Buffer Free Space Query (TX → 0x50)
```
{0x04, 0x00, 0x51, 0xAA, 0x04, 0x00, 0x00, 0x00}
 ├─ offset 0x0004 ─┤ ├─ CR_CMD 0xAA51 ┤ ├── length = 4 ──────┤
```

### 12.4 Config Write (TX → 0x50)
```
{0x00, 0x10, 0x53, 0xAA, <len_b0>, <len_b1>, <len_b2>, <len_b3>}
 ├─ offset 0x1000 ─┤ ├─ CW_CMD 0xAA53 ┤ ├── write length (LE)┤
```

### 12.5 WAKE Sequence (TX → 0x50, then TX → 0x58)
```
Step 1 → 0x50: {0x00, 0x10, 0x53, 0xAA, 0x01, 0x00, 0x00, 0x00}
                 ├─ CW config: "writing 1 byte" ──────────────────┤
Step 2 → 0x58: {0x00}
                 ├─ dummy byte ──┤
```

### 12.6 Response from 0x54 (always 4 bytes, little-endian uint32)
```
{byte0, byte1, byte2, byte3} → value = byte0 | (byte1<<8) | (byte2<<16) | (byte3<<24)
```

---

## 13. Reproduction Checklist

Use this checklist to verify a new implementation will work. Every item is REQUIRED.

- [ ] I2C bus initialized at 100 kHz on GPIO15 (SDA) / GPIO14 (SCL)
- [ ] Three separate `i2c_master_dev_handle_t` created: 0x50, 0x54, 0x58
- [ ] **NO** `i2c_master_probe()` on 0x50, 0x54, or 0x58 during bus scan
- [ ] PMIC power cycle: AXP2101 reg 0x90 bitmask 0x2C (ALDO3+ALDO4+BLDO2) OFF **5s** → ON
- [ ] TCA9554 NRESET: P5 LOW during power-off → P5 HIGH after power-on
- [ ] **5-second** wait after NRESET release (LC76G boot time — v15L2: 3s insufficient)
- [ ] Device handles created AFTER power cycle completes
- [ ] WAKE × 3 at startup: CW config(0x50) + dummy write(0x58), 500ms spacing
- [ ] Initial avail check × 5: TX query(0x50) + RX(0x54), verify data flow
- [ ] Main loop: `i2c_master_bus_reset()` before every cycle
- [ ] Main loop: **50ms** delay after bus reset (v15L2: tuned optimal for shared bus)
- [ ] TX(0x50) then 10ms delay then RX(0x54) — NEVER `transmit_receive` on 0x54
- [ ] Length response sanity check: 0 < avail < 65536
- [ ] WAKE trigger after **5** consecutive avail=0 polls (v15L2: lowered from 30)
- [ ] **Per-read WAKE:** CW config(0x50) + dummy write(0x58) after every successful data read
- [ ] Activation recovery: `transmit_receive(0x50)` + **re-prime** (data_req + drain read) after every 5th consecutive fail
- [ ] **Primer at boot:** TxRx on 0x50 + data_req (CR_CMD offset 0x2000, 256 bytes) — REQUIRED
- [ ] **Primer in recovery:** Same primer sequence after every PMIC power cycle
- [ ] **NEVER** put CW write in RX fail path (poisons module response register → [4D4D4D4D])
- [ ] **NEVER** send data_req without drain read (poisons avail queries → [2C2C2C2C])
- [ ] NMEA line parser handles CR, LF, and CR+LF line endings
- [ ] Thread-safe data access via FreeRTOS mutex
- [ ] GPS task: core 0, priority 5, stack 8192 bytes
- [ ] `gps_handler_send_cold_start()` sends `$PQTMCOLD*xx\r\n` via CASIC write protocol

---

## 14. Elite Enhancements & 100% Throughput Roadmap

> **Status:** ROADMAP — these enhancements build on the proven production driver
> documented in Sections 1–13. Each subsection includes **Insights** from the
> March 2025 production guide + Quectel LC26G/LC76G I2C Application Note V1.0
> (the authoritative reference for firmware LC76GABNR12A03S), **Rationale** for
> why the original code was minimal, and **Implementation** with copy-paste-ready
> code that follows the existing code patterns exactly.
>
> **This section is written so that future developers (and LLMs) upgrading the
> driver use the same field names, function signatures, and error-handling patterns
> established in the working production code.**

---

### 14.1 Complete NMEA Parsing (GGA, GSV, GSA, VTG)

#### Insights

The production parser (Section 7) fully handles RMC and GGA, parses GSV for
visible-satellite totals, and counts GSA/GLL/VTG without extracting data. The
LC76G outputs all core sentences at every fix when enabled:

| Sentence | Parsed in Production | What's Missing |
|----------|---------------------|----------------|
| RMC | Full (time, status, lat, lon, speed, heading) | Date (year/month/day) |
| GGA | Full (quality, sats, HDOP, altitude) | — |
| GSV | Visible-sat total only | Per-constellation breakdown |
| GSA | Counted only | PDOP, VDOP, fix mode (2D/3D) |
| VTG | Counted only | Pure ground speed + track |
| GLL | Counted only | Redundant with RMC (skip) |

The protocol spec confirms talker prefixes: `GN` (multi-GNSS), `GP` (GPS),
`GL` (GLONASS), `GA` (Galileo), `GB` (BeiDou). GSV uses per-constellation
talkers while GSA/RMC/GGA use `GN` for multi-GNSS.

#### Rationale

The production code prioritized getting data flowing reliably over parsing
every field. Now that the I2C layer is proven, we can extract the full data
set without risk. Key principles:

- **Use the existing `nmea_get_field()` function** — it is non-destructive
  (never modifies the input string) and reentrant. Do NOT use `strtok()`,
  which modifies its input, is not thread-safe, and will corrupt NMEA data
  if any interrupt or other task calls it concurrently.
- **Keep existing `gps_data_t` field names** — `altitude`, `speed`, `heading`,
  `satellites`, `visible_sats`, `hdop`. Renaming breaks all consumers
  (UI manager, ESP-NOW broadcast, LVGL widgets, odometer guard).
- **Add new fields at the END of the struct** — maintains binary compatibility.

#### Implementation

**Step 1: Extend `gps_data_t` in `gps_handler.h`** (add after existing fields):

```c
typedef struct {
    /* ── Existing fields (DO NOT reorder or rename) ── */
    double latitude;
    double longitude;
    float altitude;
    float speed;
    float heading;
    uint8_t satellites;
    uint16_t visible_sats;
    float hdop;
    float accuracy;
    bool fix_valid;
    uint8_t fix_quality;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;

    /* ── New fields (14.1 enhancement) ── */
    float pdop;                   /**< Position DOP from GSA */
    float vdop;                   /**< Vertical DOP from GSA */
    bool fix_3d;                  /**< true if GSA reports 3D fix (mode=3) */
    uint16_t visible_gps;         /**< GPS satellites in view (from GPGSV) */
    uint16_t visible_glo;         /**< GLONASS in view (from GLGSV) */
    uint16_t visible_gal;         /**< Galileo in view (from GAGSV) */
    uint16_t visible_bds;         /**< BeiDou in view (from GBGSV) */
    uint16_t year;                /**< UTC year from RMC (optional) */
    uint8_t month;                /**< UTC month from RMC */
    uint8_t day;                  /**< UTC day from RMC */
} gps_data_t;
```

**Step 2: Add `parse_gsa()` and `parse_vtg()` in `gps_handler.c`**:

```c
/**
 * @brief Parse $GNGSA / $GPGSA — DOP values and fix mode.
 *
 * GSA format: $xxGSA,mode,fix,sv1..sv12,pdop,hdop,vdop*cs
 *   Field 2:  fix type (1=no fix, 2=2D, 3=3D)
 *   Field 15: PDOP
 *   Field 16: HDOP
 *   Field 17: VDOP (before checksum)
 */
static void parse_gsa(const char *sentence, gps_data_t *data)
{
    char field[32];

    /* Field 2: Fix mode */
    if (nmea_get_field(sentence, 2, field, sizeof(field)) && field[0]) {
        data->fix_3d = (atoi(field) == 3);
    }

    /* Field 15: PDOP */
    if (nmea_get_field(sentence, 15, field, sizeof(field)) && field[0]) {
        data->pdop = atof(field);
    }

    /* Field 16: HDOP (may refine GGA's value) */
    if (nmea_get_field(sentence, 16, field, sizeof(field)) && field[0]) {
        float gsa_hdop = atof(field);
        if (gsa_hdop > 0.0f) {
            data->hdop = gsa_hdop;
            data->accuracy = gsa_hdop * 5.0f;
        }
    }

    /* Field 17: VDOP (strip checksum if present) */
    if (nmea_get_field(sentence, 17, field, sizeof(field)) && field[0]) {
        data->vdop = atof(field);  /* atof stops at '*' naturally */
    }
}

/**
 * @brief Parse $GNVTG / $GPVTG — Track and ground speed.
 *
 * VTG format: $xxVTG,cog_true,T,cog_mag,M,sog_knots,N,sog_kmh,K,mode*cs
 *   Field 7:  Speed over ground in km/h (most useful)
 *   Field 1:  Course over ground, true (heading backup)
 */
static void parse_vtg(const char *sentence, gps_data_t *data)
{
    char field[32];

    /* Field 7: Speed in km/h (direct, no conversion needed) */
    if (nmea_get_field(sentence, 7, field, sizeof(field)) && field[0]) {
        float vtg_speed = atof(field);
        if (vtg_speed > 0.0f) {
            data->speed = vtg_speed;  /* VTG km/h is more direct than RMC knots */
        }
    }

    /* Field 1: Course over ground, true north (heading backup) */
    if (nmea_get_field(sentence, 1, field, sizeof(field)) && field[0]) {
        data->heading = atof(field);
    }
}
```

**Step 3: Enhance `parse_gsv()` for per-constellation breakdown**:

```c
/**
 * @brief Parse $xxGSV — Satellites in view, per constellation.
 *
 * GSV format: $xxGSV,numMsgs,msgNum,totalSats,PRN,elev,azim,SNR,...*cs
 *   Talker prefix determines constellation:
 *     $GP = GPS,  $GL = GLONASS,  $GA = Galileo,  $GB = BeiDou
 *   Field 3 = total visible satellites for THIS constellation.
 *   Only read from message 1 of N (field 2 == "1").
 */
static void parse_gsv(const char *sentence, gps_data_t *data)
{
    char field[32];

    /* Only process message 1 of the GSV sequence (has the total count) */
    char msgnum[8];
    if (!nmea_get_field(sentence, 2, msgnum, sizeof(msgnum)) || msgnum[0] != '1') {
        return;
    }

    /* Field 3: total satellites in view */
    if (!nmea_get_field(sentence, 3, field, sizeof(field)) || !field[0]) {
        return;
    }
    int sats = atoi(field);

    /* Route to correct constellation counter based on talker prefix.
     * sentence[0] = '$', sentence[1..2] = talker (GP/GL/GA/GB/GN) */
    if (strncmp(sentence, "$GP", 3) == 0) {
        data->visible_gps = sats;
        data->visible_sats = sats;  /* Reset total with GPS (first in cycle) */
    } else if (strncmp(sentence, "$GL", 3) == 0) {
        data->visible_glo = sats;
        data->visible_sats += sats;
    } else if (strncmp(sentence, "$GA", 3) == 0) {
        data->visible_gal = sats;
        data->visible_sats += sats;
    } else if (strncmp(sentence, "$GB", 3) == 0) {
        data->visible_bds = sats;
        data->visible_sats += sats;
    }
}
```

**Step 4: Update `process_nmea_line()` to call the new parsers**:

```c
    } else if (strncmp(type, "GSA", 3) == 0) {
        parse_gsa(line, data);           /* was: counted only */
        current_gps_debug.cnt_gsa++;
    } else if (strncmp(type, "GLL", 3) == 0) {
        current_gps_debug.cnt_gll++;     /* skip — redundant with RMC */
    } else if (strncmp(type, "VTG", 3) == 0) {
        parse_vtg(line, data);           /* was: counted only */
        current_gps_debug.cnt_vtg++;
    }
```

**Step 5: Add RMC date parsing** (optional, add inside existing `parse_rmc()`):

```c
    /* Field 9: Date (ddmmyy) */
    if (nmea_get_field(sentence, 9, field, sizeof(field)) && strlen(field) >= 6) {
        data->day   = (field[0] - '0') * 10 + (field[1] - '0');
        data->month = (field[2] - '0') * 10 + (field[3] - '0');
        data->year  = 2000 + (field[4] - '0') * 10 + (field[5] - '0');
    }
```

#### Performance Note

Each `nmea_get_field()` call walks the string from the beginning, which is
O(n × fields). For the short NMEA sentences (≤82 chars per NMEA 0183 spec),
this completes in < 10 µs per sentence on ESP32-S3 at 240 MHz. Total parsing
overhead for a full 10-sentence burst: < 100 µs. No impact on polling speed.

> **WARNING: Never use `strtok()` for NMEA parsing.** It modifies the input
> string (inserts NUL bytes), is not reentrant, and will silently corrupt data
> if called from multiple contexts. The existing `nmea_get_field()` is const-safe
> and works correctly on shared I2C bus data where lines may be partially
> corrupted. Every Grok-generated attempt (grok-GPS-i2c.c, grok-GPS-new.c) used
> `strtok()` — this is one of the patterns that marks non-production code.

---

### 14.2 Command Sending API (Rate + Constellation Control)

#### Insights

The production code already has the complete 3-step CASIC write implementation
in `lc76g_send_command()` (Section 6.3). It correctly builds checksums, queries
free space, sends the CW config header, and writes data to 0x58. The cold-start
command `$PQTMCOLD` is verified working.

The LC76G firmware (LC76GABNR12A03S) accepts both `$PQTM` and `$PAIR` command
families:

| Command | Purpose | Family |
|---------|---------|--------|
| `$PQTMCOLD` | Cold start (clear all stored data) | PQTM |
| `$PQTMWARM` | Warm start (keep ephemeris) | PQTM |
| `$PQTMHOT`  | Hot start (keep everything) | PQTM |
| `$PAIR050,100` | Set fix rate to 10 Hz (100ms interval) | PAIR |
| `$PAIR050,1000` | Set fix rate to 1 Hz (default) | PAIR |
| `$PAIR066,1,1,1,1,0,0` | Enable GPS+GLO+GAL+BDS | PAIR |

> **CAUTION:** The `$PAIR` command family originates from the Airoha AG3335
> chipset (used in some Quectel variants). The LC76G supports a subset. If a
> `$PAIR` command is not recognized, the module silently ignores it — no error,
> no ACK. **Always verify command acceptance by checking for a `$PAIR001` ACK
> response** in the NMEA stream after sending. If no ACK appears within 2
> seconds, the command is not supported on your firmware version.

#### Rationale

The production code only exposes `gps_handler_send_cold_start()`. For elite
operation we need rate control and constellation selection. The existing
`lc76g_send_command()` already does the heavy lifting — we just need thin
wrappers that build the correct command body.

#### Implementation

**Add to `gps_handler.h`**:

```c
/** Set GNSS fix output rate (1-10 Hz). Sends $PAIR050 via CASIC write. */
esp_err_t gps_handler_set_rate_hz(uint8_t hz);

/** Enable/disable constellations. Sends $PAIR066 via CASIC write. */
esp_err_t gps_handler_set_constellations(bool gps, bool glonass, bool galileo, bool beidou);

/** Send a warm start command ($PQTMWARM). */
esp_err_t gps_handler_send_warm_start(void);
```

**Add to `gps_handler.c`** (uses existing `lc76g_send_command()`):

```c
esp_err_t gps_handler_set_rate_hz(uint8_t hz)
{
    if (hz < 1 || hz > 10) {
        ESP_LOGE(TAG, "Invalid rate %d — must be 1-10 Hz", hz);
        return ESP_ERR_INVALID_ARG;
    }
    char body[24];
    uint16_t interval_ms = 1000 / hz;
    snprintf(body, sizeof(body), "PAIR050,%u", interval_ms);
    ESP_LOGI(TAG, "Setting GPS rate to %d Hz (%u ms)", hz, interval_ms);
    return lc76g_send_command(body);
}

esp_err_t gps_handler_set_constellations(bool gps, bool glonass, bool galileo, bool beidou)
{
    char body[48];
    snprintf(body, sizeof(body), "PAIR066,%d,%d,%d,%d,0,0",
             gps ? 1 : 0, glonass ? 1 : 0, galileo ? 1 : 0, beidou ? 1 : 0);
    ESP_LOGI(TAG, "Setting constellations: GPS=%d GLO=%d GAL=%d BDS=%d",
             gps, glonass, galileo, beidou);
    return lc76g_send_command(body);
}

esp_err_t gps_handler_send_warm_start(void)
{
    return lc76g_send_command("PQTMWARM");
}
```

**When to call (from `gps_task`, after first successful NMEA data)**:

```c
if (!ever_received) {
    ESP_LOGI(TAG, "=== FIRST GPS DATA! ===");
    ever_received = true;

    /* Configure 10 Hz after first data — module is alive and listening */
    gps_handler_set_rate_hz(10);
    gps_handler_set_constellations(true, true, true, true);
}
```

#### ACK Verification

After sending a `$PAIR` command, the LC76G should respond with:
```
$PAIR001,050,0*cs     ← 0 = success
$PAIR001,050,1*cs     ← 1 = unsupported
```

To verify, add handling in `process_nmea_line()`:

```c
    } else if (strncmp(line + 1, "PAIR", 4) == 0) {
        current_gps_debug.cnt_pqtm++;  /* reuse counter for proprietary */
        ESP_LOGI(TAG, "  [PAIR ACK] %s", line);
        /* Parse result: field 2 = "0" means success */
        char result[4];
        if (nmea_get_field(line, 2, result, sizeof(result)) && result[0] == '0') {
            current_gps_debug.cmds_ok++;
        }
    }
```

---

### 14.3 Tiered Error Recovery + Diagnostics

#### Insights

The production driver has two recovery mechanisms:
1. **Activation** — `transmit_receive(0x50)` every 5th consecutive RX failure
2. **WAKE** — CW config(0x50) + dummy(0x58) after 30 empty polls

These handle 99%+ of real-world situations. The missing tier is a **full PMIC
power cycle** for catastrophic failure (the "poisoned I2C state machine" case
documented in Section 4). Currently, only a reboot triggers this.

#### Rationale

Adding a power-cycle recovery tier requires care:
- The 0x50/0x54/0x58 device handles become **stale** after a PMIC power cycle
  because the LC76G I2C slave resets its internal state.
- You **must** remove and re-add the device handles around the power cycle,
  otherwise ESP-IDF's I2C driver may hold stale bus arbitration state.
- This is why the production code only does power cycle in `gps_handler_init()`
  BEFORE creating device handles.

#### Implementation

**Extend `gps_debug_t` in `gps_handler.h`** (add after existing fields):

```c
typedef struct {
    /* ── Existing fields (DO NOT reorder) ── */
    uint32_t total_bytes;
    uint32_t total_sentences;
    uint32_t cycle;
    uint32_t consecutive_fails;
    uint32_t warm_ups;
    uint32_t cmds_sent;
    uint32_t cmds_ok;
    uint32_t cnt_gga, cnt_rmc, cnt_gsv, cnt_gsa;
    uint32_t cnt_gll, cnt_vtg, cnt_txt, cnt_pqtm, cnt_other;
    char last_gga[128];
    char last_rmc[128];

    /* ── New fields (14.3 enhancement) ── */
    uint32_t total_wakes;           /**< Number of WAKE sequences performed */
    uint32_t total_activations;     /**< Activation transmit_receive count */
    uint32_t total_power_cycles;    /**< Mid-run PMIC power cycle count */
    uint32_t total_nacks;           /**< Total I2C NACKs on 0x54 */
    uint32_t max_consecutive_fails; /**< High-water mark for fail streaks */
} gps_debug_t;
```

**Add power-cycle recovery function in `gps_handler.c`**:

```c
/**
 * @brief Full recovery: remove device handles, power cycle, re-add handles.
 *
 * This is the LAST RESORT — only triggered after 100+ consecutive RX failures
 * (v15L2: raised from 50 to allow more time for re-prime/drain to recover).
 * Indicates the LC76G I2C state machine is irrecoverably poisoned.
 *
 * CRITICAL: Device handles must be removed before power cycle and re-created
 * after, because the LC76G resets its I2C slave endpoints during power loss.
 *
 * v15L2 ADDITION: After re-creating handles, run primer sequence:
 *   3x WAKE (CW + 0x58) → TxRx on 0x50 → data_req + drain read
 */
static void gps_full_recovery(i2c_master_bus_handle_t bus)
{
    ESP_LOGE(TAG, "=== FULL RECOVERY: 100+ failures — PMIC power cycle ===");

    /* Step 1: Remove stale device handles */
    if (lc76g_handle)      { i2c_master_bus_rm_device(lc76g_handle);      lc76g_handle = NULL; }
    if (lc76g_read_handle) { i2c_master_bus_rm_device(lc76g_read_handle); lc76g_read_handle = NULL; }
    if (lc76g_dwr_handle)  { i2c_master_bus_rm_device(lc76g_dwr_handle);  lc76g_dwr_handle = NULL; }

    /* Step 2: Full PMIC power cycle (same as init) */
    gps_ensure_power(bus);

    /* Step 3: Re-create device handles */
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = LC76G_I2C_ADDR_WRITE,
        .scl_speed_hz = LC76G_I2C_CLK_HZ,
    };
    i2c_master_bus_add_device(bus, &cfg, &lc76g_handle);

    cfg.device_address = LC76G_I2C_ADDR_READ;
    i2c_master_bus_add_device(bus, &cfg, &lc76g_read_handle);

    cfg.device_address = LC76G_I2C_ADDR_DATA_WR;
    i2c_master_bus_add_device(bus, &cfg, &lc76g_dwr_handle);

    /* Step 4: Re-run WAKE sequence (module just booted) */
    for (int w = 0; w < 3; w++) {
        i2c_master_bus_reset(bus);
        vTaskDelay(pdMS_TO_TICKS(50));
        uint8_t wake_cw[] = {0x00, 0x10, 0x53, 0xAA, 0x01, 0x00, 0x00, 0x00};
        i2c_master_transmit(lc76g_handle, wake_cw, sizeof(wake_cw), 500);
        vTaskDelay(pdMS_TO_TICKS(10));
        uint8_t dummy = 0x00;
        i2c_master_transmit(lc76g_dwr_handle, &dummy, 1, 500);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    /* Step 5 (v15L2): Primer — TxRx + data_req + drain read */
    uint8_t primer_r[4] = {0};
    i2c_master_transmit_receive(lc76g_handle,
        (uint8_t[]){0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00},
        8, primer_r, 4, 1000);
    vTaskDelay(pdMS_TO_TICKS(10));
    /* data_req: CR_CMD to offset 0x2000, read 256 bytes */
    uint8_t data_req[] = {0x00, 0x20, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00};
    i2c_master_transmit(lc76g_handle, data_req, sizeof(data_req), 500);
    vTaskDelay(pdMS_TO_TICKS(10));
    /* DRAIN: read 256 bytes to prevent avail poisoning */
    uint8_t drain[256];
    i2c_master_receive(lc76g_read_handle, drain, 256, 1000);
    vTaskDelay(pdMS_TO_TICKS(200));

    current_gps_debug.total_power_cycles++;
    ESP_LOGE(TAG, "=== FULL RECOVERY COMPLETE (with primer+drain) ===");
}
```

**Updated recovery tiers in main loop** (replace the RX failure block):

```c
        if (rx_ret != ESP_OK) {
            consecutive_fails++;
            current_gps_debug.total_nacks++;

            /* Track high-water mark */
            if (consecutive_fails > current_gps_debug.max_consecutive_fails) {
                current_gps_debug.max_consecutive_fails = consecutive_fails;
            }

            /* ── Tier 1: Activation + re-prime + drain (every 5 RX fails) ── */
            /* v15L2: TxRx on 0x50 wakes the module, then data_req + drain
             * prevents avail poisoning ([2C2C2C2C] response). */
            if (consecutive_fails > 0 && (consecutive_fails % 5 == 0)) {
                uint8_t act_r[4] = {0};
                esp_err_t act = i2c_master_transmit_receive(lc76g_handle,
                    queryData, sizeof(queryData), act_r, 4, 1000);
                /* Re-prime: data_req + drain read */
                uint8_t data_req[] = {0x00, 0x20, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00};
                i2c_master_transmit(lc76g_handle, data_req, sizeof(data_req), 500);
                vTaskDelay(pdMS_TO_TICKS(10));
                uint8_t drain[256];
                i2c_master_receive(lc76g_read_handle, drain, 256, 1000);
                ESP_LOGW(TAG, "  Tier1 Activation+RePrime(0x50): %s", esp_err_to_name(act));
                current_gps_debug.total_activations++;
            }

            /* ── Tier 2: Full power cycle (at 100 failures) ── */
            /* v15L2: raised from 50 — gives re-prime more chances to recover
             * before expensive power cycle. */
            if (consecutive_fails >= 100) {
                gps_full_recovery(bus);
                consecutive_fails = 0;
            }

            cycle++;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
```

#### Cycle Timing for Maximum Throughput

The production loop timing breakdown:

| Phase | Time | Notes |
|-------|------|-------|
| `i2c_master_bus_reset()` | ~1 ms | 9 SCL clocks at 100 kHz |
| Post-reset delay | **50 ms** | Shared bus stabilization (v15L2: 20→50ms optimal) |
| TX query (8 bytes @ 100 kHz) | ~1 ms | Including START/STOP |
| TX→RX delay | 10 ms | Minimum tested stable |
| RX response (4 bytes @ 100 kHz) | ~0.5 ms | |
| TX data request + RX NMEA | Variable | ~5-50 ms depending on buffer size |
| **Minimum cycle (no data)** | **~63 ms** | Reset + 50ms delay + TX + delay + RX |
| **Minimum cycle (with data)** | **~80-110 ms** | Plus data transfer + parsing |

At **10 Hz fix rate** (100 ms between NMEA bursts), an ~80 ms poll cycle
is tight but achievable. At **1 Hz** (default), the 100 ms `IDLE_BACKOFF`
delay in the production code is appropriate.

**To switch to aggressive polling when 10 Hz is active**, reduce the no-data
delay from 100 ms to 30 ms:

```c
        if (dataLength == 0) {
            empty_polls++;
            if (empty_polls >= 5) { /* WAKE — v15L2 threshold */ }

            /* Aggressive poll when 10 Hz is active, relaxed at 1 Hz */
            uint32_t backoff = (rate_hz >= 10) ? 30 : 100;
            vTaskDelay(pdMS_TO_TICKS(backoff));
            continue;
        }
```

> **NOTE:** The "0 lost bytes at 10 Hz" claim requires field verification on
> your specific board. The LC76G's I2C buffer is finite (~4096 bytes). If the
> poll loop stalls (e.g., LVGL render spike on the shared core), data CAN be
> lost. The 4096-byte `GPS_NMEA_BUF_SIZE` in the production code accommodates
> ~4 seconds of 1 Hz NMEA data, or ~0.4 seconds at 10 Hz. This is adequate
> margin for typical FreeRTOS scheduling jitter.

---

### 14.4 Integration with LVGL UI (Production Dashboard)

#### Insights

The Waveshare conversion guide (Section 6) documents LVGL 9.2 setup with
double-buffered RGB888  PSRAM and partial render mode. The GPS data is ideal
for real-time dashboard widgets (speed, satellite count, HDOP gauge,
constellation status indicators).

#### Rationale

**Critical FreeRTOS + LVGL rules:**

1. **Never call LVGL from the GPS task.** LVGL is not thread-safe. All widget
   updates must happen from the thread that owns the LVGL context.
2. **Use LVGL's threading API** — LVGL 9 provides `lv_lock()` / `lv_unlock()`
   for cross-thread access. If your UI task is LVGL's only caller, use the
   mutex-protected `gps_handler_get_data()` from that task instead.
3. **Use `xTaskCreatePinnedToCore()`** per project convention (all tasks pinned
   to core 0 on this board).
4. **LVGL 9 color API** uses `lv_palette_main()` and `lv_color_make()`, NOT
   `lv_color_green()` / `lv_color_gray()` (which don't exist in LVGL 9).

#### Implementation

**GPS overlay update function** (add to `ui_manager.c` or equivalent):

```c
#include "gps_handler.h"
#include "lvgl.h"

/* Widget handles (created during UI init) */
static lv_obj_t *speed_label  = NULL;
static lv_obj_t *sats_label   = NULL;
static lv_obj_t *hdop_bar     = NULL;
static lv_obj_t *gps_icon     = NULL;
static lv_obj_t *glo_icon     = NULL;
static lv_obj_t *gal_icon     = NULL;
static lv_obj_t *bds_icon     = NULL;

/**
 * @brief Update GPS widgets from latest data.
 *
 * Called from the UI task (same thread as LVGL tick), so LVGL calls are safe.
 * If called from another thread, wrap with lv_lock()/lv_unlock().
 */
static void ui_gps_update(void)
{
    gps_data_t gps;
    if (gps_handler_get_data(&gps) != ESP_OK) return;

    /* Speed display */
    if (speed_label) {
        lv_label_set_text_fmt(speed_label, "%.0f", gps.speed);
    }

    /* Satellite count */
    if (sats_label) {
        lv_label_set_text_fmt(sats_label, "%d/%d",
                              gps.satellites, gps.visible_sats);
    }

    /* HDOP quality bar (0-50 range → 0-100 bar) */
    if (hdop_bar) {
        int bar_val = (int)(gps.hdop * 20.0f);  /* HDOP 5.0 = 100% */
        if (bar_val > 100) bar_val = 100;
        lv_bar_set_value(hdop_bar, 100 - bar_val, LV_ANIM_OFF);  /* invert: lower HDOP = better */
    }

    /* Constellation indicators (green = satellites visible, gray = none) */
    lv_color_t active = lv_palette_main(LV_PALETTE_GREEN);
    lv_color_t inactive = lv_palette_main(LV_PALETTE_GREY);

    if (gps_icon) lv_obj_set_style_bg_color(gps_icon, gps.visible_gps > 0 ? active : inactive, 0);
    if (glo_icon) lv_obj_set_style_bg_color(glo_icon, gps.visible_glo > 0 ? active : inactive, 0);
    if (gal_icon) lv_obj_set_style_bg_color(gal_icon, gps.visible_gal > 0 ? active : inactive, 0);
    if (bds_icon) lv_obj_set_style_bg_color(bds_icon, gps.visible_bds > 0 ? active : inactive, 0);
}
```

**GPS health debug panel** (for development/diagnostics):

```c
static lv_obj_t *debug_label = NULL;

static void ui_gps_debug_update(void)
{
    gps_debug_t dbg;
    if (gps_handler_get_debug(&dbg) != ESP_OK) return;

    if (debug_label) {
        lv_label_set_text_fmt(debug_label,
            "Bytes: %lu  Sentences: %lu\n"
            "GGA:%lu RMC:%lu GSV:%lu GSA:%lu VTG:%lu\n"
            "Fails: %lu  Wakes: %lu  PowerCycles: %lu\n"
            "NACKs: %lu  MaxFail: %lu",
            (unsigned long)dbg.total_bytes,
            (unsigned long)dbg.total_sentences,
            (unsigned long)dbg.cnt_gga, (unsigned long)dbg.cnt_rmc,
            (unsigned long)dbg.cnt_gsv, (unsigned long)dbg.cnt_gsa,
            (unsigned long)dbg.cnt_vtg,
            (unsigned long)dbg.consecutive_fails,
            (unsigned long)dbg.total_wakes,
            (unsigned long)dbg.total_power_cycles,
            (unsigned long)dbg.total_nacks,
            (unsigned long)dbg.max_consecutive_fails);
    }
}
```

**Task setup** (call from `app_main()` after `ui_manager_init()`):

```c
static void ui_update_task(void *arg)
{
    while (1) {
        ui_gps_update();
        ui_gps_debug_update();  /* remove in production */
        vTaskDelay(pdMS_TO_TICKS(50));  /* 20 Hz UI refresh */
    }
}

/* In init sequence: */
xTaskCreatePinnedToCore(ui_update_task, "ui_upd", 4096, NULL, 4, NULL, 0);
```

> **Note:** 20 Hz UI refresh (`vTaskDelay(50)`) is optimal for human perception
> on this 466×466 panel. Higher rates waste CPU without visible benefit. The GPS
> data itself updates at 1-10 Hz depending on configured rate.

---

### 14.5 Implementation Checklist — Elite Upgrade

Add these items to the Section 13 reproduction checklist when implementing
the elite enhancements:

- [ ] `gps_data_t` extended with new fields **at the end** (pdop, vdop, fix_3d, per-constellation sats, date)
- [ ] `gps_debug_t` extended with recovery counters (total_wakes, total_activations, total_power_cycles, total_nacks)
- [ ] `parse_gsa()` extracts PDOP/VDOP/fix_mode from fields 2, 15, 16, 17
- [ ] `parse_vtg()` extracts speed (km/h, field 7) and heading (field 1)
- [ ] `parse_gsv()` routes satellite counts by talker prefix (`$GP`/`$GL`/`$GA`/`$GB`)
- [ ] `parse_rmc()` extracts date from field 9 (ddmmyy)
- [ ] `nmea_get_field()` used for ALL parsing — **no `strtok()`**
- [ ] `gps_handler_set_rate_hz()` sends `$PAIR050,<ms>` via existing `lc76g_send_command()`
- [ ] `gps_handler_set_constellations()` sends `$PAIR066,<g>,<l>,<e>,<b>,0,0`
- [ ] `$PAIR001` ACK response parsed in `process_nmea_line()` to verify command acceptance
- [ ] Tier 3 recovery (`gps_full_recovery()`) removes device handles BEFORE power cycle, re-adds AFTER
- [ ] LVGL updates use `lv_palette_main()` (NOT `lv_color_green()`)
- [ ] LVGL updates happen ONLY from the UI task thread (never from GPS task)
- [ ] UI update task pinned to core 0 with `xTaskCreatePinnedToCore()`
- [ ] 10 Hz rate set AFTER first successful NMEA data (not at init)
- [ ] `$PAIR` commands verified via ACK before relying on new rate

---

### 14.6 Grok-GPS-new.c Failure Analysis

A second Grok attempt (`gps/main/grok-GPS-new.c`, 497 lines) was generated
from this guide. While structurally better than the first attempt
(grok-GPS-i2c.c), it still contains critical errors:

| Bug | Line | Impact |
|-----|------|--------|
| `strtok((char*)line, ",")` in RMC parser | ~388 | Destructive, not thread-safe, strips const |
| GGA counted but never parsed | ~435 | No satellite count, HDOP, or altitude extracted |
| `memcpy(&current_gps_data, &current_gps_data, ...)` | ~283 | Self-copy no-op — data never actually published |
| `#include "axp2101.h"` / `#include "tca9554.h"` | 29-30 | Headers don't exist — code uses inline helpers |
| `gps_handler_start()` returns `void` | ~120 | Breaks API contract (header says `esp_err_t`) |
| `gps_handler_send_cold_start()` returns `void` | ~135 | Breaks API contract |
| GSV counted with `strstr(line, "$GSV")` | ~443 | Wildcard match — would match `$XYZGSV` or any embedded `$GSV` |
| No `gps_handler_start()` function guards | ~120 | Missing NULL check on task handle for double-start protection |

**Root cause of the self-copy bug:** Line 283 copies `current_gps_data` to
itself instead of copying `local_data` to `current_gps_data`. The GPS task
parses into `current_gps_data` directly (not a local copy), then the "publish"
step does nothing. Combined with the unparsed GGA, consumers get partial data
(RMC fields only, no sats/altitude/HDOP).

**This validates the guide's completeness test:** the guide documented the I2C
protocol and recovery mechanisms correctly (both Grok attempts got those parts
right), but did not adequately document the code-level patterns (safe parsing,
return types, local-vs-global data flow). Sections 14.1–14.4 above close
that gap.

---

> **This document is the single source of truth for the OpenDash LC76G GPS
> driver. It supersedes `I2C_GPS-FIX.md`, `first-GPS_I2C_RESOLUTION.md`,
> `grok-cant-see.md`, and all `.bak` / `.working` files. Any LLM should be
> able to produce a working `gps_handler.c` from this document alone.**
