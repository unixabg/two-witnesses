#!/bin/bash
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
# live.sh - the live dashboard without a recording: a quick look at the boards
# (are they all heard, how strong, does a link react when you walk through).
#
# Usage:
#   ./live.sh [HOURS]      show the boards live (default: up to 4 hours); q stops it
#   ./live.sh stop         stop it from another terminal
# Nothing is kept but ~/tw-data/live/listen.log (and placement.csv if you press m).
# Sleep is not blocked. For anything you want to look at later, use run.sh.
#
# Settings: TW_DATA=~/tw-data PORT=/dev/ttyACM0 VENV=~/scratch-esp DASH_TAP=47001
set -u
KIT=$(cd "$(dirname "$0")" && pwd)
TW_DATA=${TW_DATA:-$HOME/tw-data}; PORT=${PORT:-/dev/ttyACM0}; VENV=${VENV:-$HOME/scratch-esp}
PY=$VENV/bin/python; [ -x "$PY" ] || PY=python3
DASH_TAP=${DASH_TAP:-47001}
. "$KIT/tw_lib.sh"

case "${1:-}" in
  stop)
    tmux has-session -t live 2>/dev/null || { echo "live.sh is not running"; exit 1; }
    tmux kill-session -t live; echo "stopped"; exit 0 ;;
  -h|--help) sed -n '4,13p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
esac
HOURS=${1:-4}

command -v tmux >/dev/null || { echo "tmux is missing (sudo apt install tmux)"; exit 1; }
tw_port_free_or_exit live
[ -e "$PORT" ] || { echo "no board at $PORT (a board on the Chromebook's native USB port)"; exit 1; }
[ -w "$PORT" ] || { echo "REFUSED: no permission for $PORT (sudo usermod -aG dialout $USER, then log in again)"; exit 1; }

DIR=$TW_DATA/live
mkdir -p "$DIR"
REC=$DIR/commands-listener.sh
{
  echo "#!/bin/bash"
  printf '%q ' "$PY" "$KIT/listen.py" "$PORT" "$DIR" --hours "$HOURS" --tap "$DASH_TAP" --no-save; echo
  printf 'tmux kill-session -t live 2>/dev/null\n'
} > "$REC"
DASHCMD=$DIR/commands-dash.sh
{
  echo "#!/bin/bash"
  printf '%q ' "$PY" "$KIT/dash.py" --tap "$DASH_TAP" --folder "$DIR"; echo
  echo "tmux kill-session -t live 2>/dev/null     # q in the dashboard ends the live look"
} > "$DASHCMD"
chmod +x "$REC" "$DASHCMD"
tmux new-session -d -s live -n listener "bash $REC"
tmux set-environment -t live TW_RUN "$DIR (live, nothing saved)"
tmux new-window -d -t live:1 -n dash "bash $DASHCMD"
printf 'live %s\n' "$DASHCMD" > "$TW_DATA/.dash-current"
sleep 3
if ! pgrep -f "^$PY $KIT/listen.py $PORT" >/dev/null; then
  echo "the listener stopped at once:"
  tail -5 "$DIR/listen.log" 2>/dev/null
  tmux kill-session -t live 2>/dev/null
  exit 1
fi
echo "live (nothing saved) for up to $HOURS h. q in the dashboard stops it; Ctrl-b then d leaves it running."
if [ -t 0 ] && [ -t 1 ]; then
  exec "$KIT/dash.sh"
fi
echo "open it: ~/tw-kit/dash.sh      stop: $0 stop"
