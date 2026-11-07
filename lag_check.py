#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
"""lag_check.py - how far behind the timekeeper's clock the other boards run,
from a run's own files (docs/TIMING-2.3.md). No firmware change needed.

    ~/scratch-esp/bin/python lag_check.py RUN_FOLDER
    e.g. ~/scratch-esp/bin/python lag_check.py ~/tw-data/runs/2026-11-08_0000

Every record carries the measuring board's network clock when it heard the
frame (net_us). So:
  - the timekeeper's records of board k say where k's frames really arrive
    in k's slot (us after the slot start; the slot target is 400);
  - board k's records of the timekeeper say where k *thinks* the
    timekeeper's frames arrive in the timekeeper's slot;
  - the cabled board's TWF (2.2+) says where its own frames' send-done
    comes in its slot (tx_at + tx_radio). When the cabled board is the
    timekeeper, that is about where its frames really arrive (a receive
    callback comes about as late as a send-done).
So, per board k:
    lag (A) = timekeeper's send-done - where k thinks the timekeeper arrives
    lag (B) = where k's frames really arrive - timekeeper's send-done
              (if k's own delays are like the timekeeper's; air times differ
              by up to a few hundred us with frame length)
    lag (C) = half of (where k's frames really arrive - where k thinks the
              timekeeper arrives): needs no TWF, so it works on any run (k's
              delays like the timekeeper's, as in B)
Also, for the cabled board: how long after the radio's own receive time
(the record's rx_us, the rx_ctrl timestamp) its CSI callback ran (the
record's net_us, less the board's clock offset from TWS). That is the part
of the lag on the receiving side, if the two clocks are the same one. The
real boards gave 1.1-1.5 ms for all three (2026-11-07). Normal frames'
measurements only. Writes RUN_FOLDER/lag_check.txt too.
"""
import argparse
import collections
import glob
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import analyze_csi as A  # noqa: E402  (read_lines)
import split_links as S  # noqa: E402  (parse_twr)

SLOT_US = 3000


def pos(net_us, period_us, slot_of):
    """where net_us falls in board slot_of's slot, us after its start (as the firmware works it
    out: 32-bit clock, mod the period); in -period/2 .. period/2"""
    raw = ((slot_of - 1) * SLOT_US) % period_us
    d = ((net_us % (1 << 32)) % period_us + period_us - raw) % period_us
    return d - period_us if d >= period_us // 2 else d


