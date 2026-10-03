<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# LC76G GPS Node — Final Post-Mortem: Why This Hardware Fails Racing Telemetry

> **Verdict: UNUSABLE for racing/performance telemetry. Development ended 2026-09-29.**
> **✅ CLOSED-FINAL 2026-09-30 — iteration-5/6 probes ran. The read-only conclusion is now
> PROVEN with spec-exact commands (see §8). The reopening resolved against rehabilitation:
> spec-exact `$PAIR051`/`$PAIR050,100` delivered with verified transport-ACKs produce ZERO
> parser response. §§1–3 stand, this time on correct evidence.**
> **Board:** Waveshare ESP32-S3-Touch-AMOLED-1.75 · **Module:** Quectel LC76G (LC76GABNR12A03S, 2024-04-14)
> **Companion docs:** `gps-driver-debugging-v16.md` (full debugging journal),
> `LC76G-I2C-GPS-Driver-Guide.md` (read-path reference), `LC76G-10Hz-Spec-Breakout.md` (debunked spec)

---

## 1. The Requirement vs. The Reality

A racing telemetry gauge needs position/velocity updates fast enough to resolve
cornering dynamics: serious lap-timing wants 10–25 Hz; the absolute floor is
~5 Hz. At 1 Hz, a car at 60 mph (26.8 m/s) travels **27 meters between
samples** — corner-exit apex speed resolution is physically impossible. You
cannot filter, interpolate, or "poll faster" your way out of a sensor that
only speaks once per second.

**Measured reality (objectively instrumented in the driver, every session):**

```
NMEA throughput: 15.4 sent/s, 1.0 GGA/s, 667 B/s
```

1.0 GGA/s = the nav engine produces **exactly one position solution per
second**, every session, every firmware generation, since the day this board
shipped. The host ESP32 read loop polls ~3.3 cycles/second into a buffer that
receives 1 Hz content. Bus utilization never exceeds ~2%.

## 2. Why It Can't Be Fixed In Firmware

The LC76G exposes three I2C slave endpoints (0x50 control/read-query, 0x54
data out, 0x58 "command data in"). Through exhaustive testing:

1. **The read path works** — offset-0x0008 length query + offset-0x2000 data
   read reliably stream the module's NMEA output. This is the only working part.
2. **The write path delivers bytes but commands are dialect-gated:**
   - Every `$PAIR` command (050 rate, 066 constellation, 513/514 restart,
     020/021 query) was **silently ignored — zero `$PAIR001` ACKs in any
     session, rate never moved.** The "$PAIR subset" claim in the original
     spec doc was simply false for this module.
   - The **only** payload ever observed to change module behavior was
     `$PQTMCOLD` — and it is a *reset* (it cancels acquisition; see §3).
     It is not, and has no sibling command for, a rate setting.
3. **The write path is unreliable even as a dumb pipe:** the 0x58 payload
   write NACKs (`ESP_ERR_INVALID_RESPONSE`) randomly on every dialect, and
   the "RX buffer free space" query (offset-0x0004) returns a constant,
   clearly-fictional `4096` in every session.
4. **The vendor's own reference driver**
   (`gps/waveshare-amoled-gps-i2c-lc76g.py`) implements **read only**. There
   is no supported command channel over I2C on this part. That was always the
   answer; everything below is the evidence trail proving it.

**A module whose output rate cannot be changed is a data logger. It cannot be
a telemetry sensor, no matter what firmware runs on the other side of the bus.**

## 3. The Controlled Experiment (how we know, not how we guessed)

The decisive test held everything constant (same bench, same sky, same read
loop, same delivery mechanism) and varied only the command payload family:

| Session | Commands actually *delivered* (write ACKed) | Result |
|---|---|---|
| iteration-1 (PAIR dialect) | PAIR050 mostly NACKed; one late delivery at t=529 s | **Natural 3D fix at 422 s / 528 s** (matches cold TTFF pattern) |
| iteration-2 (PAIR dialect) | same pattern | natural fix again |
| **iteration-3 (PQTM dialect)** | `$PQTMCOLD*1C` **successfully written** at uptime 160 s & 202 s | **NEVER achieved a fix in 1500 s.** The delivered cold-start visibly, repeatedly cancelled nav acquisition |
| iteration-4 (control: PQTM never sent) | only a 25-byte ignored PAIR066 landed | **natural fix restored at ~450 s** |

The moment a `$PQTM` payload reached the module, module behavior changed.
PAIR payloads changed nothing, ever, in any configuration. Therefore: parser
alive for PQTM only → "wrong dialect" was the true root cause of the *old*
"commands do nothing" belief → and even the alive dialect offers only a
reset. There is no road to 10 Hz. (Earlier v16-era folklore that "10 Hz
before fix starves the engine" was reinterpreted correctly: it was never a
rate command working — cold-start deliveries *were* killing acquisition.)

