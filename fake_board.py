#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
"""fake_board.py - a pretend receiver, for testing the tools without hardware.
Its lines have the same shape as the real board's, and its planted signals
are known, so a tool that misses them (or finds things that are not there)
is caught at the desk instead of in the garage.

Live, through a fake serial port (for listen.py / record_csi.py):
    python3 fake_board.py live
  -> prints the port name (e.g. /dev/pts/5); runs until Ctrl-C. Planted:
     a 7 s gap at 100 s, a 30 s walk at 200 s, 40 s of 70% loss at 300 s.

A whole pretend run written straight to gzip files (for view.py):
    python3 fake_board.py run RUN_FOLDER --start "2026-10-24 22:00" --hours 3 [--blind --seed N]
  The files go straight into RUN_FOLDER, as listen.py does; the plan of
  planted signals is written to RUN_FOLDER/PLANTED.txt (open it only after
  reading the view).

Habits of the fake: 90 packets/s, about 7% lost at random, RSSI -67 dBm with
sd about 0.15 dB, a gentle 0.23 Hz compressor-like ripple always present,
64 subcarriers of which 52 are live.

Planted kinds: walk (someone crosses), breath (a slow small rhythm), gap
(sender silent), step (RSSI moves), loss (a stretch where most packets go
missing, as when car 2 was opened on 2026-10-26: the picture must be marked
LOW DATA there, never called movement).
"""
import argparse
import datetime as dt
import gzip
import os
import sys
import time

import numpy as np

RATE = 90.0
NSC = 64
LIVE = np.r_[np.zeros(6), np.ones(26), np.zeros(1), np.ones(26), np.zeros(5)].astype(bool)
assert LIVE.size == NSC and LIVE.sum() == 52


def default_plan(t0):
    """Planted signals, as (kind, start unix, end unix, parameter)."""
    h = 3600
    return [
        ("loss", t0 + 0.40 * h, t0 + 0.40 * h + 600, 0.7),      # 10 min with 70% of packets missing
        ("walk", t0 + 0.69 * h, t0 + 0.69 * h + 90, 0.25),     # someone crosses: big broadband change
        ("breath", t0 + 1.17 * h, t0 + 1.83 * h, 0.27),        # slow rhythm 0.27 Hz, small
        ("gap", t0 + 2.50 * h, t0 + 2.50 * h + 8, 0),          # sender quiet 8 s
        ("step", t0 + 2.75 * h, t0 + 3.0 * h, -1.0),           # RSSI drops 1 dB (a door?)
    ]


class Board:
    def __init__(self, t0, plan, seed=1):
        self.rng = np.random.default_rng(seed)
        self.base = np.where(LIVE, 20 + 15 * self.rng.random(NSC), 0.0)
        self.phase = self.rng.random(NSC) * 2 * np.pi
        self.walk_state = np.zeros(NSC)
        self.t0, self.plan = t0, plan
        self.pid = 0
        self.us0 = int(self.rng.integers(1e6, 4e9))

    def lines(self, t_unix):
        """Lines for packets at the given times (array of unix seconds)."""
        out = []
        for t in t_unix:
            self.pid += 1
            if self.rng.random() < 0.07:
                continue
            on = [p for p in self.plan if p[1] <= t < p[2]]
            if any(p[0] == "gap" for p in on):
                continue
            if any(p[0] == "loss" and self.rng.random() < p[3] for p in on):
                continue
            amp = self.base * (1 + 0.004 * np.sin(2 * np.pi * 0.23 * t + self.phase)
                               + 0.03 * self.rng.standard_normal(NSC))
            rssi = -67.0 + 0.15 * self.rng.standard_normal()
            for kind, _a, _b, par in on:
                if kind == "walk":
                    self.walk_state += par * self.rng.standard_normal(NSC) * 0.3
                    self.walk_state *= 0.97
                    amp = amp * (1 + self.walk_state)
                    rssi += 3 * self.walk_state.mean()
                elif kind == "breath":
                    amp = amp * (1 + 0.012 * np.sin(2 * np.pi * par * t + self.phase))
                elif kind == "step":
                    rssi += par
            amp = np.abs(amp)
            ph = self.rng.random(NSC) * 2 * np.pi
            iq = np.empty(2 * NSC, dtype=int)
            iq[0::2] = np.round(amp * np.cos(ph))
            iq[1::2] = np.round(amp * np.sin(ph))
            us = (self.us0 + int((t - self.t0) * 1e6)) % 2 ** 32
            data = ",".join(map(str, iq))
            line = (f"CSI_DATA,{self.pid},1a:00:00:00:00:00,{int(round(rssi))},11,1,0,1,0,1,0,0,0,0,"
                    f"-92,0,11,1,{us},0,40,0,{2 * NSC},0,\"[{data}]\"")
            out.append((t, line))
        return out


