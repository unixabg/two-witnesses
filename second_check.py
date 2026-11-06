#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
"""second_check.py - how the second frame in each board's slot did on a run
(tw_node 2.1, docs/SECOND-FRAME.md), from the run's own files.

    ~/scratch-esp/bin/python second_check.py RUN_FOLDER
    e.g. ~/scratch-esp/bin/python second_check.py ~/tw-data/runs/2026-11-07_0000

Says, for each board the cabled board hears:
  - how many of its frames announced a second frame, and how many of those the
    cabled board heard (can a board hear two frames about 1 ms apart?);
  - how many of the second frames gave a CSI measurement (does the radio
    measure both?);
  - the gap from its normal frame to its second frame, as heard here (no
    clock comes into it), and where it says its second frame ended in its slot
    by its own clock (the limit is 3200 us: 200 us into the next board's guard);
  - the cabled board's measurements of it that the radio flagged (rx_state).
For the cabled board itself: second frames sent, skipped for lack of time,
failed, and where they ended (only in "second force": an exit sends none
otherwise).
With "measure-second on": how the CSI of each second frame compares with the
normal frame 1 ms before it (RSSI, and shape in dB), next to the same between
two normal frames one period apart. If the first is no worse, the second
frame measures as well as the first.

Writes RUN_FOLDER/second_check.txt too. Runs from older firmware have none of
these reports; it says so.
"""
import argparse
import collections
import glob
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import analyze_csi as A  # noqa: E402  (read_lines: reads gzip cut short by a power cut)
import split_links as S  # noqa: E402  (parse_twr)

LIMIT_US = 3200             # TW_SLOT_US + TW_SLOT_SLACK_US
SECOND_OFF, SECOND_FORCE, MEASURE, CAP_SHIFT = 0x02, 0x04, 0x08, 4


def flags_text(f):
    second = "force (test)" if f & SECOND_FORCE else "off" if f & SECOND_OFF else "on"
    t = f"second frame {second}"
    if f & MEASURE:
        t += ", measure-second on (test)"
    if f >> CAP_SHIFT:
        t += f", cap {f >> CAP_SHIFT} (test)"
    return t


def amps(r):
    return np.frombuffer(r["q"], dtype=np.uint8).astype(np.float32) * (r["step"] / 4.0)


def shape_db(a, b):
    """how far apart two amplitude shapes are, dB (RMS over the bins live in both; the overall
    level of each taken out first)"""
    n = min(len(a), len(b))
    a, b = a[:n], b[:n]
    live = (a > 0.5) & (b > 0.5)
    if live.sum() < 8:
        return None
    x, y = a[live] / a[live].mean(), b[live] / b[live].mean()
    return float(np.sqrt(np.mean((20 * np.log10(x / y)) ** 2)))


