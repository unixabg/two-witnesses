#!/bin/bash
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
# after_run.sh - make the views of a finished recording. run.sh and watch.sh
# call it at the end; you can also run it by hand on any run folder.
#
# Usage:   ~/tw-kit/after_run.sh FOLDER [view.py options]
#
# One link (the two-board kits): view.py on the folder -> view.png, view.txt.
# Many boards (tw_node): split_links.py --view -> FOLDER/links/<link>/view.*
# for every link, and FOLDER/links/across.txt comparing them.
# (view.py options such as --usual apply to one-link recordings only.)
set -u
KIT=$(cd "$(dirname "$0")" && pwd)
VENV=${VENV:-$HOME/scratch-esp}
PY=$VENV/bin/python; [ -x "$PY" ] || PY=python3
DIR=${1:-}
[ -n "$DIR" ] && [ -d "$DIR" ] || { sed -n '4,14p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
shift
FIRST=$(ls "$DIR"/csi-*.csv.gz 2>/dev/null | head -1)
[ -n "$FIRST" ] || { echo "no recording in $DIR"; exit 1; }
if zcat -f "$FIRST" 2>/dev/null | head -400 | grep -q '^[0-9]*,TW[RSNH],'; then
  echo "many-board recording: splitting it into links and making each link's view (this takes a while)"
  if "$PY" "$KIT/split_links.py" "$DIR" --view > "$DIR/view.out" 2>&1; then
    echo "views ready: $DIR/links/across.txt (all links together), $DIR/links/<link>/view.png and view.txt"
  else
    echo "the views failed: see $DIR/view.out"; exit 1
  fi
else
  if "$PY" "$KIT/view.py" "$DIR" "$@" > "$DIR/view.out" 2>&1; then
    echo "view ready: $DIR/view.png"
  else
    echo "view failed: see $DIR/view.out"; exit 1
  fi
fi
