#!/bin/bash
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
# watch.sh - detection mode: the recorder (as run.sh) plus the live
# detector and the camera buffer (watch.py), together in tmux.
#
# Usage:
#   ./watch.sh UNTIL [--nocam]        e.g.  ./watch.sh 07:00
#   ./watch.sh stop                   stop early (view and sheets are still made)
# Everything goes in one folder, ~/tw-data/watch/<date>_<start time>/ :
#   csi-*.csv.gz, listen.log            the full recording, as in run.sh
#   view.png, view.txt, usual.json      the view, made at the end
#   scene.txt                           the scene you typed at the start
#   detect/events.csv                   the events: fill in label and note
#   detect/sheet-NN.png                 contact sheets, one row per event
#   detect/events/<event>/*.jpg         the kept frames
#   detect/seconds.csv, watch.log       the detector's second-by-second record
# Watch it:   ~/tw-kit/dash.sh       the live dashboard (q closes it; the run carries on)
#             tmux attach -t watch   (windows: 0 recorder, 1 detector, 2 dashboard; Ctrl-b n to switch, Ctrl-b d to leave)
#
# Settings: TW_DATA=~/tw-data PORT=/dev/ttyACM0 VENV=~/scratch-esp TAP=47000
#   CAM_INPUT="-f v4l2 ... -i /dev/video0" to use another camera
#   SCENE="..." to skip the scene question; USUAL=, QUIET= as run.sh
set -u
KIT=$(cd "$(dirname "$0")" && pwd)
TAP=${TAP:-47000}
. "$KIT/tw_lib.sh"                      # the settings (TW_DATA, PORT, VENV, PY, ...) and shared steps

if [ "${1:-}" = "stop" ]; then
  tmux has-session -t watch 2>/dev/null || { echo "nothing is being watched"; exit 1; }
  tmux send-keys -t watch:1 C-c 2>/dev/null; sleep 2; tmux send-keys -t watch:0 C-c 2>/dev/null
  echo "stopping; the view and contact sheets are being made (tmux attach -t watch)"
  exit 0
fi
UNTIL=${1:-}; [ -z "$UNTIL" ] || [ "$UNTIL" = "-h" ] && { sed -n '4,22p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
shift
NOCAM=""
for opt in "$@"; do
  case "$opt" in
    --nocam) NOCAM="--nocam" ;;
    *) echo "unknown option $opt"; exit 1 ;;
  esac
done

command -v tmux >/dev/null || { echo "tmux is missing"; exit 1; }
tw_port_free_or_exit watch
[ -e "$PORT" ] || { echo "no receiver at $PORT (the board's native USB port)"; exit 1; }
[ -w "$PORT" ] || { echo "REFUSED: no permission for $PORT (sudo usermod -aG dialout $USER, then log in again)"; exit 1; }
if [ -z "$NOCAM" ] && [ -z "${CAM_INPUT:-}" ] && [ ! -e /dev/video0 ]; then
  echo "no camera at /dev/video0 (or use --nocam)"; exit 1
fi
if [ -z "$NOCAM" ] && ! command -v ffmpeg >/dev/null; then
  echo "ffmpeg is missing (sudo apt install ffmpeg), or use --nocam"; exit 1
fi
if [ "$("$SYSTEMCTL" is-enabled sleep.target 2>/dev/null)" != "masked" ]; then
  # shellcheck disable=SC2086
  if sudo -n "$SYSTEMCTL" mask $SLEEP_UNITS 2>/dev/null; then
    echo "sleep is blocked (and will be unblocked at the end)"
  else
    echo "blocking sleep (sudo asks for your password):"
    # shellcheck disable=SC2086
    sudo "$SYSTEMCTL" mask $SLEEP_UNITS >/dev/null || { echo "could not block sleep; stopping"; exit 1; }
  fi
fi

