#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
"""net_check.py - do the network settings agree both ways? (tw_node 2.6,
docs/PROTOCOL-3.md step 1), from a run's own files.

    ~/scratch-esp/bin/python net_check.py RUN_FOLDER

In 2.6 the network settings (power, relay, second frame, measure-second, cap)
travel in every frame's header, as before, and also on the transport: every
board says them every 5 s (TWX lines: entries for all boards, keys 16-20).
For each board this compares what it says on the transport with what its own
frames say in the header (TWS for the board on the cable, TWN for the others,
as heard by it), at each of its announcements. It also reads the cabled
board's own count of seconds the two disagreed (TWF net_diff).

A few disagreements right after a change are the two ways arriving a moment
apart; lasting ones are not. Writes RUN_FOLDER/net_check.txt too.
"""
import argparse
import collections
import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import analyze_csi as A  # noqa: E402  (read_lines)

NAMES = ("power", "relay", "second", "measure-second", "cap")
RELAY_OFF, SECOND_OFF, SECOND_FORCE, MEASURE, CAP_SHIFT = 0x01, 0x02, 0x04, 0x08, 4


def from_header(power_q, flags):
    """the header's settings as the transport's five keys"""
    return (power_q, 0 if flags & RELAY_OFF else 1,
            2 if flags & SECOND_FORCE else 1 if flags & SECOND_OFF else 0,
            1 if flags & MEASURE else 0, flags >> CAP_SHIFT)


def text(v):
    second = {0: "on", 1: "off", 2: "force"}.get(v[2], str(v[2]))
    return (f"power {v[0] / 4:.2f} dBm, relay {'on' if v[1] else 'off'}, second {second}, "
            f"measure-second {'on' if v[3] else 'off'}, cap {v[4] or 'off'}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder")
    a = ap.parse_args()
    files = sorted(glob.glob(os.path.join(a.folder, "csi-*.csv.gz")))
    if not files:
        sys.exit(f"no csi-*.csv.gz in {a.folder}")
    header = {}                                     # board -> (power_q, flags), latest
    seen = collections.Counter()                    # board -> announcements compared
    differ = collections.Counter()
    first_diff = {}                                 # board -> (time, header, transport)
    last = {}                                       # board -> transport values, latest
    twf_diff = twf_now = None
    cable = None
    t0 = t1 = None
    for path in files:
        for line in A.read_lines(path):
            f = line.rstrip("\n").split(",")
            if len(f) < 3:
                continue
            try:
                t = int(f[0]) / 1e9
                kind = f[1]
                if kind == "TWS" and len(f) >= 25:
                    cable = int(f[2])
                    header[cable] = (int(f[19]), int(f[24]))
                elif kind == "TWN" and len(f) >= 15:
                    header[int(f[2])] = (int(f[11]), int(f[14]))
                elif kind == "TWF":
                    d = dict(x.split("=", 1) for x in f[3:] if "=" in x)
                    if "net_diff" in d:
                        twf_diff, twf_now = int(d["net_diff"]), int(d.get("net_diff_now", 0))
                elif kind == "TWX":
                    b = int(f[2])
                    vals = {}
                    for e in f[5:]:
                        p = e.split(":")
                        if len(p) == 6 and p[0] == "0" and p[2] == "0" and p[1].isdigit() and 16 <= int(p[1]) <= 20:
                            vals[int(p[1])] = int(p[5][:2], 16) if p[5] else 0
                    if len(vals) == 5 and b in header and header[b][0]:
                        t0 = t if t0 is None else t0
                        t1 = t
                        tr = tuple(vals[k] for k in range(16, 21))
                        hd = from_header(*header[b])
                        last[b] = tr
                        seen[b] += 1
                        if tr != hd:
                            differ[b] += 1
                            first_diff.setdefault(b, (t, hd, tr))
            except (ValueError, IndexError):
                continue
    L = [f"run {os.path.abspath(a.folder)}"]
    if not seen:
        L.append("  no announcements of network settings on the transport: the boards run firmware older "
                 "than tw_node 2.6")
        print("\n".join(L))
        return
    L.append(f"  {(t1 - t0) / 60:.1f} minutes of announcements; cabled board {cable}")
    if twf_diff is not None:
        L.append(f"  the cabled board counted {twf_diff:,} s when its two ways disagreed"
                 + (f" (disagreeing at the end: keys {twf_now:#04x})" if twf_now else ""))
    L += ["", "  board   announcements compared   disagreed   settings on the transport (last)"]
    for b in sorted(seen):
        L.append(f"  {b:>5}   {seen[b]:>22,}   {differ[b]:>9,}   {text(last[b])}")
    for b, (t, hd, tr) in sorted(first_diff.items()):
        L.append(f"  board {b}, first disagreement {t - t0:.0f} s in: header {text(hd)}; transport {text(tr)}")
    L += ["", "Each board's own announcements, every 5 s, against the header of its frames heard just before.",
          "A disagreement or two right after a change is the two ways arriving a moment apart."]
    out = "\n".join(L) + "\n"
    print(out)
    open(os.path.join(a.folder, "net_check.txt"), "w").write(out)
    print(f"saved {os.path.join(a.folder, 'net_check.txt')}")


if __name__ == "__main__":
    main()
