# OpenDash Roster Policy Gate — design (A+ P1.3)

> **Status: RATIFIED (owner rulings 2026-10-08) — IMPLEMENTED (pure core +
> wiring landed; bench proof on the second ESP32 pending).** This doc converts
> ruling D4 ("silence never disarms" while powered; trust = ROSTER, not
> center-only) plus the owner's 2026-10-08 clarifications (power-loss reboot =
> disarm is CORRECT standard; persistence is an opt-in user choice; pairing has
> NO hardware button — it is center-screen event coordination; the physical
> switch panel is replaced by center-controlled channels) into a buildable spec.
> §4–§6 are now code + host truth tables, exactly like P1.2 did for the fire gate.

## 1. Today's verified truth (baseline this replaces)

* Source: `mos-4ch-a/main/main.c` read 2026-10-08 (mos-b identical).

1. **No gate exists.** `dispatch_message()` (mos-a `:211+`) applies
   `SET_RELAY` / `PARACHUTE_SET_CONFIG` / `SET_ARM` / `PULL_ALL` / `DEPLOY`
   from **any** ESP-NOW sender that addressed this node — the only check is the
   payload `target == my node id` byte. Any node that learns a MOS MAC controls
   that MOS.
2. **Center identity is a self-latch, not a trust boundary.** Any genuine
   center-class command *re*-latches `s_center_mac` (RAM-only, "Center MAC
   re-synced") — an attacker frame becomes the pinned center by existing.
3. **Deploy decision is center-only.** MOS ignores `PARACHUTE_VOTE` (0x96).
   Fusion runs in `center/main/espnow_master.c` (`rollover_tally()` +
   `rollover_fusion_eval()`): trigger = fresh `manual` vote **OR** unanimous
   fresh `rolling` votes across all `OPENDASH_ROLLOVER_DETECTORS`
   (RIGHT, POD1, POD2 — count 3), vote TTL 600 ms, fuse tick 100 ms; center then
   unicasts `PARACHUTE_DEPLOY`/`PULL_ALL` to each online MOS.
4. **Consequence (the D4 bug):** center offline ⇒ a rollover cannot deploy.
   Silence currently equals disarm, functionally.

## 2. Trust model (D4 made concrete, owner-ratified 2026-10-08)

* Every safety node stores a **roster** in NVS: `entry = {node_id, mac[6], role_bits}`.
  * `role_bits`: `ROLE_CENTER` (exactly one), `ROLE_VOTER` (0..N; every voter is
    a gyro/IMU node with real 3D spatial awareness — today RIGHT, POD1, POD2).
    A peer may hold both.
  * **Arm state is RAM-only by STANDARD (owner ruling):** a power loss IS a
    reboot, and a reboot comes up DISARMED — that is correct and stays. Safety
    nodes ride **separate redundant lithium power** (hardware-layer mitigation,
    explicitly out of firmware scope) so an armed system does not silently lose
    power; if it does, disarm-on-reboot is the desired safe state.
  * **Persistent latch is an OPT-IN user choice, not built-in behavior:** config
    flag `OPENDASH_PARACHUTE_FLAG_PERSIST_ARM` (new bit, §7). Clear = arm state
    dies with the reboot (standard). Set = arm state is mirrored to NVS and a
    reboot restores it. Center UI exposes this toggle per subsystem (§8).
  * Runtime silence still never disarms (D4 stands): no timeout anywhere clears
    `armed` or `deployed` while the node stays powered.
* Frames from non-roster senders are dropped at the dispatcher and counted (§5).
* The fire interlock (`opendash_parachute_fire_verdict()`, P1.2) is **unchanged
  and unchanged in precedence**: the gate decides *who may ask*; the verdict
  still decides *whether the channel may energize* (enabled ∧ mask≠0 ∧ armed,
  idempotent latch). Gate pass + verdict deny = still no fire.

## 3. Gate table (per opcode class → required sender role)

| opcode(s) | class | accepted from |
|---|---|---|
| `SET_RELAY` (0x21), `REQUEST_RELAY_STATUS` | channel control | `ROLE_CENTER` |
| `PARACHUTE_SET_CONFIG` (0x27), `SET_ARM` (0x28), `CALIBRATE` (0x2B) | intent/config | `ROLE_CENTER` |
| `PARACHUTE_PULL_ALL` (0x29), `PARACHUTE_STATUS` (0x95 echo) | state sync | center (requests), self (echoes) |
| `PARACHUTE_DEPLOY` (0x2A) | manual fire request | `ROLE_CENTER` (center's hold-to-deploy is a manual source) |
| `PARACHUTE_VOTE` (0x96) | detector vote | any `ROLE_VOTER` member |

Non-roster → drop + `ctrl_rejected++` (reason = `NOT_ON_ROSTER`). Roster-empty
node obeys nobody except during pairing (§6) — honest dead iron, never ghost-controlled.

## 4. MOS-local fusion (deploy survives center loss — the D4 fix)

Mirror of the proven center logic, instantiated per MOS against its own roster:

* cache `vote[node] = {rolling, manual, roll_deg, roll_rate, seq, rx_us}` from
  fresh `PARACHUTE_VOTE` frames, TTL `OPENDASH_ROLLOVER_VOTE_EXPIRY_MS` (600 ms),
  refresh cadence 150 ms, eval tick 100 ms — same constants, now honored node-side;
* trigger = `manual` fresh **OR** `rolling_count == voter_count` (unanimous
  among THIS MOS's roster voters; roster with 0 voters ⇒ auto-deploy never
  originates locally, center unicast remains the only path — backward compatible);
* `seq` monotonic per sender, stale/dup seqs ignored (dedupe already in struct);
* trigger passes through the P1.2 verdict unchanged (armed/enabled/mask gates,
  idempotent latch). `AUTO_DETECT` flag stays the per-MOS enable bit for
  vote-originated deploy.

## 5. `ctrl_rejected` telemetry

* per-node counters: `rejected_not_roster`, `rejected_not_armed`,
  `rejected_disabled` (last N reasons not stored — counts + `last_reason` byte);
* echoed in the status blob; center shows/counts them (UI surface is Phase 4,
  not part of this gate).

## 6. Pairing ritual (owner ruling: NO hardware button — center-screen event coordination)

Waveshare boards have no usable spare buttons and an expansion board is out of
scope. Pairing is therefore **wireless and center-initiated**, with an honest
bootstrap caveat:

1. **Factory/bootstrap state:** fresh NVS ⇒ empty roster + `bootstrap=true`. In
   bootstrap, the FIRST valid center-class command from any sender latches that
   sender `ROLE_CENTER` (this is exactly today's behavior, now explicitly a
* honest caveat (owner decision 2026-10-08): "first sender wins" IS the
  bootstrap trust model, and it stays that way — ESP-NOW encryption was
  investigated and DEFERRED indefinitely (encrypted-peer cap of 6/device vs
  ~9 center peers made fleet-wide encryption impossible; owner judged RF
  authentication out of threat model for a private race vehicle). This is
  not a placeholder awaiting crypto: the gate/roster/dedupe layer is the
  permanent trust model. A MAC-spoofed frame from a captured/extracted
  device remains the registered accepted risk — see ledger §7 D3.
2. **Enrollment of a new voter:** center's commissioning screen (its own
   "pairing mode" event) broadcasts `ROSTER_PUSH` frames; a node in bootstrap
   accepts them; `ROLE_VOTER` entries land in the MOS roster the same way. The
   center only enrolls nodes it already speaks through (the voters are exactly
   the gyro/IMU 3D-spatial nodes: RIGHT, POD1, POD2).
3. **Center replacement (handover):** the CURRENT center is the only authority
   that can command "adopt new center MAC" (a center-class opcode mutates the
   roster's `ROLE_CENTER` entry). If the center itself is dead, re-pairing =
   bench re-flash, which wipes NVS and returns the node to bootstrap. Honest,
   and consistent with the P1.4 one-sitting re-flash ritual.
4. After bootstrap clears, NOTHING mutates the roster except a center-class
   handover/push from a member already on it.

## 7. Wire/struct delta (packed, versioned)

* new payload struct `opendash_roster_t { version; count; entry entries[4]; }` —
  same sanitize discipline as config (`count` clamped, unknown role bits masked);
* new config flag `OPENDASH_PARACHUTE_FLAG_PERSIST_ARM (1u << 2)` — opt-in
  arm-state NVS persistence (§2); default CLEAR = today's behavior (reboot ⇒
  disarmed) kept as the STANDARD, per owner ruling;
* `opendash_parachute_status_t` grows its `reserved` byte into
  `{last_reason, ctrl_rejected_lo, ctrl_rejected_hi}` in **config version 2**;
  `OPENDASH_PARACHUTE_CONFIG_VERSION` 1→2 is the rollout signal to center UI.

## 8. Center control surface (what "integrate into /center" means, owner 2026-10-08)

The physical switch panel is **eliminated**. Every subsystem the driver used to
toggle by hand becomes a center-addressed relay channel with a user-labeled
software switch on the center screen:

* **Subsystem channels:** each MOS/relay channel gets a user-assigned role
  (fuel pump, water pump, NOS, accessories…) stored in center NVS as a channel
  registry `{node, channel, label, ui_group}`. Center UI renders the panel from
  the registry; toggling a switch sends gated `SET_RELAY`.
* **ARM/DISARM master control** lives on the same screen (per-subsystem arm via
  `SET_ARM`, plus the per-subsystem **persistent-latch opt-in toggle** that
  writes the `PERSIST_ARM` flag — a *user choice*, never built-in behavior).
* **"START ENGINE" / startup procedure:** a center-side sequenced start-up:
  e.g. arm subsystem ⇒ energize fuel pump channel ⇒ wait for pressure data
  point ⇒ enable ignition channel. Modeled as a declarative step list in center
  NVS (step = {channel, wait-for-datapoint/timeout, next}) so the procedure is
  data, not code. Engine-off/idle interlocks stay center-UI state; the MOS-side
  verdict still has the final word per channel.
* All of this rides the SAME gated control path (§3) — the panel is a new face
  on old, counted, gated rails.

## 9. Test matrix (lands with the code, same harness as P1.2)

pure `opendash_gate_decide(opcode, sender_role, bootstrap_latched) → allow|deny+reason`
and pure `opendash_fusion_eval(votes[], voter_count, now_us) → fire|hold`:
every row of the §3 table both ways, roster-empty dead state, TTL expiry,
manual override, duplicate/stale seq, bootstrap one-shot latch (second
would-be-center cannot re-latch), PERSIST_ARM both ways across a simulated
reboot, and the full P1.2 fire-verdict table still passing unchanged on top of
the gate.