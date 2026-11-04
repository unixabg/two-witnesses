#!/bin/bash
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
# survey.sh - placement test: move a board around and read the link live.
# Records the whole test (as run.sh does) and opens the dashboard in its
# placement view: big RSSI and link-quality readouts. At each spot, wait 30 s
# still, press m and type where the board is (e.g. "sender 6 m, chest
# height"); the 30 s averages go to placement.csv in the survey folder.
#
# Usage:
#   ./survey.sh [UNTIL]       UNTIL = clock time to stop (default: 2 hours from now)
#   ./survey.sh stop          stop early
# First it shows the board layout and asks if it is still right (Enter = yes,
# e = edit it, b = an earlier version; tw_layout.py): every replay of the survey
# draws it. Then the scene, as run.sh asks it.
# Folder: ~/tw-data/survey/<date>_<start time>/  (csi-*.csv.gz, listen.log,
# placement.csv, and the layout and scene it started with: boards.csv, scene.txt)
# The board on the Chromebook's cable stays put; move the others (on power
# banks). With the old two-board images: move the sender.
#
# Settings: TW_DATA=~/tw-data PORT=/dev/ttyACM0 VENV=~/scratch-esp DASH_TAP=47001
set -u
KIT=$(cd "$(dirname "$0")" && pwd)
TW_DATA=${TW_DATA:-$HOME/tw-data}; PORT=${PORT:-/dev/ttyACM0}; VENV=${VENV:-$HOME/scratch-esp}
PY=$VENV/bin/python; [ -x "$PY" ] || PY=python3
DASH_TAP=${DASH_TAP:-47001}
SLEEP_UNITS="sleep.target suspend.target hibernate.target hybrid-sleep.target"
SYSTEMCTL=$(command -v systemctl || echo /usr/bin/systemctl)
. "$KIT/tw_lib.sh"

case "${1:-}" in
  stop)
    tmux has-session -t survey 2>/dev/null || { echo "no survey is running"; exit 1; }
    tmux send-keys -t survey:0 C-c
    echo "stopping the survey"; exit 0 ;;
  -h|--help) sed -n '4,21p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
esac
UNTIL=${1:-$(date -d '+2 hours' +%H:%M)}

command -v tmux >/dev/null || { echo "tmux is missing (sudo apt install tmux)"; exit 1; }
tw_port_free_or_exit survey
[ -e "$PORT" ] || { echo "no receiver at $PORT (the board's native USB port)"; exit 1; }
[ -w "$PORT" ] || { echo "REFUSED: no permission for $PORT (sudo usermod -aG dialout $USER, then log in again)"; exit 1; }
# the layout first: is it still right? (a survey started by a script just shows it)
TW_DATA=$TW_DATA "$PY" "$KIT/tw_layout.py" confirm || exit 1
echo
if [ "$("$SYSTEMCTL" is-enabled sleep.target 2>/dev/null)" != "masked" ]; then
  # shellcheck disable=SC2086
  sudo -n "$SYSTEMCTL" mask $SLEEP_UNITS 2>/dev/null || sudo "$SYSTEMCTL" mask $SLEEP_UNITS >/dev/null \
    || { echo "could not block sleep; stopping"; exit 1; }
fi

DIR=$TW_DATA/survey/$(date +%Y-%m-%d_%H%M)
[ -e "$DIR" ] && { echo "$DIR already exists (two starts in the same minute?); wait a minute"; exit 1; }
mkdir -p "$DIR"
tw_scene_and_layout "$DIR" "Scene for this survey (which boards move, what is parked)"
REC=$DIR/commands-recorder.sh
{
  echo "#!/bin/bash"
  printf '%q ' "$PY" "$KIT/listen.py" "$PORT" "$DIR" --until "$UNTIL" --tap "$DASH_TAP"; echo
  printf 'if sudo -n %q unmask %s 2>/dev/null; then echo "sleep is allowed again"; ' "$SYSTEMCTL" "$SLEEP_UNITS"
  printf 'else echo "undo the sleep block: %q unmask"; fi\n' "$KIT/run.sh"
  tw_chain_end survey
} > "$REC"
DASHCMD=$DIR/commands-dash.sh
{
  echo "#!/bin/bash"
  printf '%q ' "$PY" "$KIT/dash.py" --tap "$DASH_TAP" --folder "$DIR" --until "$UNTIL" --survey; echo
  echo "echo 'dashboard closed (the survey carries on); reopen it: ~/tw-kit/dash.sh'; sleep 4"
} > "$DASHCMD"
chmod +x "$REC" "$DASHCMD"
tmux new-session -d -s survey -n listener "bash $REC"
tmux set-environment -t survey TW_RUN "$DIR"
tmux new-window -d -t survey:1 -n dash "bash $DASHCMD"
printf 'survey %s\n' "$DASHCMD" > "$TW_DATA/.dash-current"
sleep 3
if ! pgrep -f "^$PY $KIT/listen.py $PORT" >/dev/null; then
  echo "the listener stopped at once:"
  tmux capture-pane -p -t survey:0 2>/dev/null | grep -v '^$' | tail -6
  tmux kill-session -t survey 2>/dev/null
  # shellcheck disable=SC2086
  sudo -n "$SYSTEMCTL" unmask $SLEEP_UNITS 2>/dev/null
  exit 1
fi
echo "survey running until $UNTIL; folder $DIR"
echo "dashboard: ~/tw-kit/dash.sh   stop: $0 stop"
if [ -t 0 ] && [ -t 1 ]; then
  exec "$KIT/dash.sh"
fi
