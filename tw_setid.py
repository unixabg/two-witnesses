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
    ~/scratch-esp/bin/python tw_setid.py PORT led on [GPIO]   THIS board's RGB LED on (GPIO 48 if not given; tw_node 2.4)
    ~/scratch-esp/bin/python tw_setid.py PORT led off         THIS board's LED off (the default)
    ~/scratch-esp/bin/python tw_setid.py PORT led bright N    its brightness, 1-255 (32 by default)
    ~/scratch-esp/bin/python tw_setid.py PORT led test        red, green, blue, three blues, off (7 s)
    ~/scratch-esp/bin/python tw_setid.py PORT led try R G B   that color (0-255 each) for 10 s, to choose by eye (2.4.2)
    ~/scratch-esp/bin/python tw_setid.py PORT led on|off|bright|test|try ... board N|all
                                                          the same for board N, or every board, over the air
                                                          (2.5; test and try 2.5.1)
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

The LED (tw_node 2.4) is a setting of each board, kept in its flash: for the
board on the cable, or (2.5) with "board N" or "board all" for others over the
air (a settings record, passed on by every board for 60 s; this waits until the
board reports it took it). It shows the board's own movement score in
the replay's colors: off when quiet, faint pale blue to full deep blue as the
movement grows. Boards without one keep it off. On boards like the ones in
this setup the LED is on GPIO 48; on some, an "RGB" solder pad next to it must
be bridged first (led test shows whether it lights).