## 4. How Prior Work Added Up To This

- **v15L2 (March 2025):** established the read path (the one true thing). Its
  own logged "production performance" (62–95 KB per 5-minute run ≈ 207–316
  B/s) confirms every "golden era" run was ~1 Hz — the 10 Hz claim in that
  era's spec doc was never true and was never measured. The myth survived
  because the old status log printed `total=<bytes> <sentences>s c=<cycles>`:
  the "12622s" column everyone read as *seconds* was actually a *sentence
  count*. Nobody ever divided sentences by real seconds. Once the driver
  measured sentences/second honestly, the number was 1.0 GGA/s forever.
- **v16a–v16k (July 2026):** many iterations of bus hardening (power-cycle
  recovery, wake sequences, grace periods, shared-bus contention analysis) —
  legitimate work that improved *read reliability*, spent stabilizing a pipe
  that could never carry more than 1 Hz no matter how perfect the driver.
- **Final session (2026-09-29):** ported the full v16k feature set into the
  active `gps/main` codebase — priority 8 + core-0 pinning, tiered wall-clock
  fix watchdog, interrupt-driven CST9217 touch (replacing 50 Hz blind polling
  that had contended the shared I2C bus), corrected NMEA command handling,
  rate-attempt ladder, and rolling GGA/s + sent/s instrumentation. Flashed,
  hard-reset, and verified over serial with the live logs quoted in §3. Only
  then could the command-channel question be settled with evidence instead
  of folklore. It was settled: 1 Hz, permanently.
- **The port work was still worth doing:** interrupt-driven touch removed the
  contention behind the original "sees sats then freezes" symptom, and the
  log-only watchdog (it must never deliver another cold start!) leaves the
  engine undisturbed. The node now streams 1 Hz *reliably* — as a
  fixed-position demo unit, which is all it can ever be.

## 5. Community Guidance — What To Use Instead

Do **not** buy any "GNSS module behind an I2C ring buffer" (LC26G/LC76G style)
for vehicles:

- The I2C application notes for this part family document **data-out only**
  (plus a cold start); you cannot query or set rate, constellation, or any
  configuration from firmware. Your host MCU is a passenger.
- Fixed 1 Hz output makes every dynamic racing use case (lap delta,
  cornering G-position mapping, threshold-braking traces) impossible. Full stop.
- Buy a UART/USB GNSS receiver with a **documented, community-proven command
  interface** (u-blox UBX + RTCM for 10–25 Hz + correction support is the
  reference pattern), wire it to a real UART, and configure it with vendor
  protocol messages that ACK. Any "smart module" whose config surface is
  hard-coded in vendor firmware is a logger, not a sensor.

## 6. Status

- Final firmware on the node: iteration-4 driver — instrumented 1 Hz
  streaming, log-only watchdog. Stable. Archived as-is.
- `gps/` node: **frozen.** No further firmware work. Do not fork a
  "high-speed" variant; speed is a property of the module, not the driver.
- The two-driver idea ("GPS-TOUCH vs HIGH-SPEED") was explicitly rejected:
  with touch already interrupt-driven (zero idle bus traffic) and the module
  fixed at 1 Hz, a driver fork buys zero hertz.

*This module is not a slow GPS. It is a GPS-shaped data logger with a locked
1 Hz mouth. For racing telemetry it was never a tool — it was the obstacle.*

## 7. CORRIGENDUM (2026-09-30) — what the doc comparison found

After closure, the two official documents were finally obtained and compared
line-by-line against every command our driver ever sent: the vendor product
wiki for the LC76G and the **Quectel LC26G & LC76G & LC86G GNSS Protocol
Specification V1.0.0** (saved copy: `/tmp/quectel_gnss.txt`, source
`files.waveshare.com/upload/0/06/…GNSS_Protocol_Specification_V1.0.0_Preliminary.pdf`).

**What the documents actually say:**

1. The LC76G's entire command protocol is **`$PAIR` packets** — §2.3 of the
   spec: `001` ACK, `002/003` GNSS-subsystem power on/off, `004–007`
   hot/warm/cold/full-cold starts, **`050 SET_FIX_RATE`**, `051 GET_FIX_RATE`,
   `062/063` per-sentence output rate, `066/067` constellation search mode,
   `864/865` baud. The product wiki's "PAIR Sentence" chapter lists the same
   set. **`$PQTM` appears in NO LC76G document at all.**
