#!/bin/bash
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
# after_run.sh - make the views and the checks of a finished recording. run.sh and
# watch.sh call it at the end; you can also run it by hand on any run folder.
#
# Usage:   ~/tw-kit/after_run.sh FOLDER [view.py options]
#
# One link (the two-board kits): view.py on the folder -> view.png, view.txt.
# Many boards (tw_node): split_links.py --view -> FOLDER/links/<link>/view.*
# for every link, and FOLDER/links/across.txt comparing them; then the checks
# (second_check, lag_check, net_check, score_check), each into FOLDER/<check>.txt.
# A check that does not apply to the run (older firmware) says so and the rest go on.
# (view.py options such as --usual apply to one-link recordings only.)
set -u
KIT=$(cd "$(dirname "$0")" && pwd)
. "$KIT/tw_lib.sh"                      # the settings (VENV, PY, ...) and shared steps
DIR=${1:-}
[ -n "$DIR" ] && [ -d "$DIR" ] || { sed -n '4,15p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
shift
FIRST=$(ls "$DIR"/csi-*.csv.gz 2>/dev/null | head -1)
[ -n "$FIRST" ] || { echo "no recording in $DIR"; exit 1; }
if zcat -f "$FIRST" 2>/dev/null | head -400 | grep -q '^[0-9]*,TW[RSNH],'; then
  STATUS=0
  echo "many-board recording: splitting it into links and making each link's view (this takes a while)"
  if "$PY" "$KIT/split_links.py" "$DIR" --view > "$DIR/view.out" 2>&1; then
    echo "views ready: $DIR/links/across.txt (all links together), $DIR/links/<link>/view.png and view.txt"
  else
    echo "the views failed: see $DIR/view.out"; STATUS=1
  fi
  # the checks: each reads the recording itself (not the views), so each runs whatever the others did
  : > "$DIR/checks.out"
  for c in second_check lag_check net_check score_check; do
    echo "== $c" >> "$DIR/checks.out"
    rm -f "$DIR/$c.txt"                 # what it says now, not an earlier one
    if "$PY" "$KIT/$c.py" "$DIR" >> "$DIR/checks.out" 2>&1 && [ -s "$DIR/$c.txt" ]; then
      echo "  $c: $DIR/$c.txt"
    else
      echo "  $c: nothing for this run (see $DIR/checks.out)"
    fi
  done
  exit $STATUS
else
  if "$PY" "$KIT/view.py" "$DIR" "$@" > "$DIR/view.out" 2>&1; then
    echo "view ready: $DIR/view.png"
  else
    echo "view failed: see $DIR/view.out"; exit 1
  fi
fi
