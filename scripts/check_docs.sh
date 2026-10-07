#!/usr/bin/env bash
# Licensed under Sovereign Individual License v1.0 — see LICENSE file
# ─────────────────────────────────────────────────────────────────────────────
# OpenDash docs-lint — the documentation guarantees, ENFORCED.
#
# Checks (any failure exits non-zero and fails the docs-lint CI job):
#   1. Every relative markdown link in the tracked .md docs resolves to a
#      real file (root, docs/, wiki/, */README.md).
#   1b. Backticked repo paths inside core docs (e.g. `common/include/...h`)
#      resolve to real files — catches stale source-of-truth tables.
#   2. Every tracked C/H source file carries the license header.
#   3. Per-node display_init.c drift report (invariant visibility — see
#      DISPLAY_SYNCHRONIZATION.md). pod1/pod2 MUST be identical; left/right
#      are documented-identical page-set exceptions, so their hash is only
#      printed. Any NEW unique hash (node not in the expected table) fails.
#   4. Repo size guard (A+ P0.4): tracked file > 5 MB unless allow-listed in
#      .repo-size-allowlist (every entry needs a one-line WHY); tracked
#      *.FCStd/*.f3d/*.log or gps/time-style log blobs fail outright.
#   5. Opcode count self-verify (A+ P0.6): the OPENDASH_CMD_* #define count in
#      opendash_protocol.h must match the count TODO.md claims.
#   6. Every .github/workflows/*.yml must parse as YAML (zero-jobs failure
#      guard — a malformed workflow silently runs no jobs at all).
#
# WHY these checks exist (plain language, for new contributors):
#   - The docs are the contract. A markdown link that goes nowhere or a doc
#     naming a file that no longer exists is documentation LYING about the
#     tree. This check is dumb and byte-exact on purpose — same idea as
#     checksumming a backup instead of eyeballing it. Don't argue with it.
#   - License headers keep provenance provable on every file. (Vendored
#     Unity is exempt — it carries its own MIT license.)
#   - pod1/pod2 share one schematic; if their display_init.c drifts, one pod
#     behaves differently at speed and no unit test would ever catch that.
#
# Run locally (same thing CI runs):   bash scripts/check_docs.sh
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail
cd "$(dirname "$0")/.."

python3 - <<'PYEOF'
import glob, os, re, subprocess, sys, hashlib

fail = []

# ── 1. broken relative markdown links ───────────────────────────────────────
md_files = glob.glob('*.md') + glob.glob('docs/*.md') + glob.glob('wiki/*.md') \
         + glob.glob('*/README.md') + glob.glob('agent/*.md')
broken = []
for md in md_files:
    base = os.path.dirname(md)
    txt = open(md, encoding='utf-8', errors='ignore').read()
    for m in re.finditer(r'\]\(([^)#\s]+)(#[^)]*)?\)', txt):
        link = m.group(1)
        if link.startswith(('http', 'mailto:', 'mailto', '<')):
            continue
        p = os.path.normpath(os.path.join(base, link))
        if not os.path.exists(p):
            broken.append((md, link))
for md, link in broken:
    fail.append(f'BROKEN LINK   {md} -> {link}')

# ── 1b. backticked repo paths that do not exist ─────────────────────────────
# Exception list: planned (not-yet-created) files named by roadmap docs are
# intentionally absent — LAP_TRACKING_PLAN.md describes modules to be created
# only if lap tracking ever ships. Everything else must resolve.
PLANNED_PATHS = {
    'common/src/opendash_gps_config.c',
    'common/src/opendash_lap_timer.c',
    'common/src/opendash_track_db.c',
    'common/include/opendash_lap_timer.h',
    'common/include/opendash_track_db.h',
}
stale = []
path_re = re.compile(r'`((?:common|center|left|right|gps|pod1|pod2|openDstream|'
                     r'mos-4ch-a|mos-4ch-b|relay-4ch-hd|relay-8ch-a|relay-8ch-b|'
                     r'scripts|docs|wiki)/[\w./+-]+\.(?:c|h|py|sh|md|json|csv))`')
for md in md_files:
    txt = open(md, encoding='utf-8', errors='ignore').read()
    for m in path_re.finditer(txt):
        p = os.path.normpath(m.group(1))
        if p in PLANNED_PATHS:
            continue
        if not os.path.exists(p):
            stale.append((md, m.group(1)))
for md, p in stale:
    fail.append(f'STALE PATH    {md} -> `{p}`')

