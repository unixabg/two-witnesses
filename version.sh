#!/bin/bash
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
# version.sh - which kit is this, and is every file intact?
#   ~/tw-kit/version.sh
KIT=$(cd "$(dirname "$0")" && pwd)
echo "two-witnesses kit v$(cat "$KIT/VERSION" 2>/dev/null || echo '? (no VERSION file: before v5.3)')   in $KIT"
if (cd "$KIT" && sha256sum -c --quiet SHA256SUMS 2>/dev/null); then
  echo "files:    all $(grep -c . "$KIT/SHA256SUMS") match SHA256SUMS"
else
  echo "files:    PROBLEM: these do not match SHA256SUMS:"
  (cd "$KIT" && sha256sum -c SHA256SUMS 2>/dev/null | grep -v ': OK$' | sed 's/^/          /')
fi
FW=$(sha256sum "$KIT/tw_node-s3-ch11.bin" 2>/dev/null | cut -c1-16)
echo "firmware: tw_node-s3-ch11.bin ${FW:-missing}... (the boards say theirs in tw_setid.py / TWH lines)"
D=${TW_DATA:-$HOME/tw-data}
L=""
for f in "$KIT/private/boards.csv" "$D/boards.csv"; do [ -r "$f" ] && { L=$f; break; }; done
[ -n "$L" ] && echo "layout:   $L ($(grep -c '^[0-9]' "$L") boards)" || echo "layout:   none (private/boards.csv or $D/boards.csv; map view off)"
[ -L "$KIT" ] && echo "install:  $KIT is a link to $(readlink -f "$KIT") (a git clone: git pull updates it)"
[ -r "$D/scene-now.txt" ] && echo "scene:    $(head -c 120 "$D/scene-now.txt")" || echo "scene:    no standing scene yet ($D/scene-now.txt)"
