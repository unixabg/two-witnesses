#!/bin/bash
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
# flash_board.sh - set up one board on the cable, step by step: flash it
# (a new board: the whole image; a tw_node board: all but its settings), its
# group, its board number, its group's settings (and the channel, for the
# first board of a new group), its LED, then a health check. It asks before
# each change.
#
# Usage:   ~/tw-kit/flash_board.sh
# Settings (environment): PORT=/dev/ttyACM0  VENV=~/scratch-esp
set -u
KIT=$(cd "$(dirname "$0")" && pwd)
. "$KIT/tw_lib.sh"                      # the settings (PORT, VENV, PY, ...)
case "${1:-}" in -h|--help) sed -n '4,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;; esac
exec "$PY" "$KIT/flash_board.py" "$PORT"
