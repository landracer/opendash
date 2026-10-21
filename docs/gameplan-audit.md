<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OpenDash — Game-Plan Audit Ledger (`gameplan-audit.md`)

> **Purpose:** live, line-by-line audit of [`A_PLUS_GAMEPLAN.md`](./A_PLUS_GAMEPLAN.md)
> against the actual tree. Every claim in the game-plan is checked, graded, and
> (where wrong) corrected HERE. This file is the anti-drift ledger: when a plan
> line says "today X", the ledger records whether X is actually true, with
> evidence. Code/doc changes made while executing the plan get logged in §8.
> **Audit run:** 2026-10-07 against HEAD `aab09fb` (356 commits, private remote
> `git@github.com:landracer/opendash.git`).
> **Rule of engagement (inherited):** honesty over polish — a wrong claim in the
> game-plan gets marked ✏️ with the corrected number, not silently re-worded.

**Status legend**
| Mark | Meaning |
|---|---|
| ✅ VERIFIED | Game-plan claim matches reality (evidence recorded) |
| ✏️ CORRECTED | Claim directionally right, number/framing wrong — corrected below |
| ❌ REFUTED | Claim is wrong |
| ⏳ NOT STARTED | Work item understood, not yet executed |

---

## 1. §0 rubric table — verdict

✅ VERIFIED. The grading story is consistent with the tree: enforced docs-lint
(`scripts/check_docs.sh` runs in the `docs-lint` CI job, `build.yml:87-93`) and
a host test harness (`test/` + Unity + ctest in the `unit-tests` job,
`build.yml:100-115`) are the only "fails a build" mechanisms today — exactly
what the plan says.

## 2. §0.1 hygiene numbers — claim vs measured (2026-10-07)

| Game-plan claim | Measured | Verdict |
|---|---|---|
| working tree 5.0 GB | **5.3 GB** | ✅ (rounding) |
| 12 stale `*/build/` dirs ~2.8 GB | 12 dirs, **~2.9 GB** (left 273M, pod1 269M, pod2 268M, right 266M, gps 265M, center 262M, mos/relay ×4 ~239M each, openDstream 214M, test/build 1.3M) | ✅ |
| `.venv` + `.venv-ota` 142 MB | 124M + 18M = **142M** | ✅ exact |
| `.git` 252 MB | **252M** | ✅ exact |
| tracked content at HEAD 212 MB | **213 MB** (222,943,127 bytes) | ✅ (rounding) |
| `dash-pods/opendash.FCStd` 104 MB | **104,348,715 bytes** | ✅ exact |
| `gps/time` 98 MB | **98,667,859 bytes** (still tracked — `git ls-files gps/time`) | ✅ exact |
| `common/images/generated/*.png` (26 files) ~60 MB | **✏️ 9.1 MB across 26 tracked PNGs.** The ~60 M figure came from `du` on the directory, which includes ~50 MB of UNtracked generated `*.c` outputs (correctly ignored). Untracking the 26 PNG previews saves ~9 MB at HEAD, not ~60 MB. The 2 source PNGs (`splash_gps.png`, `background_gps.png`, 2.4 M each) stay tracked per plan. | ✏️ |
| history blobs from deleted build dirs ~60 MB | `build_mosA/build.ninja` 14.9M + `compile_commands.json` 13.6M + `build_odsh/build.ninja` 13.0M + `compile_commands.json` 11.7M + duplicated `libwpa_supplicant.a` ×2 5.2M … ≈ **~60 MB** | ✅ |
| `.gitignore` itself is good | ✅ deliberate, commented (e.g. the `dependencies.lock` DO-NOT-IGNORE note) | ✅ |

## 3. §0.2 THE CRITICAL FINDING — verdict: ✅ core claim true, ✏️ two corrections

**Core finding CONFIRMED by direct evidence.** All three legs verified:

1. `common/CMakeLists.txt:27-36` ("Step 0") *claims* generated sources are
   COMMITTED; `.github/workflows/build.yml:18-22` claims the same;
   `OD_PREGENERATED_ASSETS: "1"` set at `build.yml:59`.
2. `common/fonts/generated/.gitignore` = `*.c` "do not commit" →
   **0 font `.c` tracked** (only `.gitignore` + `opendash_font_config.h`).
   `common/images/generated/.gitignore` ignores `*.c` `*.h` `*.bin`.
3. Live proof on the wire: **every** `build-smoke` run on GitHub Actions is a
   `failure`. Latest run **37567083609** (2026-10-07T03:30Z) fails on exactly
   the predicted mechanism: `center/main/main.c:43:10: fatal error:
   splash_center.h: No such file or directory`.

### ✏️ Correction 1 — "has Actions ever run?" (P0.2): YES, it runs — and it's red
`gh run list` shows `build-smoke` firing on **every push since 2026-10-06**,
every run `failure` (plus 2 `cancelled`). So "CI green" was never true on this
tree — but the framing "may have never run" is replaced by the stronger,
proven statement: **CI is real and permanently red**, and per the plan's own
rule every downstream "CI green" claim in TODO/CHANGELOG carries `[?]` until
the first green run after P0.1 lands.

### ✏️ Correction 2 — the skip-gate doesn't even work in CI
Run-37567083609 logs show the converters **actually executing in CI** (the
lines "[ERROR] Node.js is not installed" from `convert_fonts.py:62` and
"[ERROR] ImageMagick is not installed" from `convert_images.py:89` appear in
the `build center` / `build gps` job logs), even though the job env prints
`OD_PREGENERATED_ASSETS: 1`. Whatever the propagation detail (the
esp-idf-ci-action builds inside its own container — note the container path
`/app/landracer/opendash/...` in the logs), the empirical truth is:
**today CI behaves as if `OD_PREGENERATED_ASSETS` were unset** — converters
run, find no Node/ImageMagick, fail soft, and the build hard-fails at link.