The board's USB line can drop a command or an answer when it is busy, so this
tries up to three times and only says "done" when the board itself reports the
new number or setting.
"""
import sys
import time

import serial

TRIES = 3
K_PIN, K_BRIGHT, K_TEST, K_TRY = 1, 2, 3, 4      # keys (docs/PROTOCOL-3.md, tw_proto.h TW_K_*)


def reports(f):
    """a TWX line's report entries (2.5.1+: target:key:flags:generation:seconds:value hex, flag 1):
    [(board, key, generation, value bytes)]"""
    out = []
    for e in f[4:]:
        p = e.strip().split(":")
        if len(p) == 6 and p[2].isdigit() and int(p[2]) & 1:
            out.append((int(p[0]), int(p[1]), int(p[3]), bytes.fromhex(p[5])))
    return out
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
    -> {board: dict(set_seq, power_q, relay, heard, cable, fw)}. fw: what the board says it runs
    (tw_node 2.2+: TWV, every 5 s; None if it did not say in time)"""
    seen, fw, led, keys, net = {}, {}, {}, {}, {}
    for line in p.lines(seconds):
        f = line.split(",")
        try:
            if f[0] == "TWX":                       # 2.5.1+: report entries: a board's own keys
                for b, k, g, v in reports(f):
                    keys.setdefault(b, {})[k] = (g, v)
                for e in f[4:]:                     # 2.6+: its network keys, said to every board
                    q = e.strip().split(":")        # (flags 0; 3.0: 2, its own standing value)
                    if len(q) == 6 and q[0] == "0" and q[2] in ("0", "2") and q[1].isdigit() and \
                            16 <= int(q[1]) <= 20:
                        net.setdefault(int(f[1]), {})[int(q[1])] = int(q[5][:2], 16) if q[5] else 0
                continue
            if f[0] == "TWV" and len(f) >= 6:
                fw[int(f[1])] = f"{f[4]} (built {f[5]})"
                if len(f) >= 10:                    # 2.5+: its LED pin, brightness, generations
                    led[int(f[1])] = tuple(int(x) for x in f[6:10])
                continue
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
    for k, kv in keys.items():                      # the reports are the newer word on the LED
        if K_PIN in kv and K_BRIGHT in kv:
            led[k] = (kv[K_PIN][1][0] if kv[K_PIN][1] else 0, kv[K_BRIGHT][1][0] if kv[K_BRIGHT][1] else 32,
                      kv[K_PIN][0], kv[K_BRIGHT][0])
    for k, b in seen.items():
        b["net"] = tuple(net[k][x] for x in range(16, 21)) if len(net.get(k, {})) == 5 else None
        b["keys"] = keys.get(k, {})
        b["led"] = led.get(k)
        b["fw"] = fw.get(k)
        b["fw_known"] = bool(fw)        # someone said: a board that did not is older (or missed)
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
        two_ways = not (b.get("fw") or "").startswith("tw_node 3")    # 3.0: the transport is the only way
        if two_ways and b.get("net") and b.get("flags") is not None and b["power_q"]:
            f = b["flags"]
            hdr = (b["power_q"], 0 if f & 0x01 else 1, 2 if f & SECOND_FORCE else 1 if f & SECOND_OFF else 0,
                   1 if f & MEASURE else 0, f >> CAP_SHIFT)
            names = ("power", "relay", "second", "measure-second", "cap")
            diff = [f"{n} {t} (header {h})" for n, t, h in zip(names, b["net"], hdr) if t != h]
            relay += ", transport agrees" if not diff else ", TRANSPORT DIFFERS: " + "; ".join(diff)
        if b.get("led"):
            pin, br = b["led"][:2]
            relay += f", LED on GPIO {pin} (brightness {br})" if pin else ", LED off"
        if b.get("fw"):
            relay = f", firmware {b['fw']}" + relay
        elif b.get("fw_known"):
            relay = ", firmware not reported (2.1 or older)" + relay
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
            # 3.0: each board says its settings every 5 s (the others' come from that): 11 s shows them all
            print("each board takes it within a second or two; what they report now:")
            show(status(p, 11) or seen)
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
    if not a or len(a) > 8:
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

    if len(a) >= 2 and a[1] == "led":
        rest, target = a[2:], None
        if len(rest) >= 2 and rest[-2] == "board":          # 2.5: for board N, or all, over the air
            t = rest[-1]
            if t == "all":
                target = 0
            elif t.isdigit() and 1 <= int(t) <= 16:
                target = int(t)
            else:
                sys.exit("board N (1-16) or board all")
            rest = rest[:-2]
        if len(rest) == 1 and rest[0] in ("off", "on", "test"):
            cmd = f"TWSET led {rest[0]}"
        elif len(rest) == 2 and rest[0] == "on" and rest[1].isdigit():
            cmd = f"TWSET led on {int(rest[1])}"
        elif len(rest) == 2 and rest[0] == "bright" and rest[1].isdigit() and 1 <= int(rest[1]) <= 255:
            cmd = f"TWSET led bright {int(rest[1])}"
        elif len(rest) == 4 and rest[0] == "try" and all(x.isdigit() and int(x) <= 255 for x in rest[1:]):
            cmd = "TWSET led try " + " ".join(str(int(x)) for x in rest[1:])
        else:
            sys.exit("led on [GPIO] | led off | led bright 1-255 | led test | led try R G B (0-255), "
                     "then board N|all for others")
        if target is None:
            for attempt in range(1, TRIES + 1):
                p.send(cmd)
                for line in p.lines(4):
                    if line.startswith(("TWOK led", "TWERR")):
                        print(line)
                        if line.startswith("TWERR"):
                            sys.exit(1)
                        return
                print(f"  no answer yet (try {attempt} of {TRIES})")
            sys.exit("no answer: is it running tw_node 2.4 or newer, and is this its native USB port?")
        key = {"on": K_PIN, "off": K_PIN, "bright": K_BRIGHT, "test": K_TEST, "try": K_TRY}[rest[0]]
        print("listening for each board's report (every 5 s) ...")
        st = status(p, 11)
        def gen_of(b):                              # 2.5.1 reports; 2.5's TWV for the LED settings
            if key in st[b].get("keys", {}):
                return st[b]["keys"][key][0]
            if st[b].get("led") and key in (K_PIN, K_BRIGHT):
                return st[b]["led"][2 if key == K_PIN else 3]
            return None
        known = {b: gen_of(b) for b in st if gen_of(b) is not None}
        targets = sorted(known) if target == 0 else [target]
        if target and target not in known:
            print(f"  board {target} did not report this in 11 s (not on tw_node 2.5.1, or not heard now); "
                  "sending anyway")
        gens = [known[b] for b in targets if b in known]
        gen = (max(gens, default=0) + 1) % 256 or 1     # 3.0: 0 is "never given"
        what = {K_PIN: "LED pin", K_BRIGHT: "LED brightness", K_TEST: "LED test", K_TRY: "LED try"}[key]
        pin_key = key == K_PIN
        done = set()
        for attempt in range(1, TRIES + 1):
            p.send(f"{cmd} board {target} gen {gen}")
            for line in p.lines(3):
                if line.startswith(("TWOK led", "TWERR")):
                    print(line)
                    if line.startswith("TWERR"):
                        sys.exit(1)
                    break
            # the board(s) say they took it in their reports (2.5.1: within a second or two; through
            # relays longer; 2.5: TWV every 5 s)
            for line in p.lines(15):
                f = line.split(",")
                if f[0] == "TWX":
                    for b, k, g, v in reports(f):
                        if b in targets and k == key and g == gen:
                            done.add(b)
                elif f[0] == "TWV" and len(f) >= 10 and f[1].isdigit() and int(f[1]) in targets and \
                        key in (K_PIN, K_BRIGHT) and int(f[8 if pin_key else 9]) == gen:
                    done.add(int(f[1]))
                if done >= set(targets):
                    break
            if done >= set(targets):
                print(f"done: {what} {'run' if key in (K_TEST, K_TRY) else 'taken'} by board{'s' if len(done) > 1 else ''} "
                      f"{', '.join(str(b) for b in sorted(done))} (generation {gen})")
                return
            print(f"  taken so far by {sorted(done) or 'none'} (try {attempt} of {TRIES})")
        missing = sorted(set(targets) - done)
        sys.exit(f"not confirmed by board {', '.join(str(b) for b in missing)}: off, out of reach, or not on "
                 "tw_node 2.5; it is passed on for 60 s, so try again when it is up")

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
            psram = ""
            if len(f) >= 7 and f[6].strip().isdigit():          # 2.2+: PSRAM found, bytes
                n = int(f[6])
                psram = f", PSRAM {n / 2**20:g} MB" if n else ", no PSRAM"
            led = ""
            if len(f) >= 9 and f[7].strip().isdigit():          # 2.4+: the LED's pin (0 = off), brightness
                led = f", LED on GPIO {f[7]} (brightness {f[8].strip()})" if int(f[7]) else ", LED off"
            print(f"board {f[1]}, factory address {f[2]}, firmware {f[3]}{psram}{led}")
            print("every board:")
            show(status(p, 11))         # 11 s: every board says its firmware and reports every 5 s, and a
                                        # relayed one can come a few seconds later
            return
    sys.exit("no answer: is it running tw_node, and is this its native USB port? (it says hello every 10 s)")


if __name__ == "__main__":
    main()
