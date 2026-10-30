#!/bin/bash
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
# dash.sh - open the live dashboard of whatever is running (run.sh, watch.sh,
# survey.sh or live.sh). Works over SSH. Leave it with Ctrl-b then d (it keeps
# running), or q (closes the dashboard; the recording carries on, and
# dash.sh opens a fresh one next time).
set -u
TW_DATA=${TW_DATA:-$HOME/tw-data}
CUR=$TW_DATA/.dash-current
SESSION=""; CMD=""
[ -r "$CUR" ] && read -r SESSION CMD < "$CUR"
if [ -z "$SESSION" ] || ! tmux has-session -t "$SESSION" 2>/dev/null; then
  SESSION=""
  for s in watch run survey live; do tmux has-session -t $s 2>/dev/null && { SESSION=$s; break; }; done
  [ -z "$SESSION" ] && { echo "nothing is running (start one with run.sh, watch.sh or survey.sh)"; exit 1; }
  CMD=""
fi
if ! tmux list-windows -t "$SESSION" -F '#W' 2>/dev/null | grep -qx dash; then
  if [ -n "$CMD" ] && [ -r "$CMD" ]; then
    tmux new-window -d -t "$SESSION" -n dash "bash $CMD"
  else
    echo "this $SESSION run was started without a dashboard (kit before v4.8)"; exit 1
  fi
fi
if [ -n "${TMUX:-}" ]; then
  exec tmux switch-client -t "$SESSION:dash"
else
  exec tmux attach -t "$SESSION:dash"
fi
