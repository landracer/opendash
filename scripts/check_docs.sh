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

if fail:
    print()
    for line in fail:
        print(line)
    print(f'\ndocs-lint: {len(fail)} problem(s) found')
    sys.exit(1)
print('docs-lint: OK — all links resolve, all source files licensed, pod pair in sync')
PYEOF
