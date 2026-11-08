#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
"""net_check.py - do the network settings and reports agree both ways?
(tw_node 2.6 and 2.7, docs/PROTOCOL-3.md steps 1 and 2), from a run's own files.

    ~/scratch-esp/bin/python net_check.py RUN_FOLDER

In 2.6 the network settings (power, relay, second frame, measure-second, cap)
travel in every frame's header, as before, and also on the transport: every
board says them every 5 s (TWX lines: entries for all boards, keys 16-20).
For each board this compares what it says on the transport with what its own
frames say in the header (TWS for the board on the cable, TWN for the others,
as heard by it), at each of its announcements. It also reads the cabled
board's own count of seconds the two disagreed (TWF net_diff).

A few disagreements right after a change are the two ways arriving a moment
apart; lasting ones are not.

2.7: each board's uptime and where its last second frame ended travel in its
once-a-second record (TWM) as well as in its frames' header (TWN, as the
cabled board heard them): compared too. Writes RUN_FOLDER/net_check.txt too.

3.0 (protocol 3): the header carries none of these any more; the transport and
the records are their only way (TWN's fields are filled from them), so there
is nothing to compare both ways. Instead each board's announcements are
compared with the cabled board's latest: do all boards have the same settings?
"""
import numpy as np
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
    up_hdr = {}                                     # board -> (time, uptime) from TWN, latest
    up_n, up_bad = collections.Counter(), collections.Counter()
    xe_hdr, xe_rec = collections.defaultdict(list), collections.defaultdict(list)
    cable = None
    t0 = t1 = None
    proto3 = False                                  # a board says it runs tw_node 3.x (TWV)
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
                    up_hdr[int(f[2])] = (t, int(f[8]))
                    if len(f) >= 21 and int(f[20]):
                        xe_hdr[int(f[2])].append(int(f[20]))
                elif kind == "TWM" and len(f) >= 24:      # 2.7: uptime and second-frame end by record
                    b = int(f[2])
                    if int(f[23]):
                        xe_rec[b].append(int(f[23]))
                    if b in up_hdr:
                        th, uh = up_hdr[b]
                        up_n[b] += 1
                        up_bad[b] += abs(int(f[5]) - (uh + (t - th))) > 2
                elif kind == "TWV" and len(f) >= 6 and f[5].startswith("tw_node 3"):
                    proto3 = True
                elif kind == "TWF":
                    d = dict(x.split("=", 1) for x in f[3:] if "=" in x)
                    if "net_diff" in d:
                        twf_diff, twf_now = int(d["net_diff"]), int(d.get("net_diff_now", 0))
                elif kind == "TWX":
                    b = int(f[2])
                    vals = {}
                    for e in f[5:]:
                        p = e.split(":")
                        # 2.6: flags 0; 3.0: 2 (standing: the board's own)
                        if len(p) == 6 and p[0] == "0" and p[2] in ("0", "2") and p[1].isdigit() and \
                                16 <= int(p[1]) <= 20:
                            vals[int(p[1])] = int(p[5][:2], 16) if p[5] else 0
                    if len(vals) == 5 and proto3:       # 3.0: against the cabled board's own
                        tr = tuple(vals[k] for k in range(16, 21))
                        last[b] = tr
                        if cable is not None and cable in last:
                            t0 = t if t0 is None else t0
                            t1 = t
                            seen[b] += 1
                            if tr != last[cable]:
                                differ[b] += 1
                                first_diff.setdefault(b, (t, last[cable], tr))
                    elif len(vals) == 5 and b in header and header[b][0]:
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
    if proto3:
        L += ["  protocol 3 (tw_node 3.x): the settings travel only on the transport; each board's against the",
              "  cabled board's", "", "  board   announcements compared   differed   settings (last)"]
        for b in sorted(seen):
            L.append(f"  {b:>5}   {seen[b]:>22,}   {differ[b]:>8,}   {text(last[b])}")
        for b, (t, cb, tr) in sorted(first_diff.items()):
            L.append(f"  board {b}, first difference {t - t0:.0f} s in: cabled board {text(cb)}; board {b} {text(tr)}")
        L += ["", "A difference or two right after a change is the change spreading (about a second a hop);",
              "lasting ones are not."]
        out = "\n".join(L) + "\n"
        print(out)
        open(os.path.join(a.folder, "net_check.txt"), "w").write(out)
        print(f"saved {os.path.join(a.folder, 'net_check.txt')}")
        return
    if up_n:
        L += ["", "  reports both ways (2.7): uptime in each board's record (TWM) against its header (TWN),",
              "  and where its second frames ended (median / max, us; header: as the cabled board heard it)",
              "  board   uptimes compared   off by over 2 s   second frame's end: header        record"]
        for b in sorted(up_n):
            def mm(v):
                return f"{np.median(v):6.0f} / {max(v):5d}" if v else "      -       "
            L.append(f"  {b:>5}   {up_n[b]:>16,}   {up_bad[b]:>15,}   {mm(xe_hdr[b]):>24}   {mm(xe_rec[b]):>14}")
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