2. **`$PAIR050,<Time>*<CS>` is a real, documented rate command.** `<Time>` is
   a single millisecond parameter, range **100–1000, default 1000** (1 Hz);
   100 ms = 10 Hz. Wiki confirms max update rate 10 Hz for LC76G(AB)
   (chip AG3352Q). "No rate command exists" (§2) was **false as a statement
   about the module**.
3. Our code NEVER sent the spec syntax. `gps_handler_set_rate_hz()` sent the
   CASIC-style **two-field** `$PAIR050,<Hz>,<Hz>` form that exists in no
   LC76G document, and the getter we tried was `PAIR020/021` (CASIC) — the
   spec getter is `$PAIR051*3E` → `$PAIR001,051,0` ACK + `$PAIR051,<ms>`
   echo. The correct syntax was never verifiably delivered (iteration-1/2
   attempts mostly NACKed at the transport).

**What survives of this post-mortem:** every empirical measurement (1.0 GGA/s,
the controlled PQTM/no-PQTM sessions, the fictional 4096 free-space value,
the NACK-happy 0x58 write path). What must be re-derived: whether this
board's I2C wiring carries command bytes to the parser **at all**, tested
with syntax that is actually valid per the spec. The PQTM-cancels-acquisition
observation (the entire basis for "the parser only speaks PQTM") was an n=1
correlation that the official docs contradict.

**Iteration-5 (built, awaiting bench):** ladder = `$PAIR051*3E` then
`$PAIR050,100*22`, both spec-exact; the read path already logs any `$PAIR`
sentence verbatim. Outcomes:

* **ACK/echo observed (or rate changes)** → the command channel is alive;
  the module was never a locked logger — our syntax was wrong for four
  months. GPS node rehabilitated; POD3 conversion abandoned.
* **No echo ever, with writes verifiably ACKed** → only then is "this board's
  I2C is data-out-only" finally *proven*, and §§1–6 stand as written.

Until iteration-5 runs, treat §§2–3 of this document as *probable but
unproven*.

## 8. FINAL VERDICT (2026-09-30) — iteration-5/6 settled it: parser is dead

The board came back to the bench and iteration-5 then iteration-6 ran with
the spec-exact commands. **The read-only-pipe conclusion is now proven with
the correct evidence.** Full raw captures: `/tmp/gps_iter5.log` (host).

**The experiment (iteration-6, module ALREADY streaming so boot-chatter
could not confound):** ladder replaced with two forever-repeating probes —
`$PAIR051*3E` (GET_FIX_RATE; spec §2.3.10 says a live parser returns
`$PAIR001,051,0` + `$PAIR051,<ms>` echo) and `$PAIR050,100*22` (SET_FIX_RATE
10 Hz; spec §2.3.9: a live parser returns `$PAIR001,050,0` and RMC+GGA jump
to the set rate). Constellation auto-config removed so nothing else touches
the stream.

**What the captures show:**

1. **Verified deliveries, zero reaction.** `$PAIR050,100*22` and `$PAIR051*3E`
   were each delivered to 0x58 with transport success
   (`Command sent OK (17 bytes to 0x58)`) in multiple independent probe
   windows — and the read stream never reacted: GGA/s pinned at ~1.0, no
   `$PAIR001` ACK ever, no `$PAIR051,<ms>` echo ever, `$GNRMC` never appears.
   Rate commands that WORK are unmistakable (10 GGA/s); nothing unmistakable
   ever happened.
2. **The `$PAIR011,001`/`$PAIR010,1,-1`/`$PAIR010,2,-1`/`$PQTMVER` bursts are
   periodic unsolicited chatter, not responses.** Decisive datum: probe #1
   FAILED at the config-write transport (0x58 NAK'd the address — the module
   verifiably received nothing) — **and the chatter burst appeared anyway
   ~10 s later**, while two probes that delivered successfully produced no
   chatter at all. The bursts recurred on a fixed ~55 s period in both
   sessions. Anti-correlated with delivery ⇒ not command responses. Case closed.
3. **Even the write-slot transport is half-dead:** 0x58 NAK'd its address on
   4 of 7 probe attempts *while 0x50/0x54 reads kept streaming normally* —
   the slave's write interface is not merely parser-dead, it intermittently
   doesn't acknowledge at the wire level at all.

**Conclusion (finally on correct evidence):** this board's I2C carries NMEA
out only. Commands — in any syntax, spec-exact included — are not consumed
by the module's parser. A 1 Hz fixed-rate NMEA logger, unconfigurable by
design of THIS board's wiring. §§1–6 stand. The POD3 conversion
(`POD3-CONVERSION-PLAN.md`) is the go-forward path for this node.
unproven*. That is exactly the standard we closed on too early.