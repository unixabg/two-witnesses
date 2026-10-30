#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
"""tw_setid.py - give a board running tw_node its board number (1-16), once.

    ~/scratch-esp/bin/python tw_setid.py PORT N
    e.g. ~/scratch-esp/bin/python tw_setid.py /dev/ttyACM0 4

The board stores the number in its flash and restarts; it keeps it through
reflashing the same image (erase-flash clears it). New boards
have no number until it is set here (or listed in the firmware's table);
until then they say they are board 16. Every board needs its own number.

    ~/scratch-esp/bin/python tw_setid.py PORT      just show what the board says
"""
import sys
import time

import serial


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    port = sys.argv[1]
    s = serial.Serial(port, 115200, timeout=0.2)
    s.reset_input_buffer()
    if len(sys.argv) == 3:
        n = int(sys.argv[2])
        if not 1 <= n <= 16:
            sys.exit("board number must be 1..16")
        s.write(f"TWSET id {n}\n".encode())
        s.flush()
        print(f"asked the board on {port} to become board {n} ...")
    end = time.time() + 12
    buf = b""
    while time.time() < end:
        buf += s.read(4096)
        *lines, buf = buf.split(b"\n")
        for raw in lines:
            line = raw.decode(errors="replace").strip()
            if line.startswith(("TWOK", "TWH")):
                print(line)
                if line.startswith("TWH") and len(sys.argv) == 2:
                    f = line.split(",")
                    print(f"board {f[1]}, factory address {f[2]}, firmware {f[3]}")
                    return
                if line.startswith("TWOK"):
                    print("done: it restarts with the new number (unplug and replug if the port vanished)")
                    return
    sys.exit("no answer: is it running tw_node, and is this its native USB port? (it says hello every 10 s)")


if __name__ == "__main__":
    main()
