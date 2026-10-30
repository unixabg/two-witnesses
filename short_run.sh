#!/bin/bash
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
# short_run.sh - one command for a short test: clock check, optional
# seal, camera and recorder started together, stopped on time, a short
# health report, files kept together in one folder on the USB.
#
# Usage:
#   ./short_run.sh NAME SECONDS [--nocam] [--delay S]
#   e.g. ./short_run.sh cars2-laps-03 600
#
# Writes $TW_DATA/short/NAME/ :
#   NAME.csv           the recording (as record_csi.py)
#   NAME-<time>.mkv    the camera, clock burned in (unless --nocam)
#   NAME.sealed.txt / NAME.sha256   if you seal an expectation
#   run.log            everything printed, plus the clock check
#   MANIFEST.sha256    fingerprints of the files
#
# Settings (environment, with defaults):
#   TW_DATA=~/tw-data   PORT=/dev/ttyACM0   CAM=/dev/video0
#   VENV=~/scratch-esp  COPY_TO=   (if set, the folder is also copied there
#                                   with rsync, e.g. user@workstation:tw/)
set -u
NAME=${1:-}; SECONDS_RUN=${2:-}
[ -z "$NAME" ] || [ -z "$SECONDS_RUN" ] && { sed -n '4,24p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
shift 2
NOCAM=0; DELAY=10
while [ $# -gt 0 ]; do
  case "$1" in
    --nocam) NOCAM=1 ;;
    --delay) DELAY=$2; shift ;;
    *) echo "unknown option $1"; exit 1 ;;
  esac; shift
done
KIT=$(cd "$(dirname "$0")" && pwd)
TW_DATA=${TW_DATA:-$HOME/tw-data}; PORT=${PORT:-/dev/ttyACM0}; CAM=${CAM:-/dev/video0}
VENV=${VENV:-$HOME/scratch-esp}; COPY_TO=${COPY_TO:-}
PY=$VENV/bin/python; [ -x "$PY" ] || PY=python3
CAM_INPUT=${CAM_INPUT:-"-f v4l2 -input_format mjpeg -framerate 15 -video_size 640x480 -i $CAM"}

OUT=$TW_DATA/short/$NAME
if [ -e "$OUT" ]; then echo "REFUSED: $OUT already exists. Give each run its own name."; exit 1; fi
mkdir -p "$OUT" && cd "$OUT" || exit 1
exec > >(tee -a run.log) 2>&1
say() { echo "[$(date +%H:%M:%S)] $*"; }
problem=0

say "short run $NAME, $SECONDS_RUN s, folder $OUT"
# 1. where the files go
"$PY" -c "import sys; sys.path.insert(0,'$KIT'); import listen as L; r,w=L.ram_check('$OUT'); print(w); sys.exit(1 if r else 0)" \
  || { say "PROBLEM: the folder is in RAM; it would vanish at shutdown"; problem=1; }
df -h --output=avail "$OUT" | tail -1 | sed 's/^/free space: /'

# 2. clock
TZNOW=$(date +%Z); say "timezone $TZNOW, local time $(date '+%Y-%m-%d %H:%M:%S')"
if command -v chronyc >/dev/null; then
  chronyc tracking | grep -E "Reference ID|System time|Leap status" | sed 's/^/  /'
  if ! chronyc tracking | grep -q "Leap status *: Normal"; then
    say "PROBLEM: chrony is not synchronised (try: sudo chronyc makestep, wait a minute)"; problem=1
  else
    OFF=$(chronyc tracking | awk '/System time/ {print $4}')
    awk -v o="$OFF" 'BEGIN{exit !(o > 0.1)}' && { say "PROBLEM: clock off by $OFF s (sudo chronyc makestep)"; problem=1; }
  fi
else
  say "PROBLEM: chronyc not found; the clock is unchecked"; problem=1
fi

# 3. receiver and camera
[ -e "$PORT" ] || { say "PROBLEM: no receiver at $PORT (plug it into the board's native USB port)"; problem=1; }
if [ $NOCAM = 0 ] && [ -z "${CAM_INPUT_TEST:-}" ]; then
  [ -e "$CAM" ] || { say "PROBLEM: no camera at $CAM (or use --nocam)"; problem=1; }
