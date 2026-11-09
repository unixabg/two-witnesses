#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
"""tw_setid.py - talk to the board on the computer's USB port (tw_node firmware).

    ~/scratch-esp/bin/python tw_setid.py PORT                 who is it; each board's power and relaying
    ~/scratch-esp/bin/python tw_setid.py PORT board N         give it board number N (1-16), once
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
    ~/scratch-esp/bin/python tw_setid.py PORT channel N       the radio channel for every board: 1-11 (2.4 GHz,
                                                          US; tw_node 3.2, every board); "channel" alone shows it
    ~/scratch-esp/bin/python tw_setid.py PORT group N         the group of the board on the cable: 0-255 (0: the
                                                          default; tw_node 3.3). Over the cable only; it restarts
    ~/scratch-esp/bin/python tw_setid.py PORT survey          every board listens on channels 1-11 for a
                                                          second each: who else is on the air (tw_node 3.1)
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

The channel (tw_node 3.2): every board takes the new channel within a second
or two and moves 20 s later, all together; this waits and names any board not
heard on the new channel. A board that missed it (off, or out of reach) looks
for the others after 30 s of hearing nobody, channel by channel, and stays
where it finds them. A new channel changes every link's readings: a run (and
the board's own quiet level) should start after it, and nights compare only
on the same channel and power. Every board must run 3.2: an older one cannot
follow and would be left behind, so this refuses.

Groups (tw_node 3.3): arrays of boards near each other keep to themselves.
Each board's group is set over its own cable, like its number (over the air a
board would leave the group that carried the command); the board restarts in
it and forgets the network settings' generations, so it takes its new group's
settings within a few seconds, looking for them channel by channel if the
group is on another channel. Group 0 is the default and the group of every
board before 3.3; only 3.3 boards keep a group apart (older ones would join
any). Two groups on one channel share its air: give each its own (survey, then
channel N on each group's cable). Board numbers are per group (each group has
its own board 1).

The channel survey (tw_node 3.1): every board at once leaves its channel 5 s
after the command, listens about a second on each of channels 1-11 (2.4 GHz,
US) and comes back: about 12 s with the network paused, so not during a run.
Each reports how busy each channel was with OTHER traffic (our boards are
silent then, and left out if not), how many foreign frames and how loud, and
the noise floor. This prints a table, every board's busy share per channel,
and ranks the 40 MHz channel pairs the boards could use by their worst board
(all boards share one channel). It is saved in $TW_DATA/surveys/. Busy counts
only traffic the radio can decode: Bluetooth or a microwave oven show only in
the noise floor.

The board's USB line can drop a command or an answer when it is busy, so this
tries up to three times and only says "done" when the board itself reports the
new number or setting.
"""
import os
import sys
import time

import serial

TRIES = 3
K_PIN, K_BRIGHT, K_TEST, K_TRY, K_SURVEY = 1, 2, 3, 4, 5  # keys (docs/PROTOCOL-3.md, tw_proto.h TW_K_*)
SURVEY_CHANNELS = {"2.4 GHz": range(1, 12)}      # 3.1: what a survey covers (US); a band adds a row
OUR_PAIR = (11, 7)                               # the firmware's channel: 11, its 40 MHz partner below
CHANNELS = {"2.4 GHz (US)": range(1, 12)}        # 3.2: what "channel N" accepts; a band adds a row


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
                                       flags=int(f[23]) if len(f) >= 24 else None,     # 2.1+
                                       chan=int(f[26]) if len(f) >= 29 else None,      # 3.2: its radio's,
                                       chan_key=int(f[27]) if len(f) >= 29 else None,  # the network's,
                                       looking=int(f[28]) if len(f) >= 29 else 0,      # looking for others
                                       group=int(f[31]) if len(f) >= 34 else None,     # 3.3: its group,
                                       other=int(f[32]) if len(f) >= 34 else 0,        # other groups' frames
                                       other_last=int(f[33]) if len(f) >= 34 else None)
            elif f[0] == "TWN" and len(f) >= 11:
                seen[int(f[1])] = dict(cable=False, set_seq=int(f[9]), power_q=int(f[10]),
                                       relay=int(f[11]) if len(f) >= 12 else None,
                                       heard=int(f[12]) if len(f) >= 13 else None,
                                       flags=int(f[13]) if len(f) >= 14 else None,     # 2.1+
                                       chan=int(f[21]) if len(f) >= 22 and int(f[21]) else None)   # 3.2
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
        if b.get("group") is not None:              # 3.3 (the board on the cable; the others share it)
            relay += f", group {b['group']}"
            if b.get("other"):
                relay += f" (heard {b['other']:,} frames of another group since it started, last group {b['other_last']})"
        if b.get("chan"):                           # 3.2
            relay += f", channel {b['chan']}"
            if b["cable"] and b.get("chan_key") and b["chan_key"] != b["chan"]:
                relay += f" (moving to {b['chan_key']})"
            if b.get("looking"):
                relay += " (LOOKING FOR THE OTHERS)"
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