def q(v, p):
    return float(np.percentile(v, p)) if len(v) else float("nan")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder")
    a = ap.parse_args()
    files = sorted(glob.glob(os.path.join(a.folder, "csi-*.csv.gz")))
    if not files:
        sys.exit(f"no csi-*.csv.gz in {a.folder}")

    tot = collections.defaultdict(collections.Counter)      # sender -> counts heard by the cabled board
    gaps = collections.defaultdict(list)                     # sender -> gap us, per second (longest)
    ends = collections.defaultdict(list)                     # sender -> its own end us, per second
    flags_seen = collections.OrderedDict()                   # flags -> first time seen
    cable, fw = None, set()
    twf = []                                                 # the cabled board's TWF dicts
    t0 = t1 = None
    # measure-second: pairs (second frame vs the normal frame 1 ms before), and the baseline
    # (two normal frames one period apart)
    recent = collections.defaultdict(dict)                   # (rx, boot, tx) -> {txseq: (rssi, amps)}
    pend = collections.defaultdict(dict)                     # same key -> {txseq: (rssi, amps)} extra
    pair = collections.defaultdict(lambda: ([], []))         # link -> (rssi diffs, shape dB)
    base = collections.defaultdict(lambda: ([], []))
    n_extra_rec = 0

    for path in files:
        for line in A.read_lines(path):
            f = line.rstrip("\n").split(",")
            if len(f) < 3:
                continue
            try:
                t = int(f[0]) / 1e9
            except ValueError:
                continue
            t0 = t if t0 is None else t0
            t1 = t
            kind = f[1]
            try:
                if kind == "TWN" and len(f) >= 22:
                    s = int(f[2])
                    c = tot[s]
                    c["seconds"] += 1
                    c["frames"] += int(f[15])
                    c["announced"] += int(f[16])
                    c["heard"] += int(f[17])
                    c["csi"] += int(f[18])
                    c["rx_err"] += int(f[21])
                    if int(f[17]):
                        gaps[s].append(int(f[19]))
                    if int(f[20]):
                        ends[s].append(int(f[20]))
                    flags_seen.setdefault(int(f[14]), t)
                elif kind == "TWS" and len(f) >= 27:
                    cable = int(f[2])
                    flags_seen.setdefault(int(f[24]), t)
                elif kind == "TWF":
                    d = dict(x.split("=", 1) for x in f[3:] if "=" in x)
                    if "extra_sent" in d:
                        twf.append((t, {k: int(v) for k, v in d.items()}))
                elif kind == "TWH" and len(f) >= 5:
                    fw.add(f[4])
                elif kind == "TWR":
                    r = S.parse_twr(f[1:])
                    if r is None:
                        continue
                    key = (r["rx"], r["boot"], r["tx"])
                    if r["kind"] & 0x20:
                        n_extra_rec += 1
                        pend[key][r["txseq"]] = (r["rssi"], amps(r))
                    else:
                        rec = recent[key]
                        here = (r["rssi"], amps(r))
                        prev = rec.get(r["txseq"] - 1)
                        link = f"{r['tx']}-{r['rx']}"
                        if prev is not None:
                            d = shape_db(prev[1], here[1])
                            if d is not None:
                                base[link][0].append(here[0] - prev[0])
                                base[link][1].append(d)
                        rec[r["txseq"]] = here
                        if len(rec) > 64:
                            for k in sorted(rec)[:-48]:
                                del rec[k]
                    # match second-frame records with their normal frame (either may come first)
                    for k in list(pend[key]):
                        normal = recent[key].get(k)
                        if normal is not None:
                            ex = pend[key].pop(k)
                            d = shape_db(normal[1], ex[1])
                            if d is not None:
                                pair[f"{key[2]}-{key[0]}"][0].append(ex[0] - normal[0])
                                pair[f"{key[2]}-{key[0]}"][1].append(d)
                        elif len(pend[key]) > 64:
                            pend[key].pop(k)
            except (ValueError, IndexError):
                continue

    L = [f"run {os.path.abspath(a.folder)}",
         f"  {(t1 - t0) / 60 if t0 else 0:.1f} minutes; firmware {', '.join(sorted(fw)) or 'not reported'}; "
         f"cabled board {cable if cable else '?'}"]
    if not tot and not twf:
        L.append("  no second-frame reports: the boards run firmware older than tw_node 2.1")
        print("\n".join(L))
        return
    L.append("  settings seen: " + "; ".join(f"{flags_text(fl)}" for fl in flags_seen))
    L += ["", "heard by the cabled board, per board (totals over the run):",
          "  board  frames  announced  second heard   with CSI   gap after normal (median / max)   "
          "own end in slot (median / max)   rx_state flagged"]
    for s in sorted(tot):
        c = tot[s]
        ann = c["announced"]
        heard = f"{c['heard']:,} ({100 * c['heard'] / ann:.1f}%)" if ann else f"{c['heard']:,}"
        csi = f"{c['csi']:,} ({100 * c['csi'] / c['heard']:.1f}%)" if c["heard"] else "0"
        g = f"{q(gaps[s], 50):.0f} / {max(gaps[s])} us" if gaps[s] else "-"
        e = f"{q(ends[s], 50):.0f} / {max(ends[s])} us" if ends[s] else "-"
        late = sum(1 for x in ends[s] if x > LIMIT_US)
        e += f" ({late} s over {LIMIT_US})" if late else ""
        L.append(f"  {s:>5}  {c['frames']:7,d}  {ann:9,d}  {heard:>15}  {csi:>14}   {g:>30}   {e:>30}   "
                 f"{c['rx_err']:,d}")
    if twf:
        first, last = twf[0][1], twf[-1][1]
        sent = last["extra_sent"] - first["extra_sent"]
        if sent < 0:                       # it restarted during the run: count from the restart
            sent = last["extra_sent"]
        endmax = max(d["extra_end_us"] for _, d in twf)
        L += ["", f"the cabled board's own: second frames sent {sent:,}, skipped (no time) "
                  f"{last['extra_skipped'] - first['extra_skipped']:,}, failed {last['extra_err'] - first['extra_err']:,}"
                  + (f", latest end {endmax} us into its slot (limit {LIMIT_US})" if endmax else "")
                  + f"; rx_state-flagged measurements {last.get('rx_err', 0) - first.get('rx_err', 0):,}"]
    if pair:
        L += ["", f"measured second frames ({n_extra_rec:,} records): second frame vs the normal frame 1 ms "
                  "before, against two normal frames one period apart",
              "  link    pairs   RSSI diff (median, spread)   shape dB (median / 90%)     "
              "normal-normal: RSSI spread   shape dB (median / 90%)"]
        for link in sorted(pair, key=lambda k: [int(x) for x in k.split("-")]):
            pr, pd = pair[link]
            br, bd = base.get(link, ([], []))
            L.append(f"  {link:>5}  {len(pd):6,d}   {q(pr, 50):+5.1f} dB, {q(pr, 75) - q(pr, 25):4.1f} dB IQR"
                     f"        {q(pd, 50):5.2f} / {q(pd, 90):5.2f}              "
                     f"{q(br, 75) - q(br, 25):4.1f} dB IQR           {q(bd, 50):5.2f} / {q(bd, 90):5.2f}")
    elif n_extra_rec:
        L += ["", f"{n_extra_rec:,} records from second frames, but none matched their normal frame"]
    L += ["", "announced = normal frames heard here saying a second frame follows; second heard = of those,",
          "the second frames that arrived too; with CSI = of those, the ones the radio gave a measurement",
          "for (measured, or only counted when measure-second is off). gap = from",
          "the normal frame to the second, as heard here; own end = where the sender says its second",
          "frame ended, by its own clock (20 us steps)."]
    text = "\n".join(L) + "\n"
    print(text)
    out = os.path.join(a.folder, "second_check.txt")
    open(out, "w").write(text)
    print(f"saved {out}")


if __name__ == "__main__":
    main()
