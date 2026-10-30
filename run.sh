#!/bin/bash
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
# run.sh - one command to start a long run (a night, a day, as long as you
# like). Starts the listener in tmux (so closing the terminal or the SSH
# link does not stop it), blocks sleep, and makes the view when it stops.
#
# Usage:
#   ./run.sh UNTIL            UNTIL = clock time to stop, e.g. 07:00 or 17:30
#   ./run.sh stop             stop early (the view is still made)
#   ./run.sh unmask           let the computer sleep again (if the end of the run did not)
# Watch it:   ~/tw-kit/dash.sh        the live dashboard (q closes it; the run carries on)
#             tmux attach -t run      the listener itself (leave again: Ctrl-b then d)
# Afterwards: open ~/tw-data/runs/<date>_<start time>/view.png and view.txt
#             (many boards: links/across.txt, and links/<link>/view.png and view.txt)
# When it is done, its tmux window stays open 30 minutes, then closes itself.
#
# Each run gets its own folder, ~/tw-data/runs/YYYY-MM-DD_HHMM (the date and
# clock time it started), so a day run and a night run on the same date never mix.
# The scene (which cars, doors, people, what changed) goes in the folder's
# scene.txt, shown at the top of view.txt: ~/tw-data/scene-now.txt is the
# standing scene (Enter keeps it, typing replaces it; cron uses it as it is);
# SCENE="..." gives one for this run only. ~/tw-data/boards.csv (the layout)
# is copied into the folder.
# Sleep is blocked with systemctl mask; setup_once.sh allows that one command
# without a password, so the block is undone by itself when the run ends.
# Without that, sudo asks once here, and the end of the run asks you to unmask.
#
# Settings (environment): TW_DATA=~/tw-data  PORT=/dev/ttyACM0  VENV=~/scratch-esp
#   USUAL=path/to/usual.json   judge this run against a quiet run's usual levels
#   QUIET=HH:MM-HH:MM          or take "usual" from that stretch of this run only
#   DASH_TAP=47001             the port the dashboard listens on
set -u
KIT=$(cd "$(dirname "$0")" && pwd)
TW_DATA=${TW_DATA:-$HOME/tw-data}; PORT=${PORT:-/dev/ttyACM0}; VENV=${VENV:-$HOME/scratch-esp}
PY=$VENV/bin/python; [ -x "$PY" ] || PY=python3
DASH_TAP=${DASH_TAP:-47001}
SLEEP_UNITS="sleep.target suspend.target hibernate.target hybrid-sleep.target"
SYSTEMCTL=$(command -v systemctl || echo /usr/bin/systemctl)
. "$KIT/tw_lib.sh"

# shellcheck disable=SC2086
unmask_sleep() { sudo -n "$SYSTEMCTL" unmask $SLEEP_UNITS 2>/dev/null; }

case "${1:-}" in
  stop)
    tmux has-session -t run 2>/dev/null || { echo "no run is going"; exit 1; }
    tmux send-keys -t run:0 C-c
    echo "stopping; the view is being made (tmux attach -t run)"; exit 0 ;;
  unmask)
    # shellcheck disable=SC2086
    unmask_sleep || sudo "$SYSTEMCTL" unmask $SLEEP_UNITS
    echo "sleep is allowed again"; exit 0 ;;
  ""|-h|--help) sed -n '4,33p' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
UNTIL=$1
RUN=$TW_DATA/runs/$(date +%Y-%m-%d_%H%M)

command -v tmux >/dev/null || { echo "tmux is missing (sudo apt install tmux)"; exit 1; }
tw_port_free_or_exit run
[ -e "$PORT" ] || { echo "no receiver at $PORT (plug the receiver in: the board's native USB port)"; exit 1; }
if [ ! -w "$PORT" ]; then
  # sudo inside tmux would ask for a password where nobody sees it, and hang for hours
  echo "REFUSED: you cannot write to $PORT without sudo."
  echo "  for now:       sudo chmod a+rw $PORT     (lasts until the board is unplugged)"
  echo "  for good:      sudo usermod -aG dialout $USER   then log out and in"
  exit 1
fi
[ -e "$RUN" ] && { echo "$RUN already exists (two starts in the same minute?); wait a minute"; exit 1; }

