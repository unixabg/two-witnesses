#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
"""tw_setid.py - talk to the board on the computer's USB port (tw_node firmware).

    ~/scratch-esp/bin/python tw_setid.py PORT                 who is it; each board's power and relaying
    ~/scratch-esp/bin/python tw_setid.py PORT N               give it board number N (1-16), once
    ~/scratch-esp/bin/python tw_setid.py PORT power DBM       set the transmit power of EVERY board
    ~/scratch-esp/bin/python tw_setid.py PORT power default   back to full power (20 dBm)
    ~/scratch-esp/bin/python tw_setid.py PORT relay off       relaying off on every board (a comparison run)
    ~/scratch-esp/bin/python tw_setid.py PORT relay on        back on (the default)
    ~/scratch-esp/bin/python tw_setid.py PORT second off      no second frame in a board's slot (tw_node 2.1)
    ~/scratch-esp/bin/python tw_setid.py PORT second on       a second frame when records wait (the default)
    ~/scratch-esp/bin/python tw_setid.py PORT second force    test: every board, every slot, full-length frames
    ~/scratch-esp/bin/python tw_setid.py PORT measure-second on|off   test: measure the second frames too
    ~/scratch-esp/bin/python tw_setid.py PORT cap N|off       test: at most N records per frame (1-15)
    e.g. ~/scratch-esp/bin/python tw_setid.py /dev/ttyACM0 power 11

Board number: the board stores it in its flash and restarts; erase-flash clears it.
New boards have no number until it is set here (or listed in the firmware's
table); until then they say they are board 16. Every board needs its own.

Power and relaying (network settings): 2-20 dBm, in quarter-dBm steps; relaying
on or off. The board on the cable passes the setting to all the others in its
frames; each keeps it in flash, and a board that was off picks it up when it
comes back. Only one program can use the port: stop live.sh or a run first (and
change one thing per run, as always: each run records the settings it used).

The second frame and its test settings (tw_node 2.1, docs/SECOND-FRAME.md) are
network settings too, kept in flash: after a test put them back
(second on, measure-second off, cap off). This shows them for every board.

The board's USB line can drop a command or an answer when it is busy, so this
tries up to three times and only says "done" when the board itself reports the
new number or setting.
"""
import sys
import time

import serial

TRIES = 3
SECOND_OFF, SECOND_FORCE, MEASURE, CAP_SHIFT = 0x02, 0x04, 0x08, 4     # setting flags (tw_proto.h)


class Port:
    """the serial port, reopened if the board restarts (its USB port vanishes for a moment)"""

    def __init__(self, path):
        self.path = path
        self.s = None
        self.open(5)

    def open(self, seconds):
        end = time.time() + seconds
        while True:
            try:
                self.s = serial.Serial(self.path, 115200, timeout=0.2)
                self.s.reset_input_buffer()
                return
            except (serial.SerialException, OSError):
                if time.time() > end:
                    sys.exit(f"cannot open {self.path} (is something else using it, or is the board unplugged?)")
                time.sleep(0.3)

    def send(self, text):
        # a newline first: ends anything half-received on the board's side
        self.s.write(f"\n{text}\n".encode())
        self.s.flush()

    def lines(self, seconds):
        end, buf = time.time() + seconds, b""
        while time.time() < end:
            try:
                buf += self.s.read(4096)
            except (serial.SerialException, OSError):
                try:
                    self.s.close()
                except Exception:
                    pass
                time.sleep(0.5)
                self.open(max(end - time.time(), 1))
                buf = b""
                continue
            *got, buf = buf.split(b"\n")
            for raw in got:
                yield raw.decode(errors="replace").strip()


def dbm(q):
    q = int(q)
    return "full power (never set)" if q == 0 else f"{q / 4:.2f} dBm"


def status(p, seconds):
    """what every board reports: TWS for the board on the cable, TWN for each board it hears.
    -> {board: dict(set_seq, power_q, relay, heard, cable)}"""
    seen = {}
    for line in p.lines(seconds):
        f = line.split(",")
        try:
            if f[0] == "TWS" and len(f) >= 20:
                seen[int(f[1])] = dict(cable=True, set_seq=int(f[17]), power_q=int(f[18]), radio_q=int(f[19]),
                                       relay=int(f[20]) if len(f) >= 21 else None, heard=None,
                                       relayed=int(f[21]) if len(f) >= 22 else None,
                                       flags=int(f[23]) if len(f) >= 24 else None)     # 2.1+
            elif f[0] == "TWN" and len(f) >= 11:
                seen[int(f[1])] = dict(cable=False, set_seq=int(f[9]), power_q=int(f[10]),
                                       relay=int(f[11]) if len(f) >= 12 else None,
                                       heard=int(f[12]) if len(f) >= 13 else None,
                                       flags=int(f[13]) if len(f) >= 14 else None)     # 2.1+
        except (ValueError, IndexError):
            pass
    return seen


def flags_text(f):
    """the 2.1 setting flags in words (the defaults said briefly, the test settings loudly)"""
    if f is None:
        return ""
    second = "FORCED (test)" if f & SECOND_FORCE else "OFF" if f & SECOND_OFF else "on"
    t = f", second frame {second}"
    if f & MEASURE:
        t += ", MEASURING second frames (test)"
    if f >> CAP_SHIFT:
        t += f", CAP {f >> CAP_SHIFT} records per frame (test)"
    return t