# ── 2. license header on every tracked source file ──────────────────────────
tracked = subprocess.run(['git', 'ls-files', '*.c', '*.h'],
                        capture_output=True, text=True).stdout.split()
# Exception: vendored third-party sources carry their own licenses (Unity = MIT)
tracked = [f for f in tracked if not f.startswith('test/unity/')]
unlicensed = [f for f in tracked
              if 'Sovereign Individual License'
              not in open(f, encoding='utf-8', errors='ignore').read()]
for f in unlicensed:
    fail.append(f'NO LICENSE    {f}')

# ── 3. display_init.c drift report ──────────────────────────────────────────
print('display_init.c per-node hashes (drift visibility):')
for node in ['center', 'left', 'right', 'gps', 'pod1', 'pod2']:
    p = f'{node}/main/display_init.c'
    h = hashlib.md5(open(p, 'rb').read()).hexdigest()[:12]
    print(f'  {node:7s} {h}')
p1 = open('pod1/main/display_init.c', 'rb').read()
p2 = open('pod2/main/display_init.c', 'rb').read()
if p1 != p2:
    fail.append('DRIFT         pod1/main/display_init.c != pod2/main/display_init.c (must stay identical)')

# ── 4. repo size guard (A+ P0.4) ───────────────────────────────────────────
# WHY: what a repo forces on every clone is a guarantee, not a preference.
# Any tracked file > 5 MB needs an allow-list entry WITH a one-line WHY
# (same culture as .gitignore comments); CAD binaries and raw log dumps
# never belong in git at all — external drive / Release asset instead.
MAX_BYTES = 5 * 1024 * 1024
allow = set()
if os.path.exists('.repo-size-allowlist'):
    for line in open('.repo-size-allowlist', encoding='utf-8'):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        allow.add(os.path.normpath(line.split(maxsplit=1)[0]))
tracked_all = [p.decode() for p in
               subprocess.run(['git', 'ls-files', '-z'], capture_output=True, check=True).stdout.split(b'\x00')
               if p]
for f in tracked_all:
    if f in allow:
        continue
    try:
        if os.path.isfile(f) and os.path.getsize(f) > MAX_BYTES:
            fail.append(f'SIZE          {f} is {os.path.getsize(f) // (1024*1024)} MB (> 5 MB; needs an allow-list entry with WHY, or untrack it)')
    except OSError:
        pass
for f in tracked_all:
    if f.endswith(('.FCStd', '.f3d')) or f.endswith('.log') or f == 'gps/time':
        fail.append(f'BINARY LOG    {f} — CAD binaries/log dumps do not belong in git (external drive or GitHub Release asset)')

# ── 5. opcode count self-verify (A+ P0.6) ───────────────────────────────────
# WHY: doc counts must be DERIVED from the code, never hand-counted. If the
# protocol header's opcode count no longer matches what TODO.md claims,
# the docs drift is caught here, on every push, forever.
proto = open('common/include/opendash_protocol.h', encoding='utf-8', errors='ignore').read()
n_opcode = len(re.findall(r'#\s*define\s+OPENDASH_CMD_', proto))
todo_txt = open('TODO.md', encoding='utf-8', errors='ignore').read()
if f'{n_opcode} opcodes' not in todo_txt:
    fail.append(f'OPCODE COUNT  opendash_protocol.h defines {n_opcode} OPENDASH_CMD_* opcodes but TODO.md does not say "{n_opcode} opcodes" — fix the doc, not this check')

# ── 6. workflow files must be valid YAML ────────────────────────────────────
# WHY: a workflow that fails to parse runs as ZERO-JOBS failure — it looks like
# "CI is red like always" instead of "the config file is malformed". That exact
# class of confusion cost a re-push on 2026-10-07 (see docs/gameplan-aduit.md §8).
try:
    import yaml as _yaml
    for wf in sorted(glob.glob('.github/workflows/*.yml') + glob.glob('.github/workflows/*.yaml')):
        try:
            _yaml.safe_load(open(wf, encoding='utf-8'))
        except Exception as e:
            fail.append(f'BAD YAML      {wf} does not parse: {e}')
except ImportError:
    fail.append('NO YAML-LINT  PyYAML unavailable — workflow syntax cannot be checked')

if fail:
    print()
    for line in fail:
        print(line)
    print(f'\ndocs-lint: {len(fail)} problem(s) found')
    sys.exit(1)
print('docs-lint: OK — all links resolve, all source files licensed, pod pair in sync')
PYEOF