# Keep the computer awake. systemd-inhibit asked for a password inside tmux, where
# nobody could answer (night of 2026-10-24), so block sleep here, in the open, instead.
if [ "$("$SYSTEMCTL" is-enabled sleep.target 2>/dev/null)" != "masked" ]; then
  # shellcheck disable=SC2086
  if sudo -n "$SYSTEMCTL" mask $SLEEP_UNITS 2>/dev/null; then
    echo "sleep is blocked (and will be unblocked when the run ends)"
  else
    echo "blocking sleep for the run (sudo asks for your password):"
    # shellcheck disable=SC2086
    sudo "$SYSTEMCTL" mask $SLEEP_UNITS >/dev/null || { echo "could not block sleep; stopping"; exit 1; }
    echo "sleep is blocked. When the run ends: $0 unmask   (setup_once.sh makes this automatic)"
  fi
fi
if [ -r /sys/class/power_supply/AC/online ] && [ "$(cat /sys/class/power_supply/AC/online)" = 0 ]; then
  echo "WARNING: on battery. It lasted under 5 hours on 2026-10-24; plug in for a long run."
fi
if command -v chronyc >/dev/null && ! chronyc tracking | grep -q "Leap status *: Normal"; then
  echo "WARNING: chrony not synchronised; clock times in the view may be off (sudo chronyc makestep)"
fi

mkdir -p "$RUN"
tw_scene_and_layout "$RUN" "Scene (which cars, doors, people, what changed)"

MV_ARGS=()
[ -n "${USUAL:-}" ] && MV_ARGS+=(--usual "$USUAL")
[ -n "${QUIET:-}" ] && MV_ARGS+=(--quiet "$QUIET")

# The run's chain is a script in its folder: no quoting puzzles inside tmux,
# and a record of exactly what ran.
CHAIN=$RUN/commands.sh
{
  echo "#!/bin/bash"
  echo "# written by run.sh on $(date '+%Y-%m-%d %H:%M:%S')"
  printf '%q ' "$PY" "$KIT/listen.py" "$PORT" "$RUN" --until "$UNTIL" --tap "$DASH_TAP"; echo
  printf '%q ' "$KIT/after_run.sh" "$RUN" "${MV_ARGS[@]}"; echo
  printf 'if sudo -n %q unmask %s 2>/dev/null; then echo "sleep is allowed again"; ' "$SYSTEMCTL" "$SLEEP_UNITS"
  printf 'else echo "undo the sleep block: %q unmask"; fi\n' "$KIT/run.sh"
  tw_chain_end run
} > "$CHAIN"
chmod +x "$CHAIN"

tmux new-session -d -s run -n listener "bash $CHAIN"
tmux set-environment -t run TW_RUN "$RUN"
DASHCMD=$RUN/commands-dash.sh
{
  echo "#!/bin/bash"
  printf '%q ' "$PY" "$KIT/dash.py" --tap "$DASH_TAP" --folder "$RUN" --until "$UNTIL"; echo
  echo "echo 'dashboard closed (the run carries on); reopen it: ~/tw-kit/dash.sh'; sleep 4"
} > "$DASHCMD"
tmux new-window -d -t run:1 -n dash "bash $DASHCMD"
printf 'run %s\n' "$DASHCMD" > "$TW_DATA/.dash-current"
sleep 4
# anchored to the python process itself: the tmux shell that runs the chain also has
# "listen.py" in its command line, and would match a plain pgrep -f even once
# the listener had died
if pgrep -f "^$PY $KIT/listen.py $PORT" >/dev/null; then
  tmux capture-pane -p -t run | grep -v '^$' | tail -5
  echo
  echo "listening until $UNTIL. Files: $RUN"
  echo "dashboard: ~/tw-kit/dash.sh   (q closes it; the run carries on)     stop early: $0 stop"
else
  tmux capture-pane -p -t run 2>/dev/null | grep -v '^$' | tail -8
  tmux kill-session -t run 2>/dev/null
  unmask_sleep && echo "(sleep allowed again)"
  echo "the listener stopped at once; run it by hand to see why:"
  echo "  $PY $KIT/listen.py $PORT $RUN --until $UNTIL"
fi