def show(seen):
    for k in sorted(seen):
        b = seen[k]
        relay = "" if b["relay"] is None else f", relaying {'on' if b['relay'] else 'OFF'}"
        relay += flags_text(b.get("flags"))
        if b["cable"]:
            print(f"  board {k} (on the cable): setting {b['set_seq']}, {dbm(b['power_q'])}{relay}; "
                  f"radio reports {dbm(b['radio_q'])}")
        else:
            heard = "" if b["heard"] is None else f"; the board on the cable hears {b['heard']}% of its frames"
            print(f"  board {k}: setting {b['set_seq']}, {dbm(b['power_q'])}{relay}{heard}")
    if not seen:
        print("  no status lines heard (is it running tw_node?)")


def set_network(p, command, check, what):
    """send a network setting until the board on the cable reports it (check(status) -> bool)"""
    for attempt in range(1, TRIES + 1):
        p.send(command)
        if attempt == 1:
            print(f"asked the board on {p.path} to set {what} on every board ...")
        answered = False
        for line in p.lines(3):
            if line.startswith("TWERR"):
                sys.exit(line)
            if line.startswith("TWOK"):
                print(line)
                answered = True
                break
        seen = status(p, 3)
        cable = [b for b in seen.values() if b["cable"]]
        if cable and check(cable[0]):
            if not answered:
                print("(its answer was lost on the way, but it reports the new setting)")
            print("each board takes it within a second; what they report now:")
            show(status(p, 4) or seen)
            return
        if cable and cable[0].get("relay") is None and what.startswith("relaying"):
            sys.exit("this board's firmware has no relaying: it needs tw_node 2.0 or later")
        if cable and cable[0].get("flags") is None and what.startswith(("second", "measure", "cap")):
            sys.exit("this board's firmware has no second frame: it needs tw_node 2.1 or later")
        print(f"  no confirmation yet (try {attempt} of {TRIES})")
    sys.exit("the board did not take the setting: is it running tw_node 1.1 or later "
             "(tw_setid.py PORT shows the firmware), and is nothing else using the port?")


def main():
    a = sys.argv[1:]
    if not a or len(a) > 3:
        sys.exit(__doc__)
    p = Port(a[0])

    if len(a) >= 2 and a[1] == "power":
        if len(a) != 3:
            sys.exit("power wants a value: 2..20 (dBm) or default")
        val = a[2]
        if val == "default":
            want = 80
        else:
            try:
                v = float(val)
            except ValueError:
                sys.exit("power must be a number of dBm (2..20) or default")
            if not 2 <= v <= 20:
                sys.exit("power must be 2..20 dBm")
            want = int(v * 4 + 0.5)
        set_network(p, f"TWSET power {val}", lambda b: b["power_q"] == want, f"transmit power {val}")
        return

    if len(a) >= 2 and a[1] == "relay":
        if len(a) != 3 or a[2] not in ("on", "off"):
            sys.exit("relay on, or relay off")
        want = 1 if a[2] == "on" else 0
        set_network(p, f"TWSET relay {a[2]}", lambda b: b.get("relay") == want, f"relaying {a[2]}")
        return

    if len(a) >= 2 and a[1] in ("second", "measure-second", "cap"):
        mask = {"second": SECOND_OFF | SECOND_FORCE, "measure-second": MEASURE, "cap": 0xF0}[a[1]]
        if len(a) != 3:
            sys.exit("second on|off|force, measure-second on|off, or cap N|off")
        val = a[2]
        if a[1] == "second" and val in ("on", "off", "force"):
            want = {"on": 0, "off": SECOND_OFF, "force": SECOND_FORCE}[val]
        elif a[1] == "measure-second" and val in ("on", "off"):
            want = MEASURE if val == "on" else 0
        elif a[1] == "cap" and (val == "off" or (val.isdigit() and 1 <= int(val) <= 15)):
            want = 0 if val == "off" else int(val) << CAP_SHIFT
        else:
            sys.exit("second on|off|force, measure-second on|off, or cap N (1-15) | cap off")
        set_network(p, f"TWSET {a[1]} {val}", lambda b: b.get("flags") is not None and b["flags"] & mask == want,
                    f"{a[1]} {val}")
        return

    if len(a) == 2:
        try:
            n = int(a[1])
        except ValueError:
            sys.exit(__doc__)
        if not 1 <= n <= 16:
            sys.exit("board number must be 1..16")
        for attempt in range(1, TRIES + 1):
            p.send(f"TWSET id {n}")
            if attempt == 1:
                print(f"asked the board on {p.path} to become board {n} ...")
            # it answers TWOK and restarts; it says hello (TWH) as soon as it is back
            for line in p.lines(12):
                if line.startswith("TWOK"):
                    print(line)
                if line.startswith("TWH"):
                    f = line.split(",")
                    if f[1] == str(n):
                        print(line)
                        print(f"done: it is board {n} (firmware {f[3]})")
                        return
                    break           # still the old number: the command did not get through
            print(f"  no confirmation yet (try {attempt} of {TRIES})")
        sys.exit("no confirmation: is it running tw_node, and is this its native USB port? "
                 "(check with: tw_setid.py PORT)")

    for line in p.lines(12):
        if line.startswith("TWH"):
            f = line.split(",")
            print(line)
            print(f"board {f[1]}, factory address {f[2]}, firmware {f[3]}")
            print("every board:")
            show(status(p, 3))
            return
    sys.exit("no answer: is it running tw_node, and is this its native USB port? (it says hello every 10 s)")


if __name__ == "__main__":
    main()
