# OpenDash Roster Policy Gate — design (A+ P1.3)

> **Status: DESIGN — awaiting owner ratification. No firmware behavior described
> here is implemented yet.** This doc converts ruling D4 ("silence never
> disarms"; trust = ROSTER, not center-only) into a buildable spec. When ratified,
> §4–§6 become code + host truth tables, exactly like P1.2 did for the fire gate.

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

## 2. Trust model (D4 made concrete)

* Every safety node stores a **roster** in NVS: `entry = {node_id, mac[6], role_bits}`.
  * `role_bits`: `ROLE_CENTER` (exactly one), `ROLE_VOTER` (0..N IMU/trigger
    nodes whose `PARACHUTE_VOTE` this node obeys). A peer may hold both.
  * Roster + arm state **persist across reboot**. Reboot comes up in the last
    persisted arm state — *that is D4: silence never disarms*. (The old
    "reboot always DISARMED" convention is overruled per §7 of the audit ledger.)
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

## 6. Pairing ritual (the only roster mutation path)

1. Hold BOOT (GPIO0 — **owner to confirm/replace pin**) through power-on ⇒
   node enters PAIRING for 30 s, status LED fast-blink.
2. During the window: first valid `PARACHUTE_SET_CONFIG`/`SET_ARM`/`SET_RELAY`
   frame latches sender as `ROLE_CENTER`; each `PARACHUTE_VOTE` frame from a new
   peer during the window adds it as `ROLE_VOTER`. Both persist to NVS on accept.
3. Window expiry re-freezes the roster. Outside the window NOTHING mutates it —
   including frames from the already-pinned center (roster edit requires a fresh
   pairing ritual; belt-and-braces per D4's spirit).

## 7. Wire/struct delta (packed, versioned)

* new payload struct `opendash_roster_t { version; count; entry entries[4]; }` —
  same sanitize discipline as config (`count` clamped, unknown role bits masked);
* `opendash_parachute_status_t` grows its `reserved` byte into
  `{last_reason, ctrl_rejected_lo, ctrl_rejected_hi}` in **config version 2**;
  `OPENDASH_PARACHUTE_CONFIG_VERSION` 1→2 is the rollout signal to center UI.

## 8. Test matrix (lands with the code, same harness as P1.2)

pure `opendash_gate_decide(opcode, sender_role, pairing_active) → allow|deny+reason`
and pure `opendash_fusion_eval(votes[], voter_count, now_us) → fire|hold`:
every row of the §3 table both ways, roster-empty dead state, TTL expiry,
manual override, duplicate/stale seq, pairing-window-only mutation, and the
full P1.2 fire-verdict table still passing unchanged on top of the gate.