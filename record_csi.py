#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
"""record_csi.py - save CSI lines from the receiver board, each stamped with
the Linux clock (set it with chrony first).

Usage:
    ~/scratch-esp/bin/python record_csi.py PORT NAME SECONDS [BAUD]
    e.g. ~/scratch-esp/bin/python record_csi.py /dev/ttyACM0 empty-01 600
BAUD defaults to 2000000 (receiver v2); use 921600 for the first receiver.

Writes NAME.csv: host_ns,<the board's CSI_DATA line>
Prints a one-line count every 10 s so you can see it is alive.
"""
import sys
import time

import serial  # comes with esptool (pyserial)

if len(sys.argv) not in (4, 5):
    sys.exit(__doc__)

port, name, seconds = sys.argv[1], sys.argv[2], float(sys.argv[3])
baud = int(sys.argv[4]) if len(sys.argv) == 5 else 2000000
out_path = f"{name}.csv"

ser = serial.Serial(port, baud, timeout=1)
ser.reset_input_buffer()

start = time.time()
last_report = start
n_ok = n_other = 0
first_id = last_id = None

with open(out_path, "w") as out:
    out.write(f"# name={name} port={port} baud={baud} start_unix={start:.3f}\n")
    buf = b""
    while time.time() - start < seconds:
        # block reads: readline() goes byte by byte and the Chromebook could not keep up
        chunk = ser.read(max(1, min(ser.in_waiting, 1 << 16)))
        if not chunk:
            continue
        stamp = time.time_ns()
        buf += chunk
        if b"\n" not in chunk:
            continue
        *raws, buf = buf.split(b"\n")
        for raw in raws:
            line = raw.decode("ascii", errors="replace").strip()
            if line.startswith("CSI_DATA,"):
                out.write(f"{stamp},{line}\n")
                n_ok += 1
                try:
                    pkt_id = int(line.split(",")[1])
                    first_id = pkt_id if first_id is None else first_id
                    last_id = pkt_id
                except ValueError:
                    pass
            elif line:
                n_other += 1
        now = time.time()
        if now - last_report >= 10:
            elapsed = now - start
            print(f"{elapsed:6.0f} s  csi={n_ok}  ({n_ok / elapsed:.1f}/s)  other={n_other}", flush=True)
            last_report = now

lost = ""
if first_id is not None:
    expected = last_id - first_id + 1
    lost = f"  sender ids {first_id}..{last_id}: {expected - n_ok} missing of {expected}"
print(f"done: {n_ok} CSI lines in {out_path}{lost}")
