#!/bin/sh
# Licensed under Sovereign Individual License v1.0 — see LICENSE file
# ─────────────────────────────────────────────────────────────────────────
# P3.2 coverage gate — parse gcov output from a coverage-instrumented host
# test build and enforce a PER-FILE line-coverage floor on the pure-logic
# modules. Floors start at the measured 2026-10-08 baseline and may only be
# ratcheted UP (never down) — this is the A-grade mechanism: coverage is a
# gate, not a hope.
#
# Usage: scripts/coverage_gate.sh <build-dir>   (default test/build-cov)
# Requires: gcov matching the compiler that built the tree.
# ─────────────────────────────────────────────────────────────────────────
set -eu

BUILD="${1:-test/build-cov}"
[ -d "$BUILD/CMakeFiles/od_core.dir" ] || {
    echo "coverage_gate: no $BUILD/CMakeFiles/od_core.dir — build with -DOD_COVERAGE=ON" >&2
    exit 1
}

# Per-file floors, measured 2026-10-08 (ratchet history in comments):
#   node_health.c         70  (actual 74.38 @2026-10-08)
#   opendash_data_model.c 75  (actual 80.95 @2026-10-08)
#   opendash_parachute.c  90  (actual 97.40 @2026-10-08)
#   opendash_protocol.c   80  (actual 86.57 @2026-10-08)
#   opendash_roster.c     90  (actual 95.96 @2026-10-08)
#   opendash_uart.c       15  (actual 19.33 @2026-10-08; UART task/HC-05 glue
#                             is NOT host-testable — only the pure payload
#                             decoder is — floor tracks decoder coverage only)
floor_for() {
    case "$1" in
        node_health.c)         echo 70 ;;
        opendash_data_model.c) echo 75 ;;
        opendash_parachute.c)  echo 90 ;;
        opendash_protocol.c)   echo 80 ;;
        opendash_roster.c)     echo 90 ;;
        opendash_uart.c)       echo 15 ;;
        *)                     echo -1 ;;  # not gated
    esac
}

fail=0
printf '%-26s %8s %6s  %s\n' "FILE" "COVERED" "FLOOR" "VERDICT"
for g in $(find "$BUILD/CMakeFiles/od_core.dir" -name '*.gcda' | sort); do
    src=$(basename "$g" .gcda)
    pct=$(gcov -n "$g" 2>/dev/null | awk '/Lines executed/{gsub(/.*Lines executed:/,"");gsub(/%.*/,"");print $1; exit}')
    floor=$(floor_for "$src")
    [ "$floor" = "-1" ] && continue   # stubs and helpers are not gated
    verdict=$(awk -v p="$pct" -v f="$floor" 'BEGIN{print (p+0 >= f+0) ? "PASS" : "FAIL"}')
    [ "$verdict" = "FAIL" ] && fail=1
    printf '%-26s %7s%% %5s%%  %s\n' "$src" "$pct" "$floor" "$verdict"
done

[ "$fail" = "0" ] || echo "coverage_gate: BELOW FLOOR (see rows above)" >&2
exit "$fail"