DIR=$TW_DATA/watch/$(date +%Y-%m-%d_%H%M)
[ -e "$DIR" ] && { echo "$DIR already exists (two starts in the same minute?); wait a minute"; exit 1; }
mkdir -p "$DIR/detect"
tw_scene_and_layout "$DIR" "Scene (which cars, doors, people, what changed; where the camera points)"

DET_ARGS=(--tap "$TAP" --until "$UNTIL")
[ -n "$NOCAM" ] && DET_ARGS+=(--nocam)
if [ -n "${CAM_INPUT:-}" ]; then        # passed through a file: it may hold quotes
  printf '%s' "$CAM_INPUT" > "$DIR/detect/cam_input.txt"
  DET_ARGS+=(--cam-input-file "$DIR/detect/cam_input.txt")
fi
MV_ARGS=()
[ -n "${USUAL:-}" ] && MV_ARGS+=(--usual "$USUAL")
[ -n "${QUIET:-}" ] && MV_ARGS+=(--quiet "$QUIET")

# each window's chain is a script in the folder: no quoting puzzles inside tmux
REC=$DIR/commands-recorder.sh
{
  echo "#!/bin/bash"
  printf '%q ' "$PY" "$KIT/listen.py" "$PORT" "$DIR" --until "$UNTIL" --tap "$TAP" --tap "$DASH_TAP"; echo
  printf '%q ' "$KIT/after_run.sh" "$DIR" "${MV_ARGS[@]}"; echo
  printf 'if sudo -n %q unmask %s 2>/dev/null; then echo "sleep is allowed again"; ' "$SYSTEMCTL" "$SLEEP_UNITS"
  printf 'else echo "undo the sleep block: %q unmask"; fi\n' "$KIT/run.sh"
  # the detector stops at the same time; wait for its contact sheets before closing
  printf 'while pgrep -f %q >/dev/null; do sleep 5; done\n' "^$PY $KIT/watch.py $DIR"
  tw_chain_end watch
} > "$REC"
DET=$DIR/commands-detector.sh
{
  echo "#!/bin/bash"
  printf '%q ' "$PY" "$KIT/watch.py" "$DIR/detect" "${DET_ARGS[@]}"; echo
  printf 'echo %q\n' "detector finished: $DIR/detect (this window closes with the session)"
  echo "sleep 2000"
} > "$DET"
chmod +x "$REC" "$DET"

tmux new-session -d -s watch -n recorder "bash $REC"
tmux set-environment -t watch TW_RUN "$DIR"
sleep 1
tmux new-window -t watch:1 -n detector "bash $DET"
DASHCMD=$DIR/commands-dash.sh
{
  echo "#!/bin/bash"
  printf '%q ' "$PY" "$KIT/dash.py" --tap "$DASH_TAP" --folder "$DIR" --until "$UNTIL"; echo
  echo "echo 'dashboard closed (the run carries on); reopen it: ~/tw-kit/dash.sh'; sleep 4"
} > "$DASHCMD"
tmux new-window -d -t watch:2 -n dash "bash $DASHCMD"
printf 'watch %s\n' "$DASHCMD" > "$TW_DATA/.dash-current"
sleep 5
# anchored to the python processes (the tmux shells' command lines would match too)
if pgrep -f "^$PY $KIT/listen.py $PORT" >/dev/null && pgrep -f "^$PY $KIT/watch.py $DIR" >/dev/null; then
  echo "watching until $UNTIL"
  echo "folder: $DIR"
  echo "look:   ~/tw-kit/dash.sh   (the live dashboard; or tmux attach -t watch)"
  echo "stop:   $0 stop"
else
  echo "something did not start:"
  tmux capture-pane -p -t watch:0 2>/dev/null | grep -v '^$' | tail -5
  tmux capture-pane -p -t watch:1 2>/dev/null | grep -v '^$' | tail -5
  tmux kill-session -t watch 2>/dev/null
  # shellcheck disable=SC2086
  sudo -n "$SYSTEMCTL" unmask $SLEEP_UNITS 2>/dev/null && echo "(sleep allowed again)"
fi