def stats(v):
    v = np.asarray(v, dtype=float)
    return np.median(v), np.percentile(v, 25), np.percentile(v, 75), len(v)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder")
    a = ap.parse_args()
    files = sorted(glob.glob(os.path.join(a.folder, "csi-*.csv.gz")))
    if not files:
        sys.exit(f"no csi-*.csv.gz in {a.folder}")
    period = collections.Counter()           # period ms seen in TWS
    leader = collections.Counter()           # timekeeper seen in TWS
    cable = None
    arrive = collections.defaultdict(list)   # (rx, tx) -> positions of tx's frames in tx's slot, rx's clock
    sd = []                                  # cabled board: tx_at + tx_radio, per TWF (means)
    offset = None                            # the cabled board's clock offset (TWS), latest
    cbd = collections.defaultdict(list)      # sender -> cabled board's callback delay after rx_us
    for path in files:
        for line in A.read_lines(path):
            f = line.rstrip("\n").split(",")
            if len(f) < 3:
                continue
            k = f[1]
            try:
                if k == "TWS" and len(f) >= 8:
                    cable = int(f[2])
                    leader[int(f[4])] += 1
                    period[int(f[7])] += 1
                    offset = int(f[9])
                elif k == "TWF":
                    d = dict(x.split("=", 1) for x in f[3:] if "=" in x)
                    if "tx_at" in d and "tx_radio" in d:
                        at, rad = int(d["tx_at"].split("/")[0]), int(d["tx_radio"].split("/")[0])
                        if at and rad:
                            sd.append(at + rad)
                elif k == "TWR":
                    r = S.parse_twr(f[1:])
                    if r is None or (r["kind"] & 0x0F) > 1 or r["kind"] & 0x20:
                        continue
                    arrive[(r["rx"], r["tx"])].append(r["net_us"])
                    if r["rx"] == cable and offset is not None:
                        d = (r["net_us"] - offset - r["rx_us"]) % (1 << 32)
                        cbd[r["tx"]].append(d - (1 << 32) if d >= (1 << 31) else d)
            except (ValueError, IndexError):
                continue
    if not period or not leader:
        sys.exit("no status lines (TWS) in this run")
    p_ms, lead = period.most_common(1)[0][0], leader.most_common(1)[0][0]
    p = p_ms * 1000
    L = [f"run {os.path.abspath(a.folder)}",
         f"  timekeeper board {lead}, cabled board {cable}, period {p_ms} ms"]
    if len(period) > 1 or len(leader) > 1:
        L.append(f"  (also seen: periods {dict(period)}, timekeepers {dict(leader)}; the most common is used)")
    t_sd = None
    if sd and cable == lead:
        t_sd = float(np.median(sd))
        L.append(f"  timekeeper's own frames: send-done {t_sd:.0f} us into its slot "
                 f"(median of {len(sd)} TWF lines: tx_at + tx_radio)")
    elif sd:
        L.append("  the cabled board is not the timekeeper: its send-done does not say where the "
                 "timekeeper's frames arrive (lag A and B need it)")
    else:
        L.append("  no 2.2 timing in TWF: lag A and B need it (firmware before 2.2)")
    L += ["", "  board   its frames arrive, by the timekeeper's clock   the timekeeper's frames arrive, "
              "by its clock        lag A    lag B    lag C",
          "          (median, middle half, records)                  (median, middle half, records)"]
    boards = sorted({tx for (rx, tx) in arrive if rx == lead} | {rx for (rx, tx) in arrive if tx == lead})
    for b in boards:
        if b == lead:
            continue
        real = [pos(v, p, b) for v in arrive.get((lead, b), [])]
        think = [pos(v, p, lead) for v in arrive.get((b, lead), [])]
        c1 = "{:6.0f} us ({:5.0f} .. {:5.0f}) {:8,d}".format(*stats(real)) if real else "-"
        c2 = "{:6.0f} us ({:5.0f} .. {:5.0f}) {:8,d}".format(*stats(think)) if think else "-"
        la = f"{t_sd - np.median(think):6.0f}" if (t_sd is not None and think) else "     -"
        lb = f"{np.median(real) - t_sd:6.0f}" if (t_sd is not None and real) else "     -"
        lc = f"{(np.median(real) - np.median(think)) / 2:6.0f}" if (real and think) else "     -"
        L.append(f"  {b:>5}   {c1:>44s}   {c2:>44s}   {la}   {lb}   {lc}")
    if cbd:
        L += ["", f"  board {cable}'s CSI callback after the radio's receive time (rx_us), per sender:"]
        for b in sorted(cbd):
            v = np.asarray(cbd[b], dtype=float)
            L.append(f"    from board {b}: median {np.median(v):6.0f} us, middle half {np.percentile(v, 25):6.0f} .. "
                     f"{np.percentile(v, 75):6.0f}, 1% / 99% {np.percentile(v, 1):6.0f} / {np.percentile(v, 99):7.0f} "
                     f"({len(v):,d} records)")
        L.append("    (about the frame's air time plus a little: rx_us marks the frame's start, and the radio's")
        L.append("    timestamp could take the callback's place in the clock. Wildly scattered: not the same clock.)")
    L += ["", "lag = how far the board's clock runs behind the timekeeper's, us (2.2 boards: 1.1-1.5 ms).",
          "A, B and C should roughly agree; B and C also hold the difference in frame lengths. A board's slot",
          "target is 400 us: where its frames really arrive, less the air and radio time, says how",
          "late they go out."]
    text = "\n".join(L) + "\n"
    print(text)
    out = os.path.join(a.folder, "lag_check.txt")
    open(out, "w").write(text)
    print(f"saved {out}")


if __name__ == "__main__":
    main()