def fw_version(fw):
    """'tw_node 3.2 (built ...)' -> (3, 2); None if not said"""
    try:
        return tuple(int(x) for x in fw.split()[1].split(".")[:2])
    except (AttributeError, IndexError, ValueError):
        return None


def channel(p, arg):
    """3.2: show the channel, or set it for every board and check they all moved"""
    valid = [c for band in CHANNELS.values() for c in band]
    st = status(p, 11)
    if not st:
        sys.exit("no boards heard: is tw_node running (and nothing else using the port)?")
    if arg is None:
        show(st)
        return
    if not arg.isdigit() or int(arg) not in valid:
        sys.exit(f"channel must be one of {valid[0]}-{valid[-1]} (2.4 GHz, US)")
    ch = int(arg)
    old = sorted(b for b, v in st.items() if (fw_version(v.get("fw")) or (0, 0)) < (3, 2))
    if old:
        sys.exit(f"{'boards' if len(old) > 1 else 'board'} {', '.join(map(str, old))} "
                 f"{'do' if len(old) > 1 else 'does'} not run tw_node 3.2 (or did not say): an older board cannot "
                 "follow a new channel and would be left behind. Update every board first.")
    cable = [b for b in st.values() if b["cable"]]
    if cable and cable[0].get("chan") == ch and cable[0].get("chan_key") == ch:
        print(f"already on channel {ch}")
        show(st)
        return
    for attempt in range(1, TRIES + 1):
        p.send(f"TWSET channel {ch}")
        ok = False
        for line in p.lines(3):
            if line.startswith("TWERR"):
                sys.exit(line)
            if line.startswith("TWOK channel"):
                print(line)
                ok = True
                break
        if ok:
            break
        print(f"  no answer yet (try {attempt} of {TRIES})")
    else:
        sys.exit("the board did not take the command: is it tw_node 3.2?")
    print("every board takes it within a second or two, and they move together in 20 s; waiting ...")
    for line in p.lines(26):
        pass
    now = status(p, 11)
    show(now)
    here = sorted(b for b, v in now.items() if v.get("chan") == ch or (not v["cable"] and v.get("chan") == ch))
    missing = sorted(set(st) - set(here))
    if not missing:
        print(f"done: every board ({', '.join(map(str, here))}) is on channel {ch}")
    else:
        print(f"not heard on channel {ch} yet: board{'s' if len(missing) > 1 else ''} {', '.join(map(str, missing))}. "
              "A board that missed it looks for the others after 30 s of hearing nobody (channel by channel); "
              "check again in a minute with: tw_setid.py PORT channel")


def survey_parse(f):
    """a TWU line (3.1): (board, generation, {channel: (busy %, frames, loudest, mean, noise)})"""
    out = {}
    for e in f[5:]:
        q = e.strip().split(":")
        if len(q) == 6:
            num = lambda x: int(x) if x.lstrip("-").isdigit() else None
            out[int(q[0])] = (float(q[1]), int(q[2]), num(q[3]), num(q[4]), num(q[5]))
    return int(f[1]), int(f[4]), out


def pairs_2g4():
    """the 40 MHz channels in 2.4 GHz, US (1-11): a main channel and its partner 4 above or below,
    as one block of two 20 MHz halves: (low, high)"""
    return [(c, c + 4) for c in range(1, 8)]


