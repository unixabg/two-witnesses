#!/bin/bash
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
# idf.sh - run a command with ESP-IDF's environment, in firmware/tw_node (the Makefile uses it).
#   tools/idf.sh idf.py build
#   IDF_EXPORT=~/esp-idf/export.sh by default (ESP-IDF v5.5.6)
set -e
EXPORT=${IDF_EXPORT:-$HOME/esp-idf/export.sh}
[ -r "$EXPORT" ] || { echo "ESP-IDF not found: $EXPORT (install v5.5.6, or make ... IDF_EXPORT=path/to/export.sh)" >&2; exit 1; }
. "$EXPORT" >/dev/null 2>&1 || { echo "ESP-IDF's $EXPORT failed (run it by hand to see why)" >&2; exit 1; }
cd "$(dirname "$0")/../firmware/tw_node"
exec "$@"
