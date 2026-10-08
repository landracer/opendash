<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OpenDash — Game-Plan Audit Ledger (`gameplan-aduit.md`)

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
| P1.3 | ✏️ premise sharpened: the learn-the-center-MAC machinery exists (`mos-4ch-a/main/main.c:92-93` `s_center_mac`; learn block in `dispatch_message()` ≈`:211-263`), BUT it is **not a gate at all today**: ANY sender of a whitelisted control opcode is *latched as the center*, and every mismatch **re-latches** ("Center MAC re-synced", `:263`). RAM-only, no NVS. **D4 consequence (owner ruling, §7): the trust set is NOT center-only** — a DEPLOY legitimately originated by the GPS/IMU-bearing node must survive center absence, so the future policy gate pins a ROSTER (center + designated IMU/trigger nodes), per-class, NVS-persisted, button re-pair. Today an attacker frame *becomes* the pinned center — this is the from-scratch feature. **→ DESIGN DELIVERED 2026-10-08: [`docs/POLICY_GATE.md`](POLICY_GATE.md)** — gate table (§3 of doc), MOS-local vote fusion mirroring center logic (TTL 600 ms / tick 100 ms / unanimous-among-roster-voters, manual==hard-override), GPIO0-boot-button pairing ritual, `ctrl_rejected` counters, config-version-2 struct delta. **Open questions for owner inside the doc: confirm pairing-button GPIO, unanimity rule, GPIO0 default, config v2 rollout** — implementation waits on ratification | dispatch_message read 2026-10-07; design 2026-10-08 |
| P1.4 | ✅ confirmed: `common/src/opendash_espnow.c:177` `bcast_peer.encrypt = false;` and `:256` `peer.encrypt = false;` — both peer paths unencrypted, exactly as the plan claims. D3 ratified (see §7): single fleet PMK/LMK, compile-time dev default, NVS override — deliberately NOT fancy | grep |
| P1.5 | ✏️ **OWNER OVERRULED the plan's recommendation** — see §7 D4: arm is a persistent intent state; no auto-DISARM on center silence; disarm only when its own criteria are met. The plan's "(b) auto-DISARM 30 s" is DEAD; the doc paragraph must now explain why silence ≠ disarm, and P1.3's roster design carries the deploy-without-center capability | owner decision 2026-10-07 |
| P1.6 | ⏳ deferred until P1.3/P1.4 land | — |

## 6. PHASES 2–6 line-by-line verdicts

| Item | Verdict | Evidence |
|---|---|---|
| P2.1–P2.4 | ⏳ NOT STARTED (correctly gated behind P1.3/P1.4) | — |
| P3.1 | ⏳ NOT STARTED — `test/` today: `test_data_model.c`, `test_node_health.c`, `test_parachute.c`, `test_protocol.c` (482 lines total) + `stubs/` + `unity/`; no fuzz test exists; harness + `unit-tests` CI job ready for one | `ls test/` |
| P3.2 | ⏳ NOT STARTED — no `-coverage`/gcovr anywhere (build.yml has no coverage step) | build.yml read |
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
| D3 | LMK strategy | one fleet dev key now | **Yes, deliberately simple.** The RF specialist bailed; owner wants "blanketed basic encryption, not fancy": ONE fleet-wide PMK/LMK, compile-time dev default, NVS override, no key-rotation ceremony beyond the documented re-flash. Do not gold-plate. |
| D4 | Auto-disarm on center silence | auto-DISARM 30 s | **OVERRULED — arm is persistent intent.** Owner: if the system was armed, center silence (cockpit event, radio loss) is exactly when a deploy might still be needed; a deploy-capable trigger must therefore survive center absence (GPS/IMU from another module keeps the path alive). Disarm happens only when its own criteria are met — never merely for silence. Consequence for P1.3: trust = ROSTER (center + designated IMU/trigger nodes), not center-only; today NO node evaluates deploy criteria peer-to-peer when center is offline — that gap is now an explicit P1.x design item. |
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
| 2026-10-08 | (this commit) | **P1.3 design doc delivered**: `docs/POLICY_GATE.md` (roster trust model per D4, per-opcode gate table, MOS-local vote fusion, pairing ritual, ctrl_rejected telemetry, config-v2 delta) + indexed into readme tree & PROJECT_INDEX. **Awaiting owner ratification — no firmware changed.** Implementation (pure gate/fusion functions + host tables + MOS wiring) starts only after sign-off | P1.3 design |

## 9. Next actions (ledger-adjusted, post-Phase-0)

1. **P1.3: owner ratification of `docs/POLICY_GATE.md`** (open questions listed in
   the doc: pairing GPIO, unanimity rule, config-v2 rollout) → then implement:
   pure `gate_decide()`/`fusion_eval()` + full host tables + MOS wiring + bench
   proof with the second ESP32
2. P1.4 LMK rollout runbook (wiki/ota-bluetooth style) → two-board dry run →
   full-fleet one-sitting re-flash; keeps the D3 simplicity bar (this re-flash
   also lands the widened pod partition tables — see §8); same re-flash carries
   the D5-signed images once P2.2 wires CONFIG_APP_SIGNING
3. Then Phase 3 (fuzz + coverage gate) and Phase 4 (ui_manager split,
   gauge-pair dedupe, 47-marker grandfather burn-down) per plan order

Closed review items: owner line-by-line review of `ffc2dd9` (P1.2) — SIGNED OFF
2026-10-08; committed sdkconfig review — same sign-off covers it.

---

*Ledger opened 2026-10-07. Every future phase-gate updates §8 and flips the
matching row's verdict from ⏳ to ✅/✏️ with its evidence link.*
