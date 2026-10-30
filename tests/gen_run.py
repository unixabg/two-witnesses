#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
"""Synthetic 3-board tw_node recording with known features, for testing split_links/view/links_report.

Truth:
  all links 3% random loss, links 1-3 and 3-1 27%
  00:25:00-00:25:20 (t=1500-1520): every link loses 90%  (whole channel)
  t=3000-3600: link 1-3 loses 68% (one link)
  t=2400-2460: movement on 1-2 and 2-1, then their RSSI +6 dB and stays
  t>=1800: 1.1733 Hz (x1 and x2) on every link
  all the time: 4.3067 Hz on link 1-2 only
  delivery: board 3's reports 15% lost all the time; t=1000-1200 board 2's reports 65% lost
  t=3900-3960: link 2-3 path loss 50% (nothing else changes)
Relayed records (measured by boards 2, 3) reach the computer 20-40 ms late; lines are stamped
one by one, so arrival order differs from measurement order, as on the real Chromebook.
"""
import gzip
import os
import sys

import numpy as np

out_dir = sys.argv[1]
T = int(sys.argv[2]) if len(sys.argv) > 2 else 4500
t_start = 1793260800.0          # 2026-10-28 ... any clock time
rng = np.random.default_rng(7)
PLAIN = os.environ.get("PLAIN") == "1"     # noise only: no rhythms, events or dropouts
N = 192
FPS = 50
links = [(1, 2), (1, 3), (2, 1), (2, 3), (3, 1), (3, 2)]
base = {l: 20 + 40 * rng.random(N) for l in links}
rssi0 = {(1, 2): -61, (2, 1): -61, (1, 3): -59, (3, 1): -59, (2, 3): -53, (3, 2): -53}
recseq = {1: 0, 2: 0, 3: 0}
lines = []
for tx in (1, 2, 3):
    nfr = T * FPS
    tt = np.arange(nfr) / FPS + (tx - 1) * 0.003
    for rx in (1, 2, 3):
        if rx == tx:
            continue
        l = (tx, rx)
        p = np.full(nfr, 0.27 if l in ((1, 3), (3, 1)) else 0.03)
        if not PLAIN:
            p[(tt >= 1500) & (tt < 1520)] = 0.90
        if l == (1, 3) and not PLAIN:
            p[(tt >= 3000) & (tt < 3600)] = 0.68
        if l == (2, 3) and not PLAIN:
            p[(tt >= 3900) & (tt < 3960)] = 0.50
        keep = rng.random(nfr) >= p
        idx = np.flatnonzero(keep)
        t = tt[idx]
        mod = 1 + 0.03 * rng.standard_normal((len(idx), N))
        on = 0 if PLAIN else 1
        mod *= (1 + on * 0.02 * np.sin(2 * np.pi * 1.1733 * t) * (t >= 1800)
                + on * 0.012 * np.sin(2 * np.pi * 2.3467 * t + 0.4) * (t >= 1800))[:, None]
        if l == (1, 2) and not PLAIN:
            mod *= (1 + 0.025 * np.sin(2 * np.pi * 4.3067 * t))[:, None]
        if l in ((1, 2), (2, 1)) and not PLAIN:
            ev = (t >= 2400) & (t < 2460)
            mod[ev] *= 1 + 0.3 * rng.standard_normal((ev.sum(), N))
        amp = np.clip(base[l][None, :] * mod, 0, 63.75)
        q = np.round(amp * 4).astype(np.uint8)
        q[:, :6] = 0
        rssi = rssi0[l] + np.where((l in ((1, 2), (2, 1))) & (t >= 2460) & (not PLAIN), 6, 0) + rng.integers(-1, 2, len(idx))
        delay = (0.0002 if rx == 1 else 0.020 * rng.integers(1, 3, len(idx))) + 0.002 * rng.random(len(idx))
        lines.append((t, delay, rx, tx, idx, rssi, q))

recs = []
for t, delay, rx, tx, idx, rssi, q in lines:
    for k in range(len(idx)):
        recs.append((t[k], rx, tx, int(idx[k]), int(rssi[k]), q[k], t[k] + (delay if np.isscalar(delay) else delay[k])))
recs.sort(key=lambda r: (r[0], r[1]))                 # measurement order: give recseq
rows = []
for m, rx, tx, txseq, rssi, q, arr in recs:
    recseq[rx] += 1
    if not PLAIN:
        dl = 0.15 if rx == 3 else (0.65 if rx == 2 and 1000 <= m < 1200 else 0.0)
        if rng.random() < dl:
            continue                                    # made, but the report never arrived
    rows.append((arr, rx, tx, txseq, recseq[rx], rssi, q, m))
rows.sort(key=lambda r: r[0])
os.makedirs(out_dir, exist_ok=True)
fh = None
hour = None
last_ns = 0
for i, (arr, rx, tx, txseq, rs, rssi, q, m) in enumerate(rows):
    ns = int((t_start + arr) * 1e9)
    ns = max(ns, last_ns + 1000)                        # stamped one line at a time
    last_ns = ns
    import datetime as dt
    h = dt.datetime.fromtimestamp(ns / 1e9).strftime("%Y%m%d-%H")
    if h != hour:
        if fh:
            fh.close()
        hour = h
        fh = gzip.open(os.path.join(out_dir, f"csi-{h}.csv.gz"), "wt", compresslevel=1)
        fh.write(f"# name=csi-{h} synthetic\n")
        for b in (1, 2, 3):
            fh.write(f"{ns},TWH,{b},30:30:f9:00:00:0{b},tw_node 1.0,test,1400\n")
    us = int(m * 1e6) & 0xFFFFFFFF
    fh.write(f"{ns},TWR,{rx},{tx},{txseq},0,{rs},1,{us},{us},{rssi},-92,30,0,73,0,{N},1,{q.tobytes().hex()}\n")
    if i % 2000 == 0:
        fh.write(f"{ns},TWS,1,1,1,0,7,20,0,0,{i},{i},0,{i},0,0,0,0\n")
fh.close()
print(len(rows), "records")
