# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
# tw_lib.sh - shared by run.sh, watch.sh, survey.sh and live.sh (sourced, not run).
#
# Each of them runs in its own tmux session (run, watch, survey, live). When a
# session's work is done (recording, view, sheets), its last step marks the
# session finished (tmux environment TW_FINISHED=1), keeps its window open 30
# minutes for anyone looking, then closes the session. A finished session that
# is still open is closed by the next start, so it can never block a run
# (it blocked the cron start once, 2026-10-28).

TW_SESSIONS="run watch survey live"

# close SESSION if it is open but finished; say so
tw_close_finished() {
  local s=$1
  tmux has-session -t "$s" 2>/dev/null || return 0
  if tmux show-environment -t "$s" TW_FINISHED 2>/dev/null | grep -qx 'TW_FINISHED=1'; then
    tmux kill-session -t "$s" 2>/dev/null
    echo "(closed the finished '$s' session left open)"
  fi
}

# refuse to start while another recorder holds the port; close finished ones first.
# Usage: tw_port_free_or_exit MYSESSION
tw_port_free_or_exit() {
  local me=$1 s
  for s in $TW_SESSIONS; do
    tw_close_finished "$s"
    if tmux has-session -t "$s" 2>/dev/null; then
      if [ "$s" = "$me" ]; then
        echo "'$s' is already going (or still making its view): ~/tw-kit/dash.sh, or tmux attach -t $s"
      elif [ "$s" = live ]; then
        echo "live.sh is showing the boards; close it first (q in its dashboard, or: tmux kill-session -t live)"
      else
        echo "'$s' is going (or still making its view); only one recorder can hold the port"
        echo "  look at it: ~/tw-kit/dash.sh      its folder: $(tmux show-environment -t "$s" TW_RUN 2>/dev/null | cut -d= -f2-)"
      fi
      exit 1
    fi
  done
}

# the lines that end a session's chain script. Usage: tw_chain_end SESSION >> chain.sh
tw_chain_end() {
  local s=$1
  printf 'tmux set-environment -t %q TW_FINISHED 1 2>/dev/null\n' "$s"
  echo "echo 'finished. This window closes by itself in 30 minutes (Enter closes it now).'"
  echo "read -r -t 1800"
  printf 'tmux kill-session -t %q 2>/dev/null\n' "$s"
}

# The scene of a run, and the board layout it ran with, kept in the run's folder.
# ~/tw-data/scene-now.txt is the standing scene (which cars, what changed: "board 2:
# new adapter"). At a keyboard, Enter keeps it and anything typed replaces it; with
# nobody there (cron) it is used as it is. SCENE="..." gives a scene for this run
# only. The layout (see tw_map.py), the kit's private/boards.csv or else
# ~/tw-data/boards.csv, is copied in as boards.csv, so each run keeps the layout
# it had even after boards move.
# Usage: tw_scene_and_layout FOLDER "question"
tw_scene_and_layout() {
  local dir=$1 prompt=$2 standing="$TW_DATA/scene-now.txt" now_scene="" scene="" src=""
  [ -r "$standing" ] && now_scene=$(grep -v '^#' "$standing" | tr '\n' ' ' | sed 's/  */ /g; s/^ //; s/ $//')
  if [ -n "${SCENE+x}" ]; then
    scene=$SCENE; src="given for this run"
  elif [ -t 0 ]; then
    echo "standing scene: ${now_scene:-(none yet)}"
    read -r -p "$prompt (Enter keeps it; anything typed replaces it): " scene
    if [ -n "$scene" ]; then
      printf '%s\n' "$scene" > "$standing"; src="typed; now the standing scene"
    else
      scene=$now_scene; src="standing scene"
    fi
  else
    scene=$now_scene; src="standing scene, nobody at the keyboard"
  fi
  [ -n "$scene" ] && printf '%s  %s   [%s]\n' "$(date '+%Y-%m-%d %H:%M')" "$scene" "$src" >> "$dir/scene.txt"
  local layout
  for layout in "${KIT:-.}/private/boards.csv" "$TW_DATA/boards.csv"; do
    if [ -r "$layout" ]; then cp "$layout" "$dir/boards.csv"; break; fi
  done
  return 0
}
