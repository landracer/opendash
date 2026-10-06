<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# Safety Deployment System — Bench & Track Verification Checklist

> **Purpose.** Repeatable verification checklist for the parachute / rollover-recovery
> deployment subsystem (Center fusion master, RIGHT/POD1/POD2 gyro detectors,
> MOS-4CH-A/B actuators). Written against the **v0.1.0 baseline**.
> The design & behavior reference is
> [`wiki/safety-deployment-system.md`](../wiki/safety-deployment-system.md) —
> this checklist *proves* that behavior on the bench and re-proves it at the track.
>
> **Run the full suite (Tests T1–T13) whenever:** firmware is flashed to any node in
> the deployment chain (`center`, `right`, `pod1`, `pod2`, `mos-4ch-a/b`), any MOS or
> detector hardware is swapped, or a material config change is made.
> Before **every race session**, run the condensed
> [pre-race quick check](#-pre-race-quick-check-condensed) instead.

---

## 🚨 Golden Rules — read these FIRST, every time

1. **NEVER test with live squibs/pyro initiators installed on the bench.** Every test
   below runs with **dummy loads** — 12 V lamps or power resistors wired to the MOS
   power channels where the squib harness will connect. A live initiator plus a
   "harmless" bench procedure is how fingers get lost. Live ordnance goes on **only
   at the track, at squib-install time, system DISARMED**, per track rules.
2. **DISARM (short tap on the center home status bar) before touching any wiring.**
   Power-cuts are also safe by design: ARM never persists — any reboot (center or
   MOS) boots **DISARMED**.
3. **Fail-safe ≠ license.** The 8-gate interlock chain exists to prevent accidental
   fires — it is not a reason to relax rule 1.
4. **A failed test stops the session.** Diagnose, fix, then re-run the suite from
   T1. A failed test means the system is NOT AVAILABLE, not "mostly works".
5. **Sign the sign-off table.** No signature = not verified. Record firmware
   versions (`OPENDASH_VERSION_*`, see [`opendash_common.h`](../common/include/opendash_common.h)).

---

## 🧰 Equipment

| Item | Notes |
|---|---|
| Center display (ESP32-S3 4.3″) | Fusion master, deploy panel UI, ARM bar |
| Detectors: RIGHT, POD1, POD2 | QMI8658 gyro nodes. Voter set is fixed: `OPENDASH_ROLLOVER_DETECTORS = {RIGHT, POD1, POD2}` (LEFT/GPS intentionally excluded) |
| Actuator: MOS-4CH-A (+ MOS-4CH-B if fitted) | Deploy actuation = **MOS power channels**, `channel_mask` bits 0–3 = CH1–CH4. Legacy single-GPIO actuator path is inhibited (`gpio_num = -1`) |
| 12 V bench supply or car battery | MOS channels switch 12 V power |
| 4× dummy loads | 12 V brake lamps (e.g. 1157) **or** 10–47 Ω / ≥25 W power resistors, one per channel under test; fuse the feed inline |
| DMM or test lamp | Verifies channel state independent of the UI |
| USB host with ESP-IDF v6.1 + [`scripts/od-flash.py`](../scripts/od-flash.py) | Flash **only** via this wrapper — dual-checks by-id serial *and* running-firmware log tag before writing. MOS boards share one FTDI (`/dev/ttyUSB1`): flash one at a time, the tag probe tells you which board is attached |
| Serial monitors on center and MOS | Log evidence: `RX 0x95 echo …`, `AUTO-DEPLOY <mos>: …` |
| Tilt jig: protractor / inclinometer app + foam cradle | Repeatable holds past 45° and deliberate fast-tip moves |
| Momentary NO push-button (T12 only, optional) | Detector manual-release: active-low to GND. Requires that detector flashed with `OPENDASH_ROLLOVER_BTN_GPIO_<node>` set (default `-1` = disabled) |
| Slow-motion camera (T7, optional) | Proves the fast-tip move really exceeded 300°/s |

---

## 📐 Cheat-Sheet — Verified Constants & Opcodes (v0.1.0)

Cross-checked against [`opendash_rollover.h`](../common/include/opendash_rollover.h),
[`opendash_parachute.h`](../common/include/opendash_parachute.h) and
[`opendash_protocol.h`](../common/include/opendash_protocol.h). If this table and the
headers ever disagree, the headers win — and this page gets fixed.

| Item | Value | Meaning |
|---|---|---|
| Voter set | `{RIGHT, POD1, POD2}` | Fixed by `OPENDASH_ROLLOVER_DETECTORS` (LEFT/GPS intentionally excluded) |
| Auto-deploy trigger | **unanimous 3/3** fresh `rolling=1` votes | `OPENDASH_ROLLOVER_DETECTOR_COUNT = 3` — one dissenting or silent detector blocks the fire |
| Manual-deploy trigger | **any 1** fresh `manual=1` vote | Physical button or on-screen operator fire; still gated by interlocks 3–8 |
| Vote frames | `0x96` **broadcast** | Silent when level (zero airtime); re-broadcast every **150 ms** while active; **expires at 600 ms** |
| Roll angle path | ≥ **45°** held **200 ms** (defaults) | Center-pushable config — no reflash. Hysteresis **5°** (clears below 40°) |
| Roll rate path | ≥ **300°/s** (default) | Fast tip-over — bypasses the 200 ms sustain |
| Manual button hold | **750 ms** | `OPENDASH_ROLLOVER_BTN_HOLD_MS` anti-bump; GPIO default `-1` = disabled |
| ARM | ≈1 s long-press on center home status bar | Short tap = instant DISARM. **Never persists** — center and every MOS boot DISARMED; disarm always wins instantly |
| Safe defaults | `enabled = 0`, `channel_mask = 0x00` | Fresh MOS boots disabled with no channels; legacy GPIO actuator path inhibited (`gpio_num = -1`) — MOS **power channels** CH1–CH4 are the actuator |

**Opcodes** ([`opendash_protocol.h`](../common/include/opendash_protocol.h)):

| Opcode | Name | Direction | Payload / effect |
|---|---|---|---|
| `0x27` | `PARACHUTE_SET_CONFIG` | Center → MOS + detectors | Config blob; MOS persists to NVS and echoes `0x95` |
| `0x28` | `PARACHUTE_SET_ARM` | Center → MOS | `[armed:1]` — live intent only, never persisted |
| `0x29` | `PARACHUTE_PULL_ALL` | Center → MOS/detector | Request fresh `0x95` STATUS echo |
| `0x2A` | `PARACHUTE_DEPLOY` | Center → MOS | Interlocked fire request |
| `0x2B` | `PARACHUTE_CALIBRATE` | Center → detectors (broadcast) | Zero roll to current resting angle (NVS-persisted offset) |
| `0x95` | `PARACHUTE_STATUS` | MOS/detector → Center | Echo → panel shows `✓ CONFIRMED` on exact match |
| `0x96` | `PARACHUTE_VOTE` | Detector → Center (broadcast) | `rolling` / `manual` / live roll angle + rate + seq |

**Per-MOS interlock chain — ALL 8 gates must pass for an auto fire (wiki §6):**
1 `trigger` true · 2 not already latched this event · 3 MOS **online** · 4 valid `0x95`
status echo · 5 MOS **ARMED** · 6 config **enabled** · 7 **AUTO_DETECT** flag set ·
8 `channel_mask != 0`. On pass, `DEPLOY 0x2A` + `PULL_ALL 0x29` are force-sent and the
per-MOS latch is set — fires **once per event**; latch resets when trigger clears.
The MOS independently re-checks enabled / channel / armed / already-fired before it
energizes anything (defence in depth — see `DEPLOY refused:` log lines).

---

## 🧪 Bench Test Suite

Mark a test `[x]` only if it passes **exactly** as written. Any deviation = **stop and
diagnose** (Golden Rule 4). Default bench config used throughout: **roll angle 45° /
sustain 200 ms / roll rate 300°/s / FIRE: LATCH / deploy channel CH1**, dummy load on
CH1. Reference log markers quoted verbatim from firmware:

- Center: `AUTO-DEPLOY mos-4ch-a: ROLLOVER QUORUM (rolling=3/3) ch=0x1` (or `MANUAL RELEASE`)
- MOS fire: `!!! PARACHUTE DEPLOY — energizing channels 0x1, reason=…, mode=LATCH !!!`
- MOS refuse: `DEPLOY refused: system DISABLED` / `no channel selected` / `DISARMED`
- MOS pulse mode: `Parachute pulse complete — channels 0x… de-energized (DEPLOYED latched)`

### T1 — Boot into safe state

**Proves:** nothing is armed, enabled, or energized on a fresh power-up.

1. Wire the CH1 dummy load to MOS-A; leave MOS-B unpowered (its test is T10).
2. `python scripts/od-flash.py center --no-monitor` (and `right`, `pod1`, `pod2`,
   `mos-4ch-a`) — wrapper dual-checks by-id serial + log tag before writing.
3. Power MOS-A first, watch its serial log banner: config fields echo at boot
   `(DISARMED)`.
4. Power center + detectors. Center home status bar must read **DISARMED**.
5. Center → Config → **DEPLOYMENT SYSTEM** → MOS-A: live row shows `ROLL VOTES 0/3`,
   `auto off`, MOS echo **not** DEPLOYED, `enabled` shows persisted value only after
   T2 pushes it.

- [ ] **PASS:** every node boots safe; no channel energizes with the load attached;
  no `AUTO-DEPLOY` line anywhere in any log.

### T2 — Config push, echo confirm, persistence

**Proves:** `0x27` config path, exact-match `✓ CONFIRMED` echo, MOS NVS persistence,
detector mirroring.

1. DEPLOYMENT SYSTEM → select **MOS-A** → **ENABLE** ON, **CH1** ON,
   FIRE: **LATCH**, MIN SPEED / ROLL ANGLE 45 / ROLL RATE 300 / SUSTAIN 200,
   **AUTO: OFF** → **PUSH CONFIG**.
2. Watch for `✓ CONFIRMED` on the live row (reconciler re-sends ~1 Hz until the MOS's
   `0x95` echo byte-matches — there is no "press again").
3. **REFRESH** → panel re-loads the MOS's persisted config.
4. Power-cycle MOS-A → **REFRESH** again.

- [ ] **PASS:** echo confirms within ~1–2 s; after the MOS reboot the panel shows the
  *same* config (NVS-persisted) — while the MOS remains **DISARMED** (config persists,
  ARM never does); detectors now use the pushed thresholds.

### T3 — Zero / calibrate (ZERO/CAL)

**Proves:** mount-angle calibration offsets are captured, NVS-persisted, and confirmed.

1. With the car/jig **level on its wheels**, press **ZERO/CAL**.
2. Watch `ROLL VOTES` → immediately `0/3` (each detector broadcasts a fresh *level* vote
   on calibrate, clearing in-progress roll state).
3. Power-cycle POD1 (in its **as-mounted** orientation) and let it re-join.

- [ ] **PASS:** `0/3` before and after; POD1 boots already level without a new cal —
  its offset survived reboot (NVS namespace `rollover`, key `roll_off`);
  zero vote traffic while level (detectors silent — zero airtime).
- ⚠️ Note for the record: ZERO/CAL zeroes *any* angle presented, real tilt included —
  calibrate only level, per wiki §8. Rate path is unaffected by calibration.

### T4 — Single / double detector vote must NOT auto-fire

**Proves:** quorum is unanimous; a majority is insufficient.

Setup: T2/T3 complete, then **AUTO: ON** + PUSH CONFIG (`✓ CONFIRMED`), then **ARM**
(long-press home status bar ≈1 s → live row shows `ARMED`, `AUTO ON`).

1. Tilt **only POD1** past 45°, hold 10 s. Panel: `ROLL VOTES 1/3`.
2. Level it; repeat with **RIGHT + POD1** tilted together: `2/3`.
3. Throughout, watch center + MOS logs and the CH1 dummy load.

- [ ] **PASS:** votes refresh while held, decay to `0/3` ≤ 600 ms after leveling;
  **no** `AUTO-DEPLOY` log line, **no** `PARACHUTE DEPLOY` on any MOS, dummy load never
  lights at 1/3 or 2/3.

### T5 — Full 3/3 quorum auto-fires (dummy load)

**Proves:** the happy path — unanimous votes energize the selected channel once.

1. Same armed setup as T4; all three detectors upright, `ROLL VOTES 0/3`.
2. Tilt **all three** detectors past 45° within the same second; hold ≥ 2 s.
3. Watch the load, the MOS log, and the center log.
4. Level the detectors → votes clear → `0/3`.

- [ ] **PASS:** center logs `AUTO-DEPLOY mos-4ch-a: ROLLOVER QUORUM (rolling=3/3) ch=0x1`;
  MOS logs the `!!! PARACHUTE DEPLOY — energizing channels 0x1 … mode=LATCH !!!` line;
  CH1 dummy lamp lights and **stays lit** (latched); panel live row shows **DEPLOYED**;
  unselected channels CH2–CH4 stay dead.

### T6 — Rate path (fast tip-over) fires without the 200 ms hold

**Proves:** the gyro-rate detector leg works and also reaches the quorum.

1. Start from a **cleared + re-armed** state (see T13 clear sequence; `0/3`, `ARMED`,
   `AUTO ON`).
2. Whipsaw all three detectors from upright to fully tilted fast enough that the roll
   **rate** exceeds 300°/s — deliberately faster than the 200 ms angle-sustain needs
   (slow them down in step T5 if you want to isolate the difference; the vote payload's
   `roll_rate` field on the center log is the visible proof).

- [ ] **PASS:** quorum fires via the rate path (same `ROLLOVER QUORUM` center log, lamp
  lights); a slow tilt through the same angles but < 300°/s and < 45° does **not** fire.

### T7 — Rough shake does NOT chatter-fire

**Proves:** hysteresis (5°) + `sustain_ms` (200 ms) + per-node enable gate suppress
road noise and handling bumps.

1. Armed, `AUTO ON`, load attached, detectors at rest (`0/3`).
2. Shake, thump, and wobble the rig hard for ≥ 60 s — sharp taps, lateral jinks,
   partial rolls deliberately kept **under 45°**.

- [ ] **PASS:** `ROLL VOTES` stays `0/3` throughout (or flickers ≤ 2/3 if you exceed one
  node's threshold — still below quorum); **zero** fire events;
  after a genuine > 45° single-detector tilt, the vote only clears once the node is back
  **below 40°** (5° hysteresis — verify on the panel counter).

### T8 — Detector power-pull mid-event → vote expires → no auto fire

**Proves:** the 600 ms vote-expiry fail-silent path (wiki §16: a detector that dies
mid-run must **not** complete the quorum).

1. Armed, `AUTO ON`, `0/3` idle. Tilt **RIGHT + POD1** past 45° and keep them held —
  panel shows `2/3`.
2. Tilt POD2 past 45° and **immediately yank its power** (single clean motion).
3. Hold for ≥ 10 s.

- [ ] **PASS:** center tally may flash `3/3` for ≤ 600 ms but **must settle at `2/3`**;
  no `AUTO-DEPLOY` fire occurs (confirm the MOS never logged a DEPLOY — the expiry must
  beat the 100 ms fusion tick *before* it ever sees a unanimous set). Then re-power POD2:
  it re-joins, re-reads its NVS offset, votes level → stays `2/3`, still no fire.
- ⚠️ If it *does* fire, you tilted POD2 past threshold and powered it off too slowly —
  re-run; a real fire on this rig is your one free mistake (Golden Rule 1: dummy loads
  only, so it costs you nothing but a re-arm).

### T9 — ARM non-persistence (both ends)

**Proves:** ARM is live intent only — never stored, dead on any reboot.

1. Armed state active (live row `ARMED`). **Power-cycle MOS-A** → monitor its boot log
   (`(DISARMED)` banner) and the panel echo.
2. Re-ARM via home-bar long-press → `ARMED` again. Now **power-cycle CENTER**.

- [ ] **PASS:** after each reboot the system is **DISARMED** — MOS never re-arms itself,
  the center never re-asserts a stale ARM (it boots with safe-default intent and the
  operator must deliberately long-press again); while disarmed, **every** fire path
  refuses (`DEPLOY refused: DISARMED` if you trigger T5's tilt at this point — do one
  test tilt to see it).

### T10 — Offline MOS interlock blocks its channel

**Proves:** interlock gate 3 — an offline actuator cannot be *ordered* to fire, and its
absence cannot block the other one from working (already proven); this test pins the
center side: no force-send completes to an offline MOS.

1. With MOS-B **unpowered/offline**, repeat the T5 three-detector tilt.
2. Watch which node fires (MOS-A) and what the center logs.

- [ ] **PASS:** `AUTO-DEPLOY mos-4ch-a:` only — no fire to MOS-B, no crash/retry-storm
  on the center (its reconciler just keeps no-op retrying until B re-joins; MOS-B boots
  DISARMED whenever it is powered anyway); center live row shows MOS-B offline.

### T11 — Physical manual-release button fires with AUTO OFF

**Proves:** `manual=1` vote is a *single-vote* fire path, independent of `AUTO_DETECT`.

Pre-req: one detector (POD1) reflashed with `OPENDASH_ROLLOVER_BTN_GPIO_POD1` set to its
test GPIO (default `-1` = disabled; button is active-**low**, so wire the button
GPIO→GND). Center config stays **AUTO: OFF** — only enable + CH1 + LATCH.

1. ARM the system (long-press). Verify `auto off` on the live row.
2. **Brief tap** of the button (< 750 ms).
3. **Hold** the button ≥ 750 ms.

- [ ] **PASS:** the tap produces **no** vote (750 ms anti-bump); the hold raises a fresh
  `manual=1` vote → center logs `AUTO-DEPLOY mos-4ch-a: MANUAL RELEASE` → CH1 lamp
  lights even though `AUTO_DETECT` is OFF; releasing the button clears the manual vote.

### T12 — Center on-screen deploy + ARM-bar semantics

**Proves:** the operator override path (always available) and its pre-checks.

1. System disarmed but enabled + CH1 set: press-and-hold **HOLD TO DEPLOY** → must
   **refuse** (MOS not armed → T9's `DEPLOY refused: DISARMED` still on the MOS).
2. ARM (long-press home bar). Hold **HOLD TO DEPLOY** again.
3. Short-**tap** the home status bar.
4. Repeat step 2 while MOS-A is unpowered.

- [ ] **PASS:** step 1 no fire; step 2 fires (lamp on, `MANUAL RELEASE` log); step 3
  short tap = **instant DISARM** (live row flips; any later quorum tilt is refused);
  step 4 is refused — the center's pre-send check sees MOS offline (online gate) and
  nothing is force-fired at a node that can't echo.

### T13 — Latch → disarm → clear → re-arm cycle

**Proves:** deploy stays latched until disarm; clearing the latch requires DISARM first;
one event = exactly one fire.

1. Continuing from T11/T12: channel latched ON, lamp lit, echo DEPLOYED.
2. While **still armed**, re-tilt all three detectors: no second fire, no error spam —
  latch is already set (`s_auto_latched[]` / MOS idempotent lockout).
3. **DISARM** (short tap) → observe: lamp goes out (the disarm path de-energizes the
  fired channel and clears the per-MOS latch — `parachute_disarm_reset()`), echo leaves
  DEPLOYED.
4. **Re-ARM** + repeat the T5 quorum tilt → fires again, cleanly.
5. Optional (PULSE): set FIRE: **PULSE**, pulse 500 ms, PUSH CONFIG; fire once — lamp is
  live ~500 ms (`Parachute pulse complete …` log) then dark while still DEPLOYED.

- [ ] **PASS:** exactly one fire per armed event; latch survives trigger-clear until
  DISARM; after disarm a test tilt logs `DEPLOY refused: DISARMED`; re-arm restores.

---

## ✅ Pre-Race Quick-Check (condensed)

The full 13-test suite is for the bench. On race morning, run just this:

1. Car on its wheels, **level**, flat ground.
2. Power up → center home bar reads **DISARMED**; DEPLOYMENT SYSTEM → MOS-A (B if
   used): live row shows the expected config (`enabled`, CH, thresholds, `auto on/off`).
3. **REFRESH** → values match what you intend to run.
4. **ZERO/CAL** → `ROLL VOTES 0/3`.
5. (Autonomous) **AUTO: ON** → **PUSH CONFIG** → wait `✓ CONFIRMED`.
6. Squibs installed last, per track rule; harness continuity per manufacturer procedure.
7. **ARM** (long-press home bar) just before the run — live row: `ARMED`.
8. After the run (or any reboot): **DISARM** / re-ARM consciously. Never trust a
   carried-over arm state — nothing persists anyway.

---

## ✍️ Sign-Off

| Test | Subject | Result (P/F) | Date | By |
|---|---|---|---|---|
| T1 | Boot safe state | | | |
| T2 | Config push / echo / persistence | | | |
| T3 | Zero-cal + offset persistence | | | |
| T4 | 1/3 + 2/3 must not fire | | | |
| T5 | 3/3 quorum fires (dummy) | | | |
| T6 | Rate path fires | | | |
| T7 | Shake does not chatter-fire | | | |
| T8 | Power-pull → expiry → no fire | | | |
| T9 | ARM non-persistence | | | |
| T10 | MOS-offline interlock | | | |
| T11 | Manual button single-vote fire | | | |
| T12 | On-screen deploy + ARM-bar tap | | | |
| T13 | Latch / disarm / clear cycle | | | |
| — | Pre-race quick-check walked | | | |

Firmware baseline: `OPENDASH_VERSION_*` = ______________  •  Rig: ______________

**This system is not cleared for live squibs until every row above is P-signed.**

---

## 🔗 Related

- Design reference: [`wiki/safety-deployment-system.md`](../wiki/safety-deployment-system.md)
- Opcodes / interlocks / failure modes: wiki §12 / §6 / §16
- Safe-flash wrapper: [`scripts/od-flash.py`](../scripts/od-flash.py)