def block_busy(ch, lo, hi):
    """how busy a 40 MHz block (channels lo and hi, 2.4 GHz: 5 MHz apart, 20 MHz wide) is for one
    board: the busiest channel overlapping it, by the share of that channel's 20 MHz inside the block
    (a network halfway in counts half). Not a sum: one network is heard on its neighbors too"""
    worst = 0.0
    for c, v in ch.items():
        over = min(5 * c + 10, 5 * hi + 10) - max(5 * c - 10, 5 * lo - 10)
        if over > 0 and v[0] is not None:
            worst = max(worst, v[0] * min(over, 20) / 20)
    return worst


def survey_report(res, when):
    """the survey's table, the pairs ranked by their worst board; -> text"""
    boards = sorted(res)
    chans = [c for band in SURVEY_CHANNELS.values() for c in band]
    L = [f"channel survey {when}: busy % (other traffic's share of the air) per board, foreign frames in "
         "brackets", "", "  channel " + "".join(f"   board {b:<6}" for b in boards)]
    for c in chans:
        row = f"  {c:>7} "
        for b in boards:
            busy, fr, _, _, _ = res[b].get(c, (None, 0, None, None, None))
            row += f"   {busy:5.1f} ({fr:>4})" if busy is not None else "       -      "
        L.append(row)
    L += ["", "  loudest foreign signal / noise floor (dBm), per board:"]
    for c in chans:
        row = f"  {c:>7} "
        for b in boards:
            _, fr, mx, _, nf = res[b].get(c, (None, 0, None, None, None))
            row += f"   {mx if mx is not None else '':>4} / {nf if nf is not None else '':<4}"
        L.append(row)
    ranked = []
    for lo, hi in pairs_2g4():
        per = {b: block_busy(res[b], lo, hi) for b in boards}
        ours = {lo, hi} == set(OUR_PAIR)
        ranked.append((round(max(per.values()), 1), round(sum(per.values()), 1), not ours, lo, hi, per))
    ranked.sort(key=lambda x: x[:4])               # a tie: the channel we are on first (no change for nothing)
    L += ["", "  40 MHz pairs (main channel and its partner), by the busiest board; a pair's busy: the busiest",
          "  channel overlapping its 40 MHz, by how much of it overlaps (in 2.4 GHz every pair overlaps some):"]
    for worst, tot, _, lo, hi, per in ranked:
        ours = "   <- now" if {lo, hi} == set(OUR_PAIR) else ""
        L.append(f"    {lo:>2} + {hi:<2}  worst {worst:5.1f}%   " + "  ".join(f"board {b} {per[b]:.1f}" for b in boards)
                 + ours)
    best = ranked[0]
    head = "already on the cleanest" if not best[2] else "cleanest for every board"
    L += ["", f"  {head}: {best[3]} + {best[4]} (worst board {best[0]:.1f}% busy). "
          "All boards share one channel; changing it comes later (tw_node 3.2).",
          "  busy = decodable Wi-Fi only: Bluetooth or a microwave oven show only as a higher noise floor."]
    return "\n".join(L) + "\n"


