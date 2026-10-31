#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
"""tw_setid.py - talk to the board on the computer's USB port (tw_node firmware).

    ~/scratch-esp/bin/python tw_setid.py PORT                 who is it (board number, firmware, power)
    ~/scratch-esp/bin/python tw_setid.py PORT N               give it board number N (1-16), once
    ~/scratch-esp/bin/python tw_setid.py PORT power DBM       set the transmit power of EVERY board
    ~/scratch-esp/bin/python tw_setid.py PORT power default   back to full power (20 dBm)
    e.g. ~/scratch-esp/bin/python tw_setid.py /dev/ttyACM0 power 11

Board number: the board stores it in its flash and restarts; erase-flash clears it.
New boards have no number until it is set here (or listed in the firmware's
table); until then they say they are board 16. Every board needs its own.

Power (firmware tw_node 1.1 or later): 2-20 dBm, in quarter-dBm steps. The board
on the cable passes the setting to all the others in its frames; each keeps it
in flash, and a board that was off picks it up when it comes back. This command
then listens for a few seconds and shows the power each board reports. Only one
program can use the port: stop live.sh or a run first (and change one thing per
run, as always: each run records the power its boards used).
"""
import sys
import time

import serial


def lines(s, seconds):
    end, buf = time.time() + seconds, b""
    while time.time() < end:
        buf += s.read(4096)
        *got, buf = buf.split(b"\n")
        for raw in got:
            yield raw.decode(errors="replace").strip()


def dbm(q):
    q = int(q)
    return "full power (never set)" if q == 0 else f"{q / 4:.2f} dBm"


def show_power(s, seconds=6):
    """the power every board reports: TWS for the board on the cable, TWN for each board it hears"""
    seen = {}
    for line in lines(s, seconds):
        f = line.split(",")
        if f[0] == "TWS" and len(f) >= 20:
            seen[int(f[1])] = f"board {f[1]} (on the cable): setting {f[17]}, {dbm(f[18])}; radio reports {dbm(f[19])}"
        elif f[0] == "TWN" and len(f) >= 11:
            seen[int(f[1])] = f"board {f[1]}: setting {f[9]}, {dbm(f[10])}"
        elif f[0] == "TWN" and len(f) >= 9 and int(f[1]) not in seen:
            seen[int(f[1])] = f"board {f[1]}: older firmware (no power setting; stays at full power)"
    for k in sorted(seen):
        print("  " + seen[k])
    if not seen:
        print("  no status lines heard (is it running tw_node?)")


def main():
    a = sys.argv[1:]
    if not a or len(a) > 3:
        sys.exit(__doc__)
    port = a[0]
    s = serial.Serial(port, 115200, timeout=0.2)
    s.reset_input_buffer()

    if len(a) >= 2 and a[1] == "power":
        if len(a) != 3:
            sys.exit("power wants a value: 2..20 (dBm) or default")
        val = a[2]
        if val != "default":
            try:
                v = float(val)
            except ValueError:
                sys.exit("power must be a number of dBm (2..20) or default")
            if not 2 <= v <= 20:
                sys.exit("power must be 2..20 dBm")
        s.write(f"TWSET power {val}\n".encode())
        s.flush()
        print(f"asked the board on {port} to set every board's power to {val} ...")
        for line in lines(s, 4):
            if line.startswith("TWOK power"):
                print(line)
                print("each board takes it within a second; what they report now:")
                show_power(s)
                return
            if line.startswith("TWERR"):
                sys.exit(line)
        sys.exit("no answer to the power command: this needs firmware tw_node 1.1 or later "
                 "(tw_setid.py PORT shows the firmware)")

    if len(a) == 2:
        try:
            n = int(a[1])
        except ValueError:
            sys.exit(__doc__)
        if not 1 <= n <= 16:
            sys.exit("board number must be 1..16")
        s.write(f"TWSET id {n}\n".encode())
        s.flush()
        print(f"asked the board on {port} to become board {n} ...")

    for line in lines(s, 12):
        if line.startswith("TWH") and len(a) == 1:
            f = line.split(",")
            print(line)
            print(f"board {f[1]}, factory address {f[2]}, firmware {f[3]}")
            print("power:")
            show_power(s, 3)
            return
        if line.startswith("TWOK"):
            print(line)
            print("done: it restarts with the new number (unplug and replug if the port vanished)")
            return
    sys.exit("no answer: is it running tw_node, and is this its native USB port? (it says hello every 10 s)")


if __name__ == "__main__":
    main()