fi
SUDO=""
if [ -e "$PORT" ] && [ ! -w "$PORT" ]; then
  say "note: $PORT needs sudo (to fix for good: sudo usermod -aG dialout $USER, then log in again)"
  SUDO="sudo"
fi

if [ $problem = 1 ]; then
  read -r -p "Problems above. Carry on anyway? [y/N] " ans
  [ "$ans" = "y" ] || { say "stopped before recording"; exit 1; }
fi

# 4. seal (optional)
read -r -p "Seal an expectation now? Type it, or press Enter to skip: " expect
if [ -n "$expect" ]; then
  "$PY" "$KIT/seal.py" seal "$NAME" "$expect" | sed 's/^/  /'
  say "keep $NAME.sealed.txt closed; send it only after the reading is posted"
fi

# 5. go
say "starting in $DELAY s"; sleep "$DELAY"
VID=""
if [ $NOCAM = 0 ]; then
  VID="$NAME-$(date +%Y%m%d-%H%M%S).mkv"
  # shellcheck disable=SC2086
  ffmpeg -nostdin -hide_banner -loglevel error $CAM_INPUT -t $((SECONDS_RUN + 15)) \
    -vf "drawtext=text='%{localtime}':x=10:y=10:fontsize=24:fontcolor=white:box=1:boxcolor=black" \
    -c:v libx264 -preset ultrafast -crf 28 "$VID" > camera.log 2>&1 &
  CAMPID=$!
  sleep 2
  kill -0 $CAMPID 2>/dev/null || { say "PROBLEM: camera stopped at once:"; cat camera.log; }
  say "camera on -> $VID"
fi
trap 'say "Ctrl-C: stopping"; [ -n "${CAMPID:-}" ] && kill -INT $CAMPID 2>/dev/null' INT
say "recording"
$SUDO "$PY" "$KIT/record_csi.py" "$PORT" "$NAME" "$SECONDS_RUN" | tee record.out
[ -n "$SUDO" ] && sudo chown "$(id -u):$(id -g)" "$NAME.csv"
say "recorder done"
if [ -n "${CAMPID:-}" ]; then
  for _ in $(seq 20); do kill -0 $CAMPID 2>/dev/null || break; sleep 1; done
  kill -INT $CAMPID 2>/dev/null && wait $CAMPID 2>/dev/null
  say "camera done"
fi
trap - INT

# 6. health and fingerprints
say "health:"
tail -1 record.out | sed 's/^/  /'
"$PY" - "$NAME.csv" <<'EOF'
import sys
n = bad = 0
ids, rec, host = [], [], []
for line in open(sys.argv[1], errors="replace"):
    if not line.startswith("#"):
        f = line.split('"')[0].split(",")
        try:
            i, r, h = int(f[2]), int(f[19]), int(f[0])
        except (IndexError, ValueError):
            bad += 1
            continue
        ids.append(i); rec.append(r); host.append(h); n += 1
if n < 2:
    sys.exit("  PROBLEM: almost nothing recorded")
dur = (host[-1] - host[0]) / 1e9
steps = [(b - a) % 2**32 for a, b in zip(rec, rec[1:])]
board = sum(x for x in steps if x < 2_000_000) / 1e6     # a damaged timestamp jumps wildly: skip it
span = ids[-1] - ids[0] + 1
print(f"  {n} packets over {dur:.0f} s = {n / dur:.1f}/s; lost {100 * (span - n) / span:.1f}%; garbled {bad}")
print(f"  clocks: computer {dur:.1f} s, receiver {board:.1f} s")
if n / dur < 60:
    print("  PROBLEM: under 60/s (camera too heavy? cable? check run.log)")
if abs(board - dur) > 0.02 * dur + 1:
    print("  PROBLEM: the two clocks disagree (receiver reset, or lines arrived in a backlog)")
EOF
[ -n "$VID" ] && [ -s "$VID" ] && ls -la "$VID" | awk '{print "  video " $5/1e6 " MB"}'
sha256sum -- * 2>/dev/null | grep -v MANIFEST > MANIFEST.sha256
if [ -n "$COPY_TO" ]; then
  rsync -a "$OUT" "$COPY_TO" && say "copied to $COPY_TO" || say "PROBLEM: copy to $COPY_TO failed"
fi
say "done. Files in $OUT"
say "next: send $NAME.csv for the blind reading (not the seal)"
