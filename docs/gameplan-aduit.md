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
| P0.1 | ⏳ NOT STARTED (design settled: Option A de-facto, see §3); hard-fail guard `if(NOT FONT_SOURCES) FATAL_ERROR` still to add | `common/CMakeLists.txt:112-113` globs `FONT_SOURCES`/`IMAGE_SOURCES` with no emptiness check today |
| P0.2 | ✅ superseded by evidence — see §3 Correction 1 (CI runs, red since 2026-10-06; earliest recorded run 2026-10-06T07:14Z). Badge rule: `readme.md` gets no CI badge until first green run | `gh run list` output captured above |
| P0.3 | ⏳ NOT STARTED. History blobs confirmed for `build_mosA/` + `build_odsh/` (ninja + compile_commands ≈ 53 MB + dup libs). `dash-pods/opendash.FCStd` 104,348,715 B and `gps/time` 98,667,859 B both still tracked at HEAD — matches plan; plan's step-4 PNG correction = −9 MB (not −60 MB, see §2) | `git ls-files`, blob table |
| P0.4 | ✅ NOT-STARTED confirmed: no size guard, **no `.repo-size-allowlist` file exists**, root `.gitignore` has **no** `*.FCStd` and no `*.log` pattern (only the `tearing-logs/` dir; `*.bin` globally ignored already). `scripts/` today: `apply_license_notice.py`, `check_docs.sh`, `od-flash.py`, `tearing_capture.sh` | `ls scripts/`, `.gitignore` at HEAD |
| P0.5 | ⏳ NOT STARTED — fleet-clean.sh not yet in scripts/ (planned); 12 stale build dirs ≈ 2.9 GB confirmed; tearing-logs/ = 284K (keep) | du table §2 |
| P0.6 | ✏️ **The "39 opcodes" claim is TRUE.** `grep -c '#define OPENDASH_CMD_' common/include/opendash_protocol.h` = **39 exactly**. The game-plan's "header greps 42 OPENDASH_CMD_ tokens" counted token *mentions* — the 3 extras are comment references (protocol.h lines 98, 190, 202). So TODO.md's "39 opcodes" needs NO wording fix; what lands is the self-verifying docs-lint rule (derive count from `#define` lines, diff TODO.md) and the PROJECT_INDEX "Last updated" bump (currently 2026-10-02) | both greps re-run 2026-10-07 |

## 5. PHASE 1 line-by-line verdicts (code claims; bench work still pending)

| Item | Verdict | Evidence |
|---|---|---|
| P1.1 | ✅ stale comments confirmed verbatim: `mos-4ch-a/main/main.c:375` + `:120`; `mos-4ch-b/main/main.c:374` + `:120` — "Firing (channel energize) is NOT wired yet — actuator stays inhibited" while `parachute_fire()` (mos-a `main.c:156`) genuinely energizes and is called live from `OPENDASH_CMD_PARACHUTE_DEPLOY` (mos-a `:406`) | sed of dispatch block 2026-10-07 |
| P1.2 | ✅ claim "only the config *store* is tested" is TRUE — `test/test_parachute.c` (4 tests) covers defaults/sanitize/NVS round-trip only; the fire-verdict (enabled ∧ mask≠0 ∧ armed, LATCH vs PULSE, idempotent lockout) lives untested inside the MOS mains. `common/src/opendash_parachute.c` + `opendash_parachute_actuator.c` exist as the move targets | file reads |
| P1.3 | ✏️ premise sharpened: the learn-the-center-MAC machinery exists (`mos-4ch-a/main/main.c:92-93` `s_center_mac`; learn block in `dispatch_message()` ≈`:211-263`), BUT it is **not a gate at all today**: ANY sender of a whitelisted control opcode is *latched as the center*, and every mismatch **re-latches** ("Center MAC re-synced", `:263`). RAM-only, no NVS. So P1.3 is a from-scratch policy feature (persist + gate + button re-pair), not an "extension" of a pin — an attacker frame today *becomes* the pinned center. The plan's wording ("extend it into a policy gate") understates this | dispatch_message read 2026-10-07 |
| P1.4 | ✅ confirmed: `common/src/opendash_espnow.c:177` `bcast_peer.encrypt = false;` and `:256` `peer.encrypt = false;` — both peer paths unencrypted, exactly as the plan claims | grep |
| P1.5 | ⏳ decision D4 pending owner (auto-DISARM vs survive; `node_health.c` heartbeat exists; 2026-10-06 "honest silence" commits in `git log`) | git log |
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



---

## 7. Owner decisions (D1–D7) — status snapshot

| # | Decision | Game-plan rec | Ledger status |
|---|---|---|---|
| D1 | CI assets: converters-in-CI (A) vs LFS-commit (B) | A | **A confirmed as de-facto reality** (§3) — awaiting owner ratification to make it law |
| D2 | History rewrite authorization | yes, after P0.1 | OPEN |
| D3 | LMK strategy (one fleet dev key now) | yes | OPEN |
| D4 | Auto-disarm on center silence | auto-DISARM 30 s | OPEN |
| D5 | App-signing now, secure boot later | yes | OPEN |
| D6 | BLE OTA: button-gate + risk register | yes | OPEN |
| D7 | Guards: 5 MB file / 1,500 line | as proposed | OPEN |

## 8. Change log (code/doc changes made while executing — append-only)

| Date | Commit | Changed | Plan item |
|---|---|---|---|
| 2026-10-07 | — | ledger opened; full plan audit run against HEAD `aab09fb` | — |

## 9. Next actions (from the plan's Week-1 quick-start, ledger-adjusted)

1. `fix(mos): reconcile stale fire-path comments…` — P1.1 (exact line refs in §5)
2. `ci: prove the build from a pristine clone` — P0.1 Option A **plus §3
   Correction-2 constraint** (tools must exist inside the action's build
   container; plain runner apt step insufficient) + CMake hard-fail guard
3. `chore(repo): untrack generated PNG previews; add .FCStd/*.log ignores + size
   guard` — P0.4 + P0.3-step-4 (−9 MB, per §2 correction)
4. feat(scripts): add fleet-clean.sh — P0.5
5. `test(parachute): fire-verdict truth table` — P1.2
6. Coordinated history rewrite — P0.3 (after D2)
7. P0.6: add opcode-count self-verify to `check_docs.sh`; bump PROJECT_INDEX
   "Last updated" per convention

---

*Ledger opened 2026-10-07. Every future phase-gate updates §8 and flips the
matching row's verdict from ⏳ to ✅/✏️ with its evidence link.*
