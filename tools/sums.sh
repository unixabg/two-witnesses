#!/bin/bash
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
# sums.sh - refresh SHA256SUMS after any change (version.sh and setup_once.sh check it).
#   tools/sums.sh            from anywhere in the repository; commit the result
set -e
cd "$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
git ls-files | grep -v '^SHA256SUMS$' | sort | xargs sha256sum > SHA256SUMS
echo "SHA256SUMS: $(grep -c . SHA256SUMS) files (VERSION: $(cat VERSION))"
