#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
"""score_check.py - does the boards' own movement score (tw_node 2.2+) follow the
computer's? From a run's own files.

    ~/scratch-esp/bin/python score_check.py RUN_FOLDER
    e.g. ~/scratch-esp/bin/python score_check.py ~/tw-data/runs/2026-11-08_0000

Each board scores, once a second, every board it hears (TWM lines): how much the
link's shape fluctuated within the second against the link's quiet level, as the
computer's "change" (replay.py, dash.py). This computes the computer's change for
the same seconds from the run's records (bands of 4 live bins, the record's own
level out, each band's spread per second against its median over the run) and
compares, per direction:
  - how closely they follow each other (rank correlation, 1 = in step);
  - the loud seconds (3x quiet or more): loud for both, for the computer only, for
    the board only (a second either side counts as the same second: the board's
    seconds do not start where the computer's do).
Differences to expect: the board's quiet level is a running median that follows
lasting changes over 10-15 minutes, the computer's here is the whole run's (so
after a car arrives they part, until the board has settled); the board starts
scoring 30 s after it starts hearing a board.

Writes RUN_FOLDER/score_check.txt too.
"""
import argparse
import collections
import glob
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import analyze_csi as A  # noqa: E402
import split_links as S  # noqa: E402

BAND, LEARN, LOUD = 4, 50, 3.0
WRAP = 2 ** 32


class Unwrap:
    """the timekeeper's clock (32-bit us, wraps every 71 min) made continuous with the host time"""
    def __init__(self):
        self.off = None

    def __call__(self, host_ns, net_us):
        host_us = host_ns // 1000
        if self.off is None:
            self.off = host_us - net_us
        k = round((host_us - self.off - net_us) / WRAP)
        return net_us + k * WRAP


class Link:
    """the computer's change for one direction, second by second"""
    def __init__(self):
        self.n = None
        self.cnt = None
        self.learned = 0
        self.bins = None
        self.open = {}                  # second -> [s1, s2, n]
        self.std = {}                   # second -> band spreads

    def add(self, sec, q, step):
        if self.n != len(q):
            self.__init__()
            self.n = len(q)
            self.cnt = np.zeros(len(q))
        if self.bins is None:
            self.cnt += q > 0
            self.learned += 1
            if self.learned >= LEARN:
                live = np.flatnonzero(2 * self.cnt > LEARN)
                nb = len(live) // BAND
                if nb:
                    self.bins = live[: nb * BAND].reshape(nb, BAND)
                else:
                    self.__init__()
            return
        db = 20 * np.log10(q[self.bins].astype(np.float64) * (step / 4.0) + 0.5).mean(axis=1)
        v = db - db.mean()
        o = self.open.get(sec)
        if o is None:
            o = self.open[sec] = [np.zeros(len(v)), np.zeros(len(v)), 0]
            for s in [s for s in self.open if s < sec - 10]:      # late records: keep 10 s open
                self.close(s)
        o[0] += v
        o[1] += v * v
        o[2] += 1

    def close(self, s):
        s1, s2, n = self.open.pop(s)
        if n >= 5:
            m = s1 / n
            self.std[s] = np.sqrt(np.maximum(s2 / n - m * m, 0))

    def change(self):
        for s in list(self.open):
            self.close(s)
        if not self.std:
            return {}
        secs = sorted(self.std)
        m = np.array([self.std[s] for s in secs])
        ref = np.maximum(np.median(m, axis=0), 1e-3)
        c = (m / ref).mean(axis=1)
        return dict(zip(secs, c))


def ranks(x):
    r = np.empty(len(x))
    r[np.argsort(x, kind="stable")] = np.arange(len(x))
    return r


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder")
    a = ap.parse_args()
    files = sorted(glob.glob(os.path.join(a.folder, "csi-*.csv.gz")))
    if not files:
        sys.exit(f"no csi-*.csv.gz in {a.folder}")
    unwrap = Unwrap()
    links = collections.defaultdict(Link)
    board = collections.defaultdict(dict)          # link -> {second: score}
    seen = set()
    for path in files:
        for line in A.read_lines(path):
            f = line.rstrip("\n").split(",")
            if len(f) < 3:
                continue
            try:
                if f[1] == "TWM" and len(f) >= 23:
                    key = (int(f[2]), int(f[3]), int(f[4]))
                    if key in seen:
                        continue
                    seen.add(key)
                    net = unwrap(int(f[0]), int(f[6]))
                    sec = (net - 500_000) // 1_000_000      # the second it closed covers ~ the one before
                    for tx, v in enumerate(f[7:23], 1):
                        if v != "0":
                            board[f"{tx}-{f[2]}"][sec] = int(v) / 10
                elif f[1] == "TWR":
                    r = S.parse_twr(f[1:])
                    if r is None or (r["kind"] & 0x0F) > 1 or r["kind"] & 0x20:
                        continue                            # not a measurement, or a second frame's
                    key = (r["rx"], r["boot"], r["recseq"])
                    if key in seen:
                        continue
                    seen.add(key)
                    sec = unwrap(int(f[0]), r["net_us"]) // 1_000_000
                    links[f"{r['tx']}-{r['rx']}"].add(sec, np.frombuffer(r["q"], np.uint8), r["step"])
            except (ValueError, IndexError):
                continue
    L = [f"run {os.path.abspath(a.folder)}"]
    if not board:
        L.append("  no movement scores (TWM): the boards run firmware older than tw_node 2.2")
        print("\n".join(L))
        return
    L += ["", f"per direction: the board's score against the computer's change (loud = {LOUD:g}x quiet or more)",
          "  link   seconds   in step (rank corr.)   median board / computer   loud: both   computer only   board only"]
    for link in sorted(board, key=lambda k: [int(x) for x in k.split("-")]):
        comp = links[link].change() if link in links else {}
        secs = sorted(s for s in board[link] if s in comp)
        if len(secs) < 30:
            L.append(f"  {link:>5}   {len(secs):7d}   (too few seconds in common)")
            continue
        b = np.array([board[link][s] for s in secs])
        c = np.array([comp[s] for s in secs])
        rho = np.corrcoef(ranks(b), ranks(c))[0, 1]
        bl = {s for s in secs if board[link][s] >= LOUD}
        cl = {s for s in secs if comp[s] >= LOUD}
        near = lambda s, other: any(x in other for x in (s - 1, s, s + 1))
        both = sum(1 for s in cl if near(s, bl))
        conly = len(cl) - both
        bonly = sum(1 for s in bl if not near(s, cl))
        L.append(f"  {link:>5}   {len(secs):7,d}   {rho:20.2f}   {np.median(b):12.2f} / {np.median(c):.2f}"
                 f"   {both:10,d}   {conly:13,d}   {bonly:10,d}")
    L += ["", "in step: 1 = they rise and fall together, 0 = unrelated. A quiet link has little to follow,",
          "so a low figure there means little; the loud seconds are the test."]
    text = "\n".join(L) + "\n"
    print(text)
    out = os.path.join(a.folder, "score_check.txt")
    open(out, "w").write(text)
    print(f"saved {out}")


if __name__ == "__main__":
    main()