Practical consequence for P0.1: the game-plan's **Option A** (CI runs the
converters) is not merely recommended — it is what the tree is *already
de facto doing* (converters execute on every CI configure) and what the
nested `do not commit` .gitignores encode as intent. The ledger adds one
constraint the plan didn't state: because `esp-idf-ci-action@v1` runs the
build **inside its own container** (log path `/app/landracer/opendash/...`),
the P0.1 step-1 toolchain prep (`imagemagick`, Pillow, `npm ci` in
`common/fonts`) must be *inside that same build environment* — a plain
runner-side `apt-get` step is NOT sufficient. Proven need: `convert_fonts.py`
shells out to npm `lv_font_conv` (`convert_fonts.py:66-89`,
`common/fonts/package.json` exists); `convert_images.py` needs ImageMagick
(`convert_images.py:89`) + Pillow. The `OD_PREGENERATED_ASSETS` gate itself
then gets deleted or hard-guarded per the plan's "hard-fail guard" item.

---

## 4. PHASE 0 line-by-line verdicts

| Item | Verdict | Evidence / note |
|---|---|---|
| P0.1 | ✅ DONE 2026-10-07 (`4a7a7c4`): workflow now installs the converter toolchain on the runner and generates sources before the IDF container build; `OD_PREGENERATED_ASSETS` deleted from CMake + docs; `if(NOT FONT_SOURCES/IMAGE_SOURCES) FATAL_ERROR` guard live | commits 4a7a7c4 |
| P0.2 | ✅ CLOSED 2026-10-07: first fully-green run is [build-smoke #30](https://github.com/landracer/opendash/actions/runs/37705648769) (12-node matrix + docs-lint + unit-tests all success, same day the pipeline went real). readme.md badge added per rule | run #30 |
| P0.3 | ✅ HEAD-side DONE (`a5f889b` + `f73a74c`): FCStd + gps/time + 26 PNGs untracked — tracked content 213 MB → **10.0 MB**. HISTORY REWRITE still ⏳ OPEN pending D2 (clone still pays full 252 MB .git until it happens) | `git ls-files` |
| P0.4 | ✅ DONE (`a5f889b`): check_docs.sh check 4 = >5 MB guard + `.repo-size-allowlist` (empty by design, WHY-per-entry culture); `*.FCStd`/`*.f3d`/`*.log`/`gps/time` ignores added | check_docs.sh diff |
| P0.5 | ✅ DONE (`a5f889b`): scripts/fleet-clean.sh landed (dry-run default, --force acts, venv report-only, tearing-logs kept) — executed with --force, tree 5.3 GB → ~2.4 GB | script + du |
| P0.6 | ✏️ **The "39 opcodes" claim was TRUE** (`#define OPENDASH_CMD_` = 39 exactly; the plan's "42" counted comment references). ✅ DONE: check_docs.sh check 5 now DERIVES the count from the header and diffs TODO.md — self-verifying forever after; PROJECT_INDEX bumped | both greps |

## 5. PHASE 1 line-by-line verdicts (code claims; bench work still pending)

| P1.1 | ✅ FIXED 2026-10-07 (`4db839a`): both MOS mains now describe the real behavior — "Firing IS wired: parachute_fire() energizes the selected channels ONLY when enabled ∧ channel_mask≠0 ∧ armed (idempotent — latches once, never re-fires); LATCH holds until disarm/reboot, PULSE auto-off after pulse_ms. Disarm safe-resets fired channels." Remaining `inhibited` mentions (mos-a :120/:684, mos-b :120/:679) verified ACCURATE — they describe the optional shared-GPIO actuator path (parachute_gpio.h -1), not the channel fire path | diff reviewed line-by-line by owner |
| P1.2 | ✅ DONE 2026-10-07: `opendash_parachute_fire_verdict()` — pure function in `common/src/opendash_parachute.c`, FULL truth table (disabled / no-channel incl. garbage-high-bits / not-armed / allow / already-fired lockout precedence / NULL fail-safe) now runs in `test/test_parachute.c` on every CI unit-tests run. Both MOS mains call it and carry zero gate logic | local ctest 4/4 suites pass; CI enforces |
| P1.3 | ✏️ premise sharpened: the learn-the-center-MAC machinery exists (`mos-4ch-a/main/main.c:92-93` `s_center_mac`; learn block in `dispatch_message()` ≈`:211-263`), BUT it is **not a gate at all today**: ANY sender of a whitelisted control opcode is *latched as the center*, and every mismatch **re-latches** ("Center MAC re-synced", `:263`). RAM-only, no NVS. **D4 consequence (owner ruling, §7): the trust set is NOT center-only** — a DEPLOY legitimately originated by the GPS/IMU-bearing node must survive center absence, so the future policy gate pins a ROSTER (center + designated IMU/trigger nodes), per-class, NVS-persisted, button re-pair. Today an attacker frame *becomes* the pinned center — this is the from-scratch feature. **→ RATIFIED 2026-10-08 (owner, §7 D4 + §8): [`docs/POLICY_GATE.md`](POLICY_GATE.md)** — gate table, MOS-local vote fusion (TTL 600 ms / tick 100 ms / unanimous-among-roster-voters [mirrors center; owner not contradicted], manual==hard-override), **center-screen pairing (no hardware buttons)**, PERSIST_ARM opt-in flag, ctrl_rejected counters, config-version-2 delta, center control-panel scope (switch panel → software channels + START ENGINE sequencer). Implementation authorized: pure gate/fusion functions + host tables, then MOS wiring, then bench proof. **→ WIRED (this commit): both MOS mains gate every inbound frame through `opendash_gate_decide()` (old latch block deleted); roster NVS store live; MOS-local fusion wired (AUTO_DETECT-gated, seq-dedupe, node-id-bound); `PERSIST_ARM` opt-in honored (restore at boot, mirror on SET_ARM); status echo v2 carries `last_reason`+`ctrl_rejected`; center auto-pushes sealed roster on every enrollment; host tables extended (bootstrap-ineligible rows + fake-NVS store round-trip). Remaining: bench proof on the second ESP32 + center UI panel/sequencer (Phase 4 surface)** | owner rulings 2026-10-08 |
| P1.4 | ✅ confirmed: `common/src/opendash_espnow.c:177` `bcast_peer.encrypt = false;` and `:256` `peer.encrypt = false;` — both peer paths unencrypted, exactly as the plan claims. D3 ratified (see §7): single fleet PMK/LMK, compile-time dev default, NVS override — deliberately NOT fancy. **⚠ DESIGN LIMIT found 2026-10-08:** `ESP_NOW_MAX_ENCRYPT_PEER_NUM = 6` — ESP-NOW supports max **6 encrypted peers per device** (total peers 20). CENTER has ~9 radio peers ⇒ "encrypt every peer at center" as scoped is physically impossible. **→ CLOSED 2026-10-08 (owner): encryption DEFERRED indefinitely, accepted as registered risk (see §7 D3 rewrite) — the gate/roster/dedupe layer is the permanent trust model; transport stays plaintext by decision, not by omission** | esp_now.h (IDF master) + owner ruling |
| P1.5 | ✏️ **OWNER OVERRULED the plan's recommendation** — see §7 D4: arm is a persistent intent state; no auto-DISARM on center silence; disarm only when its own criteria are met. The plan's "(b) auto-DISARM 30 s" is DEAD; the doc paragraph must now explain why silence ≠ disarm, and P1.3's roster design carries the deploy-without-center capability | owner decision 2026-10-07 |
| P1.6 | ⏳ deferred until P1.3/P1.4 land | — |

## 6. PHASES 2–6 line-by-line verdicts

| Item | Verdict | Evidence |
|---|---|---|
| P2.1–P2.4 | ⏳ NOT STARTED (correctly gated behind P1.3/P1.4) | — |
| P3.1 | ✅ DONE 2026-10-08 (this commit): `test/test_fuzz_parsers.c` — deterministic xorshift64* corpus (seed `0x9E3779B97F4A7C15` committed, 10k iterations per suite, whole suite <1 s). Both byte sinks fuzzed: msg codec (noise + boundary lengths + bit-flips; invariant: every accepted frame is canonical — re-serialization byte-identical, validate() agrees, checksum-byte flips never verify) and the MD parser (pure `opendash_md_parse_payload()` extracted from the UART task so the decoder is host-coverable; TAG/length gates hard, determinism checked, golden-value frame). Real corruption of a valid frame is ALWAYS rejected — collision-accepts still forced canonical | local ctest 6/6 green |
| P3.2 | ✅ DONE 2026-10-08 (this commit): `OD_COVERAGE` CMake option (hoisted before all targets so `--coverage` actually reaches the objects under test — caught and fixed during bring-up), `scripts/coverage_gate.sh` parses gcov per-file line coverage and fails below floor; CI `unit-tests` job now builds `-DOD_COVERAGE=ON` and runs the gate. Measured 2026-10-08 floors (may only ratchet UP): protocol 80 (actual 86.57), data_model 75 (80.95), parachute 90 (97.40), roster 90 (95.96), node_health 70 (74.38), uart 15 (19.33 — UART task/HC-05 glue is inherently unhost-testable; floor tracks the pure decoder only) | gate output green locally |
| P3.3/P3.4 | ⏳ NOT STARTED (in priority order as plan states) | — |
| P4.1 | ✅ `center/main/ui_manager.c` = **4,752 lines** (plan: 4,752 ✓). Docs-lint line-count guard absent; check_docs.sh today enforces only links/license/pod-sync invariants | wc -l |
| P4.2 | ✅ drift confirmed: `left/main/display_init.c` = **1,243** lines vs `right/main/display_init.c` = **993** (≈354-line diff — left carries the GT911 touch block, right doesn't). `pod1`/`pod2` `display_init.c` = **550 lines each, byte-identical** (`diff -q` clean) — the pod-pair invariant already holds. 2.8C shared-driver extraction not started. NOTE: display-config work here follows the standing display verification ritual (build → flash → 60 s monitor → no artifacts) | wc/diff |
| P4.3 | ✅ `center/main/espnow_master.c` = **1,291 lines** (plan: 1,291 ✓) | wc -l |
| P4.4 | ✏️ "the 18 markers in `common/` + center" is WRONG: `TODO:`/`FIXME:` marker count across `common/` + `center/` sources = **47** (`common/` alone: 8). Burn-down baseline is 47, not 18. Raw `TODO`-string hits (incl. prose comments) = 93 | grep 2026-10-07 |
| P4.5 | ⏳ correctly sequenced after P1.3; `boostcontrol-staging/` exists at root (staging dir observed) | ls |
| P5.x | ⏳ deferred by design (they document phases 1–3 truths) | — |
| P6.1–P6.4 | ⏳ deferred; P6.2's single-source check will target `common/include/opendash_common.h` `OPENDASH_VERSION_*` (exists, per readme §Versioning) | readme |

### 6.1 Comment archaeology (original list, live status)

Preserved from the pre-audit "comment archaeology" list so nothing was silently
dropped when this file was rebuilt; status as of 2026-10-07:

1. ~~mos-4ch-a/README.md:12~~ **resolved** (P1.1 rewrite; see §5 P1.1)
2. `gnss/main/gnss.c:261-263` — says CAN frames are "not forwarded **yet**";
   forwarding IS wired (`forward_can_frame()` in `relay/main/main.c`) →
   rewrite to state the **routing rule** + future CAN capability (P3.4)
3. `mos-4ch-a/README.md:117` (and mos-b twin) — "reserved for future use (BLE
   OTA opcode)" → **rewrite to say what the reserved field is *for*** (wire
   evolution room) (P3.4)
4. ~~mos main's "wired in a later change / nothing fires yet" block~~
   **resolved** (P1.1; and the gate logic itself is tested common code since P1.2)
5. `relay/main/main.c:206` `// TODO: CAN TX` — CAN is **real hardware and a
   real future feature**. `boost/boost.c` is a **placeholder for integration
   with an existing commercial product** — the J1939↔ESP-NOW bridge
   (OPENDASH_CMD_BOOST / RATES) stays specified as future work, honestly
   labeled (P3.4/P3.5)

**Comment canon (still program law):** a comment may state facts ("X is not
forwarded yet — this node only forwards opcode N") but future tense = spec.
Specs live in docs, referenced by the comment; inline "whatever you want"
language is not acceptable on shared code.

---

## 7. Owner decisions (D1–D7) — RATIFIED 2026-10-07

| # | Decision | Game-plan rec | Owner ruling (governs now) |
|---|---|---|---|
| D1 | CI assets: converters-in-CI (A) vs LFS-commit (B) | A | **A — ratified** ("seems that answers itself; what is best for this project"). Landed in `4a7a7c4`: converters are a real CI stage with real toolchain. |
| D2 | History rewrite authorization | yes, once, after P0.1 | **ON HOLD (owner 2026-10-07).** Not urgent: HEAD-side untracking already cut tracked content 213 MB → 10 MB; a rewrite would only shrink the 252 MB `.git` itself. Owner may reopen later. |
| D3 | LMK strategy | one fleet dev key now | **SUPERSEDED 2026-10-08 — ESP-NOW encryption DEFERRED indefinitely (owner ruling).** Original ratification (one fleet PMK/LMK, compile-time default, NVS override) became moot when research proved `ESP_NOW_MAX_ENCRYPT_PEER_NUM = 6` makes "encrypt every peer at center" impossible (center has ~9 radio peers). Owner call on the alternative: "who is going to MAC-hack these at speed?… back-burner until more devices are supported or a better way evolves." **The policy gate + roster + seq-dedupe (P1.3) is therefore the PERMANENT trust layer, not a placeholder.** Owner's floated "timestamp + random time-offset as encryption" idea recorded honestly: a time-offset scheme is obfuscation, not authentication — firmware is extractable from any single captured device (no secure-boot fuse, D5) so the offset is never actually secret; freshness was already covered by wrap-safe seq dedupe. If RF authentication ever becomes mandatory, the honest path is per-device LMK + a real pairing ritual, or secure-boot-protected firmware — both registered non-goals today, consciously. | owner decision 2026-10-08 |
| D4 | Auto-disarm on center silence | auto-DISARM 30 s | **OVERRULED — arm is persistent intent *while powered*.** Owner: if the system was armed, center silence (cockpit event, radio loss) is exactly when a deploy might still be needed; a deploy-capable trigger must therefore survive center absence (GPS/IMU from another module keeps the path alive). Disarm happens only when its own criteria are met — never merely for silence. **2026-10-08 clarification (owner):** a *power loss* is legitimately a reboot and DOES come up disarmed (standard = RAM-only arm; safety nodes ride separate redundant lithium power — hardware-layer mitigation, out of firmware scope); cross-reboot persistence is a **user-toggleable opt-in** ("persistent latch" config flag), never built-in. Roster stays center + designated gyro/IMU 3D-spatial voters. |
| D5 | OTA provenance: signing vs secure boot | app-signing now | **RATIFIED 2026-10-07 (owner):** "just do the basic app signing that comes with the ESP32 package" — `CONFIG_APP_SIGNING` keyed HMAC image digest (espsecure keyfile, same one-fleet-key philosophy as D3), NO secure-boot fuse. Recorded answer to the md5 question: md5 is the wrong tool (unkeyed hash ≠ authenticity); the standard signed-digest is the floor, and secure boot stays in the accepted-risk register as a conscious non-goal. Lands with P2.2. |
| D6 | BLE OTA access | button-gate + register | **Ratified: all OTA staged through the verified first-party app** — `/home/sysadmin/Documents/multidisplay-app/multidisplay-unified` (Qt6 desktop + Android; its `OpenDashBridge` is the one OpenDash client in existence). Physical button-gate stays as the on-vehicle gate. Third-party/foreign BLE clients are out of model by definition. |
| D7 | Guards: 5 MB file / 1,500 line | as proposed | **Ratified with clarification:** these are *repo* guards, not firmware-size limits — the app being unfinished doesn't weaken them (that is exactly why they run pre-final); the 1,500-line rule GRANDFATHERS existing files behind a shrink-only allow-list, so nothing is blocked by today's sizes. 5 MB blocks the next 100 MB blob, which is the whole point. |

## 8. Change log (code/doc changes made while executing — append-only)

| Date | Commit | Changed | Plan item |
|---|---|---|---|
| 2026-10-07 | `d7ecc0d` | ledger opened (this file + game-plan indexed into readme/PROJECT_INDEX); full plan audit run against HEAD `aab09fb` | all lines verified, see §1–§6 |
| 2026-10-07 | `4db839a` | MOS-A/B stale fire-path comments reconciled to describe the real interlock (owner-review diff) | P1.1 |
| 2026-10-07 | `4a7a7c4` | converters-in-CI workflow (runner toolchain + generate step + raw docker build), CMake hard-fail guards, OD_PREGENERATED_ASSETS deleted everywhere, CHANGELOG/TODO wording corrected | P0.1/D1 |
| 2026-10-07 | `a5f889b` | untracked 26 PNGs + FCStd (HEAD side); ignores + allow-list + size guard + opcode self-verify + fleet-clean.sh landed | P0.4/P0.5/P0.6 |
| 2026-10-07 | `f73a74c` | gps/time finally untracked (extensionless — needed its own ignore line after a `git add -A` re-picked it up) | P0.3-step-4 |
| 2026-10-07 | — | fleet-clean --force executed locally; tracked content 213 MB → 10.0 MB; first-green-run link pending the post-push Actions run (then readme badge per P0.2) | P0.5, P0.2 |
| 2026-10-07 | `fe32212` | the five superseded docs actually MOVED into committed docs/history/ and every table link repointed — the tables had been linking docs/archived/ (git-ignored local-only: links that can never resolve in a fresh clone; caught by the now-alive docs-lint) | P0.6 culture working |
| 2026-10-07 | `375d477`+`6c4e9fa` | **biggest honest find**: resolved per-node `sdkconfig` files are now COMMITTED and all sdkconfig.defaults deleted — the old defaults had silently drifted from every bench flash (missing LV_FONT_FMT_TXT_LARGE; -Og debug default vs the fleet's actual PERF build with assertions disabled; pod1 overflowed its old 2.5 MB slot by 47 KB under the release-line toolchain). Also: my own first build.yml edit shipped a YAML syntax error (runs #23/#24 = zero-job failures, caught by new check 6) | P0.1 completion |
| 2026-10-07 | `f1b2c81` | every live doc reference synced from sdkconfig.defaults to committed sdkconfig (CHANGELOG/history untouched — they record history) | doc sync |
| 2026-10-07 | `df5c8fe` | pod1/pod2 app slots widened 2.5→3 MB (slack from storage; lands via the wired P1.4 re-flash), lv_font_conv pinned exactly 1.5.3, pod2 CSV header copy-paste drift corrected | build honesty |
| 2026-10-07 | — | **[build-smoke #30](https://github.com/landracer/opendash/actions/runs/37705648769) GREEN** — all 12 node builds + docs-lint + unit-tests; readme badge shipped; P0.2 closed | first green |
| 2026-10-07 | `ffc2dd9` | **P1.2**: fire interlock extracted to `opendash_parachute_fire_verdict()` (pure, common/) + full truth-table tests; both MOS mains stripped of gate logic. D2 marked ON HOLD; D5 ratified (standard CONFIG_APP_SIGNING, no fuse) — P2.2 unblocked. CI #32 green on the refactor itself; #33 green after this ledger restore | P1.2, D2, D5 |
| 2026-10-08 | — | **Owner signed off P1.2 review** ("all looks good") — safety-refactor review closed. Bench toolchain fixed: ImageMagick was missing on the bench/runner machine; now installed. Proven end-to-end on bench: both converters `--check` green (node v24 + local lv_font_conv; ImageMagick 7.1.2-32 + Pillow 12.2), `--force` regenerated 49 font + 78 image sources, `center` IDF v6.1 build → "Project build complete", `opendash_center.bin` 3,167,184 B, git tree stayed clean (generated-dir ignore guards working) | D1 verified, P1.2 review closed |
| 2026-10-08 | (this commit) | **P1.3 RATIFIED by owner, implementation authorized.** Rulings: (1) power-loss reboot = disarm is the STANDARD (RAM-only arm; redundant lithium on safety nodes is the hardware-layer answer — firmware does not fight it); cross-reboot latch is an opt-in user flag (`PERSIST_ARM`), not built-in; (2) NO hardware pairing buttons — pairing = center-screen event coordination (bootstrap one-shot latch → center ROSTER_PUSH enrollment → center-commanded handover; dead center ⇒ bench re-flash = factory reset); (3) physical switch panel ELIMINATED — fuel/water/NOS/accessory subsystems become center-labeled relay channels with on-screen switches + "START ENGINE" declarative startup-procedure sequencer on /center; (4) all voters = gyro/IMU 3D-spatial nodes (RIGHT, POD1, POD2). `docs/POLICY_GATE.md` updated to match, status → RATIFIED | P1.3 rulings, D4 clarify |
| 2026-10-08 | (this commit) | **P1.3 core landed (additive, unwired):** `common/src/opendash_roster.c` + `common/include/opendash_roster.h` — pure `opendash_gate_decide()` (opcode→class routing, bootstrap one-shot latch, MAC+node-id binding, fail-safe denies), `opendash_roster_sanitize/_find`, `opendash_seq_fresh` (wrap-safe), `opendash_fusion_eval` (600 ms TTL unanimity + manual override, voter_count==0 = honest dead iron); new opcode `OPENDASH_CMD_ROSTER_PUSH 0x2C` (space verified free); host suite `test/test_roster.c` 6/6 green locally (2 test-authoring bugs caught and fixed by the tables themselves — the harness earning its keep) | P1.3 core |
| 2026-10-08 | (this commit) | **P1.3 WIRED (behavioral).** Gate live in both MOS mains (old learn/re-latch block deleted; bootstrap-eligible whitelist = old is_center_cmd set, SYSTEM/BOOST never latch); NVS roster store in `common/` (mutex + blob, factory-fresh on miss/corrupt/version); MOS-local fusion + AUTO_DETECT gate + seq-dedupe in the VOTE case; `PERSIST_ARM` opt-in honored (boot restore + SET_ARM mirror); config version → 2; status echo v2 (`last_reason`, `ctrl_rejected` replace pad byte); center `roster_refresh()` pushes sealed roster on every `auto_register_node`; host tables extended (ineligible-opcode rows + fake-NVS store round-trip) — local ctest 5/5 green. Firmware builds verified by CI build-smoke; bench proof still pending | P1.3 wiring |
| 2026-10-08 | (this commit) | **P3.1 + P3.2 landed (Phase 3 core).** `test/test_fuzz_parsers.c`: seeded deterministic corpus (10k/suite) fuzzing BOTH attacker byte sinks — msg codec accepts only canonical frames; MD decoder extracted into pure `opendash_md_parse_payload()` (UART commit path = decode + atomic merge, behavior-preserving split), TAG/length gates hard + golden-value vectors. `OD_COVERAGE` build + `scripts/coverage_gate.sh` per-file floors (measured baseline, ratchet-up only) wired into the CI unit-tests job. **P1.4 design limit flagged for OWNER DECISION:** ESP-NOW caps encrypted peers at 6/device, center has ~9 radio peers — see §5 P1.4 row options (a)/(b)/(c); recommendation (a) encrypt actuation path only | P3.1, P3.2, P1.4 flagged |
| 2026-10-08 | — | **P1.4 CLOSED as deferred (owner):** "who is going to MAC-hack these at speed… back-burner till more devices are supported or a better way evolves." Transport stays plaintext permanently; the P1.3 gate/roster/dedupe layer IS the trust model. Owner's timestamp+random-offset idea recorded honestly (obfuscation ≠ authentication: firmware extractable per D5 no-fuse stance ⇒ offset never secret; freshness already handled by seq dedupe). No transport code changes — `encrypt = false` is now the DESIGN, documented in POLICY_GATE §6 | P1.4 closed, D3 superseded |
| 2026-10-10 | (this commit) | **Independent re-audit — all doc-drift findings closed (see §10):** stale defaults-mechanism guidance purged from canon docs; readme tree corrected (phantom gps parachute file, missing directories, enum-correct node IDs); architecture.md duplicate heading + SD-logging falsehood fixed; PROJECT_INDEX frame diagram now matches the codec; five controller-node READMEs landed; readme badge re-pointed to `build.yml`; this ledger renamed `gameplan-aduit.md` → `gameplan-audit.md` with every link re-pointed; `check_docs.sh` check 7 (retired-mechanism guard) added | §10 rows R1–R8 |

---

## 9. Next actions (ledger-adjusted, post-Phase-0)

1. **P1.3 bench proof (NEXT, hardware):** flash one MOS + center; verify (a)
   factory-fresh MOS ignores non-eligible frames and latches center once,
   (b) ROSTER_PUSH pins voters and VOTE frames fuse locally (AUTO_DETECT on),
   (c) PERSIST_ARM opt-in survives reboot / flag off ⇒ boot DISARMED,
   (d) gate counters echo and center shows them; old-firmware center must NOT
   be able to command a new MOS (config v2 wire bump is live)
2. **Center UI surface (Phase 4 scope, still owed):** pairing/commissioning
   screen surfacing roster state + ctrl_rejected, subsystem-channel panel
   (software-labeled channels replacing the physical switch panel), START
   ENGINE declarative sequencer
3. P1.4 LMK rollout runbook (wiki/ota-bluetooth style) → two-board dry run →
   full-fleet one-sitting re-flash; keeps the D3 simplicity bar (this re-flash
   also lands the widened pod partition tables — see §8); same re-flash carries
   the D5-signed images once P2.2 wires CONFIG_APP_SIGNING
4. Then Phase 3 (fuzz + coverage gate) and Phase 4 (ui_manager split,
   gauge-pair dedupe, 47-marker grandfather burn-down) per plan order

Closed review items: owner line-by-line review of `ffc2dd9` (P1.2) — SIGNED OFF
2026-10-08; committed sdkconfig review — same sign-off covers it.

---

## 10. Independent re-audit 2026-10-10 — documentation-drift sweep (ALL CLOSED same day)

Scope: full doc set (readme, PROJECT_INDEX, QUICKSTART/QUICK_REFERENCE,
architecture, espnow-protocol, DATAFLOW, wiki, agent playbooks, this ledger)
audited against the code/headers/git-history, with ~30 independent spot
verifications — claims that HELD were recorded and left alone (TODO-marker
count = 47 exactly as corrected here; lvgl 9.2.2 + IDF 6.1.0 in
`dependencies.lock`; version single-source at 0.1.0; codec doc matches
`opendash_protocol.h`; pod-pair byte-identical; docs-lint green).

| # | Finding (severity) | Evidence at finding | Resolution (2026-10-10) |
|---|---|---|---|
| R1 | Canon contributor docs instructed editing the deleted defaults mechanism (medium) | agent playbook + digest rules named `sdkconfig.defaults` after `375d477` deleted every such file | rules rewritten to "committed per-node `sdkconfig`"; also fixed in `docs/center-display-guide.md`, `TEARING.md`, `boost-controller-opendash.md`, `gps/INTENSIVE_TODO.md`, `left-right/README.md`, and the `build.yml` matrix comment |
| R2 | readme repo-tree wrong (medium) | tree listed `gps/main/parachute.c/h` — file does not exist (actuation lives in `common/` + MOS nodes); tree omitted all controller nodes, `openDstream/`, `agent/`, `scripts/`, `test/`, `wiki/` | tree rewritten honestly (incl. real `gps/main/sd_logger.c/h`) |
| R3 | readme/QUICKSTART pod rows false (medium) | "Pod 1/2: no separate README — see source header" while `pod1/README.md` and `pod2/README.md` existed | rows re-pointed to the real READMEs |
| R4 | architecture.md data-flow falsehood (medium) | "SD card logging happens on the Center unit (primary)" — `center/main/sd_logger.c` is a self-declared stub; the real logger is `gps/main/sd_logger.c` | step 8 corrected, stub labeled honestly |
| R5 | PROJECT_INDEX frame diagram contradicted the codec (low-medium) | diagram showed `[0xAA][CMD][LEN][NODE_ID][PAYLOAD…]`; the codec header defines a 3-byte header — node id rides *inside specific payloads* (`auto_register_node()` reads `payload[0]`; `identify_sender()` maps MAC→node) | diagram + prose corrected in PROJECT_INDEX and architecture.md |
| R6 | readme CI badge dead (low) | badge URL referenced `workflows/build-smoke.yml` — the file is `build.yml` (only the workflow *name* is build-smoke) | re-pointed to `build.yml`; private-repo caveat understood |
| R7 | stale hex node IDs (low — caught while fixing R2) | readme tree + DATAFLOW diagrams showed `0x10/0x11/0x30/0x31`; the enum truth is LEFT=1, RIGHT=2, POD1=5, POD2=6 | corrected in readme tree and both DATAFLOW diagrams |
| R8 | canonical ledger filename misspelled (cosmetic) | old file path (this line deliberately escapes it) — docs/gameplan-aduit.md | `git mv` → `docs/gameplan-audit.md`; title updated; every referencing link re-pointed (readme, PROJECT_INDEX, TODO §, CHANGELOG ×2, workflow comment, check_docs comment); ledger content untouched except this note |

Also honestly recorded, not doc errors: `center/main/ui_manager.c` grew
4752 → 5256 lines in the 3 days since the P4.1 snapshot (+10.6%) — decomposition
is losing to feature velocity, P4.1 urgency raised; `docs/archived/` remains a
git-ignored local-only residue (known trap, untracked by design); opcode count
drift (docs said 39, header defines 42) was the pre-existing gap that check 5
(P0.6) now guards — verified working by this pass.

**Grade after re-audit:** Documentation returns to **A** by this section's
evidence-backed closures. A+ remains gated on the P2.1 threat-model document —
per the §0 rubric an un-documented threat model is not an A+ category. Do not
declare A+ before P2.1 lands.

---

*Ledger opened 2026-10-07. Every future phase-gate updates §8 and flips the
matching row's verdict from ⏳ to ✅/✏️ with its evidence link.*

---

## 11. Independent audit + cleanup pass (2026-10-12)

Second independent audit of the whole documentation estate (all 100+ md files,
461 relative links scanned, docs-vs-code spot-verification of frame format,
channel tables, node IDs, display timing, version defines; host test suite
re-run green 6/6). Findings and closures:

| # | Finding (severity) | Evidence at finding | Resolution (2026-10-12) |
|---|---|---|---|
| C1 | I2C-as-transport prose survived the R-series purge (medium) | `PROJECT_INDEX.md`: checklist "shared across I2C"; glossary "I2C ... shared by all four nodes"; LEFT/RIGHT task table "I2C receive → odometer"; repo-tree note claimed right differs by "I2C addr 0x11 vs 0x10" (right/README proves both boards are GT911 @ 0x5D) | all four corrected (ESP-NOW wording; right/ line now says the only difference is logical node ID 2). Check 7 only guards `sdkconfig.defaults` — a banned-phrase guard for I2C-as-network prose remains open |
| C2 | Eight canon docs were unreachable orphans (medium) | `DATAFLOW.md` (named the "end-to-end journey" canon by espnow-protocol.md!), `TEARING.md`, `QUICK_REFERENCE.md`, `UART_CONNECTION.md`, `FEATURES_AND_SENSORS.md`, `BLUETOOTH_PAIRING.md`, `BLE_OTA.md` (readme-only), `PER_NODE_DISPLAY_CONFIG_SPEC.md`, `boost-controller-opendash.md` — absent from the other index; PROJECT_INDEX claims "every documentation file" | all indexed in PROJECT_INDEX + readme docs tables |
| C3 | `docs/rgb_lcd.rst` looked like a rogue file (low) | `git log --follow`: vendored verbatim in `9ba4c82` (2026-06-06 BLE-OTA/doc-gather commit); content = official ESP-IDF "RGB Interfaced LCD" API page — exactly the `esp_lcd_rgb_panel` API every RGB node's `display_init.c` drives | KEPT (it is real display-dev reference material, not stray); provenance header added; indexed under Architecture & Design. Upstream drift risk noted in header |
| C4 | `docs/archived/` still littered local trees | byte-identical copies of five `docs/history/` docs; untracked (known trap per §10 note); source of 9 broken relative links in local scans | deleted locally; canon stays `docs/history/` only |
| C5 | GPS firmware-side join weakness explains "doesn't always connect" (high, informational — node stays FROZEN) | `gps/main/main.c` sends NO boot `CHANNEL_MSG_ANNOUNCE`; center registers GPS only via NVS-MAC (Method 2) or GPS-domain-DP content inference (Method 3) — invisible until LC76G streams; TIME_SYNC sends day/month/year=0 under a false "not parsed" comment (parse_rmc parses them); WAKE dummy write targets 0x58, the endpoint the post-mortem proved intermittently NACKs | recorded as an audit finding in TODO §2.4 (not fixed — node frozen); main.c comments corrected to match reality |
| C6 | `gps/README.md` contradicted CLOSED-FINAL (medium) | capabilities table still advertised "Max update rate 10 Hz (via $PAIR050,100)" and header said "v15L2 verified working" | frozen banner added; rate row corrected to "not configurable — parser dead"; v16k-port label squared with code header |
| C7 | Agent playbook Rule 4 said "All nodes are ESP32-S3" (low) | five controller/bridge nodes are classic ESP32-WROOM-32E (see build.yml matrix targets) | Rule 4 rewritten: S3 displays vs classic-ESP32 controllers |

**Not a doc error, honestly recorded:** the LC76G manufacturer datasheet PDF is
NOT vendored anywhere in this repo (only the LVGL flyer PDFs ship inside
managed_components); §-references in the post-mortem point at the captured
spec inside `wiki/LC76G-I2C-GPS-Driver-Guide.md`. Vendoring the actual LC76G
datasheet into the repo remains an open hygiene item.

## 12. 2026-10-19: the vendor protocol spec landed — §11 hygiene item CLOSED

The owner supplied the real Quectel document; now vendored and tracked:
`docs/p1185254-Quectel_LC26G&LC76G&LC86G_GNSS_Protocol_Specification_V1.0.0_Preliminary.pdf`
(GNSS Protocol Specification V1.0.0 Preliminary, 2022-05-13). Verified against
it (see post-mortem §9): the module's command language IS the `$PAIR` set
(spec §2.3 — PAIR001 ACK with Result 0–5, 002–007 power/starts, 050/051
fix-interval 100–1000 ms, 062/063 per-sentence rates, 066 search mode, 382
sleep-lock, 513 NVRAM save); the spec defines NO I2C transport anywhere.
**Interpretation:** the LC76G is a fully configurable ≤10 Hz GNSS engine —
the frozen verdict stands *for this board* because its I2C surface is
data-out-only, but the root cause is now precisely named (board integration,
not silicon), and iteration-6's probe set (050/051 only) is complete enough
to have caught any live path (a working 050 would have moved RMC+GGA to
10 Hz). `gps/README.md`, `docs/architecture.md`, and PROJECT_INDEX updated to
the refined wording.

Mechanism note: `docs/p1185254-…pdf` had been *untracked* because `.gitignore`
carries a blanket `*.pdf` rule ("copyrighted datasheets — never commit"). The
owner supplying this document as ground truth is the owner-level ruling that
carves out the single documented exception (see `.gitignore` comment); the
blanket policy otherwise still stands.

## 13. 2026-10-19: SCHEMATIC CONFIRMED — LC76G UART is routed (RXD=GPIO17, TXD=GPIO18). Node REOPENED for bench test

Owner checked the actual board schematic: the module UART is wired to the
ESP32 on GPIO17 (RXD net) / GPIO18 (TXD net) — the same nets the POD3 plan
treats as the module socket signals. Historical folklore ("UART disabled — do
NOT enable", gps_handler.h verbatim) was never verified, and no gps firmware
generation ever initialized UART1 — `uart_bridge.c` did not even exist in
`gps/main/`. So every $PAIR probe in §8 failed to reach the parser *by
construction*: the only channel the vendor spec documents for commands
(serial PAIR, default 115200 per PAIR864) was never connected in software.

Iteration-7 debug firmware built: `gps/main/uart_bridge.c` probes UART1 over
both pin directions × {115200, 9600}, logs every line, sends `$PAIR051*3E`
on live bytes, and on the `$PAIR001` ACK sends `$PAIR050,100*22` and logs
measured GGA Hz. Bench verdict (serial monitor) decides the node's fate:
ACK = unfrozen (10 Hz + full §2.3 command set); silence in all four combos =
read-only 1 Hz stands, finally measured on the real channel.

## 14. 2026-10-19: BENCH EXECUTED — flashed ACM5, probe swept, verdict captured. CLOSED-FINAL (measured)

Owner authorized unattended bench execution. Steps executed: enumerated
`/dev/serial/by-id/`, probed every tty port with live-log reads (ACM0=center,
ACM1=pod1, ACM3=pod2, **ACM5=GPS** — the identity doc's old table was stale and
is corrected), flashed the iteration-7 build to ACM5, captured the boot log.

Captured verdict: `UART DEAD — silence on GPIO17/18 in BOTH directions at
115200 AND 9600`. Same log: the module is ALSO unresponsive on I2C on this
unit (`ESP_ERR_INVALID_RESPONSE` on every transaction, PMIC read = 0xFF,
endless power-cycle recovery, Sats 0). Honest limit recorded in post-mortem
§11: with the module electrically unreachable everywhere, the 17/18 silence
cannot distinguish "NC on this variant" from "module unseated/dead" — only
swapping/resating hardware can. What software can still do is done:
`uart_bridge.c` stays in-tree, ready to speak PAIR@115200 the day a module
that drives those nets is seated. GPS slot's configurable-GNSS future = POD3
conversion, not firmware. Node identity system worked as designed throughout
(gps-tagged logs; no wrong-device flash possible without a visible warning).

## 15. 2026-10-19 REVERSAL: owner surgery + iteration-8/9 → 10 Hz REAL. Node ACTIVE

The §14 "CLOSED-FINAL" stood for hours, not days. Owner: "I'll take this matter
into my own hands" — disassembled the unit and hand-ran wires from the LC76G's
native UART: **module TXD→GPIO18, module RXD→GPIO17** — physically doing what
the socket never did. Iteration-8 `uart_bridge.c` (fixed 17/18 direction;
command injected at both bauds cycling until answered; trigger = the module's
actual `$PAIR051,<ms>` echo format, since this module emits no `$PAIR001` ACK
sentence) then closed the loop live on ACM5:

    UART» $PAIR051*3E  →  UART« $PAIR051,1000*13  →  UART» $PAIR050,100*22
    UART GGA rate: 9.96/9.90/10.14/9.99 Hz over last 8 fixes   ← 10 Hz, measured

FINAL: the LC76G silicon always answered `$PAIR` on its native UART at 115200,
exactly per the vendored vendor spec. The I2C bridge mirrored data OUT only —
every frozen-era "parser is dead" result was measuring the bridge, not the
engine. Node ACTIVE as configurable ~10 Hz GNSS source; `gps_handler.c` (I2C)
remains the data path, `uart_bridge.c` is the command path. POD3 conversion is
now a nice-to-have, not a requirement, for the GPS slot.

## 16. 2026-10-19: iteration-9 config audit — Sats-0 exonerates software entirely

With the real command channel, iteration-9 read the module's config: min-SNR
mask = 9 dB (factory default), constellation search mode = all enabled, rate
setting persists across module power cycles. The module also emitted genuine
GNSS-style date/time (replacing the 1980 default). Conclusion: on the bench
unit, Sats-0 is NOT config, protocol, or firmware — every one of those is now
measured-good via module self-reports. Residual cause is that unit's physical
RF path (antenna/feed, plausibly disturbed during the same disassembly).
The mod is documented as MANDATORY for anyone using this module with our code
(`gps/README.md` §MANDATORY MODIFICATION): a unit without the two wires is, and
always was, a fixed-1 Hz read-only pipe. Owner's standing position: a module
whose command channel ships "NC" is garbage-by-design for telemetry — the
documentation now says so plainly while keeping the mod recipe canonical.