def survey(p):
    """3.1: every board surveys the channels at once; wait for each board's result"""
    st = status(p, 11)
    if not st:
        sys.exit("no boards heard: is tw_node running (and nothing else using the port)?")
    old = sorted(b for b, v in st.items() if v.get("fw") and not v["fw"].startswith(("tw_node 3.1", "tw_node 3.2",
                                                                                     "tw_node 3.3", "tw_node 4")))
    if old:
        print(f"  board{'s' if len(old) > 1 else ''} {', '.join(map(str, old))} run older firmware: "
              "they keep sending and do not survey (their frames are left out of the others' counts)")
    gens = [v["keys"][K_SURVEY][0] for v in st.values() if K_SURVEY in v.get("keys", {})]
    gen = (max(gens, default=0) + 1) % 256 or 1
    want = sorted(b for b in st if b not in old)
    for attempt in range(1, TRIES + 1):
        p.send(f"TWSET survey gen {gen}")
        ok = False
        for line in p.lines(3):
            if line.startswith(("TWOK survey", "TWERR")):
                print(line)
                if line.startswith("TWERR"):
                    sys.exit(1)
                ok = True
                break
        if ok:
            break
        print(f"  no answer yet (try {attempt} of {TRIES})")
    else:
        sys.exit("the board did not take the command: is it tw_node 3.1?")
    print("the boards leave the channel in 5 s for about 12 s, then report (up to 40 s) ...")
    res, t_end = {}, time.time() + 40
    for line in p.lines(40):
        f = line.split(",")
        if f[0] == "TWU" and len(f) >= 6:
            try:
                b, g, ch = survey_parse(f)
            except (ValueError, IndexError):
                continue
            if g == gen and b not in res:
                res[b] = ch
                print(f"  board {b} reported")
        if res and set(want) <= set(res):
            break
    if not res:
        sys.exit("no board reported a survey (are they on tw_node 3.1?)")
    missing = sorted(set(want) - set(res))
    if missing:
        print(f"  no result from board {', '.join(map(str, missing))} (out of reach, or older firmware)")
    when = time.strftime("%Y-%m-%d %H:%M")
    text = survey_report(res, when)
    print("\n" + text)
    folder = os.path.join(os.environ.get("TW_DATA", os.path.expanduser("~/tw-data")), "surveys")
    try:
        os.makedirs(folder, exist_ok=True)
        path = os.path.join(folder, time.strftime("%Y-%m-%d_%H%M") + ".txt")
        open(path, "w").write(text)
        print(f"saved {path}")
    except OSError as e:
        print(f"(not saved: {e})")


def main():
    a = sys.argv[1:]
    if not a or len(a) > 8:
        sys.exit(__doc__)
    words = ("survey", "channel", "power", "relay", "second", "measure-second", "cap", "led", "group", "board")
    if len(a) > 1 and a[1] not in words:
        sys.exit(__doc__)                   # e.g. the older "PORT N": the help shows "PORT board N"
    if len(a) > 1 and a[1] == "board" and (len(a) != 3 or not a[2].isdigit()):
        sys.exit(__doc__)
    if len(a) == 3 and a[1] == "board" and not 1 <= int(a[2]) <= 16:
        sys.exit("board number must be 1..16")
    p = Port(a[0])

    if len(a) == 2 and a[1] == "survey":
        survey(p)
        return

    if len(a) in (2, 3) and a[1] == "channel":
        channel(p, a[2] if len(a) == 3 else None)
        return

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

    if len(a) == 3 and a[1] == "group":
        if not a[2].isdigit() or not 0 <= int(a[2]) <= 255:
            sys.exit("group must be 0..255 (0: the default)")
        g = int(a[2])
        for attempt in range(1, TRIES + 1):
            p.send(f"TWSET group {g}")
            if attempt == 1:
                print(f"asked the board on {p.path} to join group {g} (it restarts) ...")
            for line in p.lines(12):
                if line.startswith(("TWOK", "TWERR")):
                    print(line)
                    if line.startswith("TWERR"):
                        sys.exit(1)
                if line.startswith("TWH"):
                    f = line.split(",")
                    if len(f) >= 10 and f[9].strip() == str(g):
                        print(line)
                        print(f"done: board {f[1]} is in group {g} (firmware {f[3]}); it takes the group's "
                              "settings within a few seconds, or looks for the group if it is on another channel")
                        return
                    if len(f) < 10:
                        sys.exit(f"this board runs {f[3]}: groups need tw_node 3.3")
                    break
            print(f"  no confirmation yet (try {attempt} of {TRIES})")
        sys.exit("no confirmation: is it running tw_node 3.3, and is this its native USB port?")

    if len(a) == 3 and a[1] == "board":     # the board number, like the other settings: board N
        try:
            n = int(a[2])
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

    if len(a) > 1:                          # a known word with the wrong values: the help
        sys.exit(__doc__)
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
            group = f", group {f[9].strip()}" if len(f) >= 10 and f[9].strip().isdigit() else ""    # 3.3+
            print(f"board {f[1]}, factory address {f[2]}, firmware {f[3]}{psram}{led}{group}")
            print("every board:")
            show(status(p, 11))         # 11 s: every board says its firmware and reports every 5 s, and a
                                        # relayed one can come a few seconds later
            return
    sys.exit("no answer: is it running tw_node, and is this its native USB port? (it says hello every 10 s)")


if __name__ == "__main__":
    main()