def live():
    import pty
    master, slave = pty.openpty()
    print(os.ttyname(slave), flush=True)
    t0 = time.time()
    board = Board(t0, [("gap", t0 + 100, t0 + 107, 0), ("walk", t0 + 200, t0 + 230, 0.25),
                       ("loss", t0 + 300, t0 + 340, 0.7)])
    os.set_blocking(master, False)      # like a real UART: unread lines are lost, not queued up
    nxt = t0
    while True:
        nxt += 0.1
        ts = nxt - 0.1 + np.arange(int(RATE / 10)) / RATE
        payload = "".join(l + "\r\n" for _t, l in board.lines(ts))
        if payload:
            try:
                os.write(master, payload.encode())
            except (BlockingIOError, OSError):
                pass
        if time.time() - nxt > 1:
            nxt = time.time()
        time.sleep(max(0, nxt - time.time()))


def random_plan(t0, hours, rng):
    """Same kinds as default_plan, at random times and rates (for blind tests)."""
    span = hours * 3600
    plan = []
    for _ in range(rng.integers(1, 4)):
        a = t0 + rng.uniform(0.05, 0.95) * span
        plan.append(("walk", a, a + rng.uniform(30, 150), 0.25))
    a = t0 + rng.uniform(0.05, 0.6) * span
    plan.append(("breath", a, a + rng.uniform(0.3, 0.6) * 3600, round(rng.uniform(0.15, 0.45), 3)))
    if rng.random() < 0.7:
        a = t0 + rng.uniform(0.05, 0.95) * span
        plan.append(("gap", a, a + rng.uniform(6, 60), 0))
    if rng.random() < 0.6:
        a = t0 + rng.uniform(0.05, 0.9) * span
        plan.append(("loss", a, a + rng.uniform(180, 1200), round(rng.uniform(0.5, 0.8), 2)))
    return sorted(plan, key=lambda p: p[1])


def run(outdir, start, hours, seed, blind=False):
    t0 = dt.datetime.strptime(start, "%Y-%m-%d %H:%M").timestamp()
    plan = random_plan(t0, hours, np.random.default_rng(seed + 1000)) if blind else default_plan(t0)
    board = Board(t0, plan, seed)
    folder = outdir
    os.makedirs(folder, exist_ok=True)
    t_end = t0 + hours * 3600
    t = t0
    f = key = None
    while t < t_end:
        hour = dt.datetime.fromtimestamp(t).replace(minute=0, second=0, microsecond=0)
        if hour != key:
            if f:
                f.close()
            key = hour
            path = os.path.join(folder, f"csi-{hour:%Y%m%d-%H}.csv.gz")
            f = gzip.open(path, "wt", compresslevel=1)
            f.write(f"# name=csi-{hour:%Y%m%d-%H} port=FAKE baud=0 start_unix={t:.3f}\n")
            print(path, flush=True)
        ts = t + np.arange(int(RATE * 10)) / RATE
        for tt, line in board.lines(ts):
            jitter = board.rng.random() * 0.004
            f.write(f"{int((tt + jitter) * 1e9)},{line}\n")
        t += 10
    f.close()
    with open(os.path.join(outdir, "PLANTED.txt"), "w") as p:
        for kind, a, b, par in plan:
            p.write(f"{kind:7s} {dt.datetime.fromtimestamp(a):%H:%M:%S} - "
                    f"{dt.datetime.fromtimestamp(b):%H:%M:%S}  param {par}\n")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["live", "run"])
    ap.add_argument("outdir", nargs="?")
    ap.add_argument("--start", default="2026-10-24 22:00")
    ap.add_argument("--hours", type=float, default=3)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--blind", action="store_true", help="random plan; see PLANTED.txt only afterwards")
    a = ap.parse_args()
    if a.mode == "live":
        live()
    else:
        if not a.outdir:
            sys.exit("run mode needs RUN_FOLDER")
        run(a.outdir, a.start, a.hours, a.seed, a.blind)
