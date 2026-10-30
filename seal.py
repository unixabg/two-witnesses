#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
"""seal.py - seal an expectation before a recording, open it after.

Seal (before the run):
    python3 seal.py seal fan-01 "fan sweep 11.5 s"
  -> writes fan-01.sealed.txt (keep it closed) and prints its sha256.
     Write the sha256 down somewhere else too (notebook, phone, chat).

Open (after the blind reading):
    python3 seal.py open fan-01
  -> checks the file still matches the sha256 recorded in fan-01.sha256,
     then shows what was sealed.

A random nonce is added so nobody can guess the text from the hash.
"""
import hashlib
import secrets
import sys
import time


def digest(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


if len(sys.argv) == 4 and sys.argv[1] == "seal":
    name, text = sys.argv[2], sys.argv[3]
    body = (f"name: {name}\n"
            f"sealed_at: {time.strftime('%Y-%m-%d %H:%M:%S %Z')}\n"
            f"expect: {text}\n"
            f"nonce: {secrets.token_hex(16)}\n")
    path = f"{name}.sealed.txt"
    with open(path, "x") as f:          # refuses to overwrite an old seal
        f.write(body)
    h = digest(path)
    with open(f"{name}.sha256", "w") as f:
        f.write(f"{h}  {path}\n")
    print(f"sealed {path}\nsha256 {h}")
elif len(sys.argv) == 3 and sys.argv[1] == "open":
    name = sys.argv[2]
    path = f"{name}.sealed.txt"
    want = open(f"{name}.sha256").read().split()[0]
    got = digest(path)
    print("MATCH" if got == want else f"MISMATCH (recorded {want}, file {got})")
    print(open(path).read(), end="")
else:
    sys.exit(__doc__)
