#!/usr/bin/env bash
# tearing_capture.sh — long-run serial capture for /center tearing investigation.
#
# Usage:
#   scripts/tearing_capture.sh                       # default port /dev/ttyACM4
#   scripts/tearing_capture.sh /dev/ttyACM4          # explicit port
#   scripts/tearing_capture.sh /dev/ttyACM4 myrun    # custom log-tag
#
# When you see a tear, in ANOTHER terminal run:
#   echo "$(date -Iseconds) TEAR — describe what you saw" >> tearing-logs/operator-notes.log
# so we can correlate visual events with the serial log.
#
# Output:
#   tearing-logs/<tag>-YYYYmmdd-HHMMSS.log    raw timestamped serial
#   tearing-logs/<tag>-YYYYmmdd-HHMMSS.log.1+ rotated chunks (50 MB each)
#   tearing-logs/operator-notes.log           your manual tear notes
#
# Stop with Ctrl-C. The script restores the tty on exit.
set -euo pipefail

PORT="${1:-/dev/ttyACM4}"
TAG="${2:-center}"
BAUD=115200

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOG_DIR="${ROOT}/tearing-logs"
mkdir -p "${LOG_DIR}"

STAMP="$(date +%Y%m%d-%H%M%S)"
LOG_BASE="${LOG_DIR}/${TAG}-${STAMP}.log"

if [[ ! -e "${PORT}" ]]; then
    echo "ERROR: serial port ${PORT} not found." >&2
    exit 1
fi

# Make sure nobody else (idf.py monitor) is holding the port.
if command -v fuser >/dev/null 2>&1; then
    if fuser "${PORT}" >/dev/null 2>&1; then
        echo "ERROR: ${PORT} is in use. Close idf.py monitor / other readers first." >&2
        exit 1
    fi
fi

echo "=== tearing_capture ==="
echo "Port      : ${PORT}"
echo "Baud      : ${BAUD}"
echo "Log base  : ${LOG_BASE}"
echo "Rotate at : 50 MB per chunk"
echo "Stop      : Ctrl-C"
echo
echo "Operator notes file: ${LOG_DIR}/operator-notes.log"
echo "  In another terminal, mark tear events with:"
echo "  echo \"\$(date -Iseconds) TEAR — description\" >> ${LOG_DIR}/operator-notes.log"
echo

# Configure tty: raw, no echo, 8N1, requested baud.
ORIG_STTY="$(stty -g -F "${PORT}")"
trap 'stty -F "${PORT}" "${ORIG_STTY}" 2>/dev/null || true; echo; echo "Capture stopped."' EXIT
stty -F "${PORT}" "${BAUD}" raw -echo -echoe -echok -echoctl -echoke \
                  cs8 -parenb -cstopb -ixon -ixoff -crtscts min 1 time 0

# Tag the start.
{
    echo "----- capture start $(date -Iseconds) port=${PORT} baud=${BAUD} -----"
} >> "${LOG_BASE}"

# Heartbeat in a background subshell so we have a visible time anchor every 60 s
# even when the device is quiet.
(
    while sleep 60; do
        echo "----- heartbeat $(date -Iseconds) -----"
    done
) >> "${LOG_BASE}.heartbeat" &
HB_PID=$!
trap 'stty -F "${PORT}" "${ORIG_STTY}" 2>/dev/null || true; kill '"${HB_PID}"' 2>/dev/null || true; echo; echo "Capture stopped."' EXIT

# Read serial -> timestamp each line -> rotate at 50 MB.
# awk emits ms-precision timestamps without needing GNU coreutils ts(1).
exec < "${PORT}"
awk -v base="${LOG_BASE}" -v maxsz=$((50 * 1024 * 1024)) '
BEGIN {
    idx = 0
    cur = base
    sz  = 0
}
{
    # Millisecond timestamp via systime()+gensub of strftime — POSIX awk lacks
    # ms precision, so call date(1) only once per second and append a counter.
    now = systime()
    if (now != lastsec) {
        cmd = "date -Iseconds"
        cmd | getline tstr
        close(cmd)
        lastsec = now
        seq = 0
    } else {
        seq++
    }
    line = sprintf("%s.%03d %s", tstr, seq % 1000, $0)
    print line >> cur
    sz += length(line) + 1
    if (sz >= maxsz) {
        close(cur)
        idx++
        cur = base "." idx
        sz = 0
        print "----- rotate " strftime("%Y-%m-%dT%H:%M:%S", systime()) " -----" >> cur
    }
}
'
